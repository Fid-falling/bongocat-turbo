/*
 * 字体与字形图集的解析/重建。
 *
 * 三条本文件特有的约定:
 * 1. 一张图集装不下所有语言的字, 所以字形范围 (glyph_ranges) 是**按需累积**的:
 * fonts_resolve 把 i18n 需要的范围 + 模型/行为名里出现过的字 + 快捷键箭头合并
 * 成一份表, 交给图集; 导入新模型后要重新解析并重建 (见 model_glyphs 那套 pending);
 * 2. 重建图集很贵 (要重新栅格化几千个字形), 所以有两个不同的入口:
 * reload_fonts 用于"内容变了"(改名/导入/换语言), refresh_raster 用于"DPI 变了";
 * 3. 绘制中途 (ui.frame_building) 绝对不能重建: Nuklear 正握着旧图集的指针。这时
 * 只置 font_reload_pending, 由 present 在帧末补做, 且额外多等一帧 (defer_once),
 * 保证这一帧画完的东西用的还是旧图集。
 */
#include "preferences_fonts.h"

#include "preferences_model_cover.h"
#include "preferences_model_glyphs.h"
#include "ui_font.h"
#include "ui_font_atlas.h"
#include "ui_paint.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/memory_policy.h"

#include <SDL3/SDL.h>
#include <string.h>

/*
 * 解析字体与字形范围 (每次重建前都重新解析, 因为系统字体、语言、模型列表都可能变了)。
 * 正文字体与标题字体各自带"常规 + 回退 + 韩文回退"三档: 一个系统字体覆盖不了中日韩,
 * 韩文单独拎出来是因为它的字形量大且和 CJK 常常不在同一个字体文件里。
 * 标题缺失时直接退回正文, 宁可标题和正文同款, 也不要出现空白标题。
 * 那段"把范围表最后一对改写成 0x2190..0x2193"是为了让快捷键里的方向键箭头一开始就
 * 在图集里 —— 用户还没录任何快捷键时界面也要能显示默认绑定里的箭头。
 */
void bongo_cat_preferences_fonts_resolve(BongoCatPreferences *value,
    BongoCatPreferenceFonts *fonts) {
    memset(fonts, 0, sizeof(*fonts));
    fonts->body = bongo_cat_ui_system_font(fonts->body_path,
        sizeof(fonts->body_path), false);
    fonts->body_fallback = bongo_cat_ui_system_font(fonts->body_fallback_path,
        sizeof(fonts->body_fallback_path), true);
    fonts->body_korean_fallback = bongo_cat_ui_system_korean_font(
        fonts->body_korean_fallback_path,
        sizeof(fonts->body_korean_fallback_path));
    fonts->heading = bongo_cat_ui_system_heading_font(fonts->heading_path,
        sizeof(fonts->heading_path), false);
    fonts->heading_fallback = bongo_cat_ui_system_heading_font(
        fonts->heading_fallback_path,
        sizeof(fonts->heading_fallback_path), true);
    fonts->heading_korean_fallback = bongo_cat_ui_system_korean_heading_font(
        fonts->heading_korean_fallback_path,
        sizeof(fonts->heading_korean_fallback_path));
    if (!fonts->heading) fonts->heading = fonts->body;
    if (!fonts->heading_fallback) fonts->heading_fallback =
        fonts->body_fallback;
    if (!value->app->i18n) return;
    size_t range_capacity = sizeof(value->glyph_ranges) / sizeof(value->glyph_ranges[0]);
    size_t range_count = bongo_cat_i18n_glyph_ranges(value->app->i18n,
        value->glyph_ranges, range_capacity - 2);
    /* Shortcut arrows must be available even before a binding is recorded. */
    if (range_count && range_count + 2 <= range_capacity) {
        value->glyph_ranges[range_count - 1] = 0x2190;
        value->glyph_ranges[range_count] = 0x2193;
        value->glyph_ranges[range_count + 1] = 0;
    }
    /* Include label glyphs in the initial atlas so page switches need no bake. */
    bongo_cat_preferences_model_glyphs(value->app, value->glyph_ranges,
        sizeof(value->glyph_ranges) / sizeof(value->glyph_ranges[0]));
    /*
     * 运行时生成的文字 (自检报告) 里用到的字。glyph_extra 是持久的那一份 ——
     * 每次重建图集上面的 i18n 调用都会重写整张范围表, 所以报告的字不能直接写进
     * glyph_ranges, 只能在最后并进来。
     */
    bongo_cat_preferences_glyphs_include_extra(value, value->glyph_ranges,
        sizeof(value->glyph_ranges) / sizeof(value->glyph_ranges[0]));
    fonts->ranges = value->glyph_ranges;
}

/* 真正重建图集; 成功后要通知内存策略"UI 又占住资源了", 否则空闲回收会算错时机 */
static bool reload(BongoCatPreferences *value,
    const BongoCatPreferenceFonts *fonts, float raster_scale) {
    bool reloaded = bongo_cat_ui_font_atlas_reload(&value->ui, fonts->body,
        fonts->body_fallback, fonts->body_korean_fallback, fonts->heading,
        fonts->heading_fallback, fonts->heading_korean_fallback, fonts->ranges,
        raster_scale);
    if (reloaded) bongo_cat_memory_policy_ui_loaded();
    return reloaded;
}

/*
 * 内容驱动的重建 (改名/导入/换语言都会走这里); 返回 true 表示"请求已受理",
 * 不一定代表这一帧就重建完了 —— 绘制中途会转成 pending 延后。
 */
bool bongo_cat_preferences_reload_fonts(BongoCatPreferences *value) {
    if (!value || !value->ui_initialized) return false;
    if (value->ui.frame_building) {
        value->font_reload_pending = true;
        value->render_dirty = true;
        return true;
    }
    BongoCatPreferenceFonts fonts;
    bongo_cat_preferences_fonts_resolve(value, &fonts);
    bool reloaded = reload(value, &fonts, value->ui.raster_scale);
    if (reloaded) {
        value->font_reload_pending = false;
        bongo_cat_preferences_model_glyphs_clear_pending(value);
    }
    return reloaded;
}

/*
 * DPI 驱动的重建 (pending_raster_scale 由 preferences_scale.c 设置)。
 * 失败时不是放弃而是 1 秒后重试 (raster_retry_ns): 换显示器瞬间 GL 上下文可能暂时
 * 不可用, 直接放弃会让窗口一直用着低分辨率图集。
 * 成功后必须连带作废三样东西 —— paint cache (按旧字号缓存了排版)、封面缓存 (按旧
 * 像素尺寸加载)、UI 素材 (同理) —— 否则界面上会混着两种清晰度的内容。
 */
bool bongo_cat_preferences_refresh_raster(BongoCatPreferences *value) {
    if (!value || value->pending_raster_scale <= 0.0f) return true;
    uint64_t now = SDL_GetTicksNS();
    if (value->raster_retry_ns > now) return false;
    float raster_scale = value->pending_raster_scale;
    BongoCatPreferenceFonts fonts;
    bongo_cat_preferences_fonts_resolve(value, &fonts);
    if (!reload(value, &fonts, raster_scale)) {
        value->raster_retry_ns = now + 1000000000ull;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Preferences font atlas could not be rebuilt for %.2fx",
            raster_scale);
        return false;
    }
    value->pending_raster_scale = 0.0f;
    value->raster_retry_ns = 0;
    bongo_cat_ui_paint_destroy(&value->ui);
    bongo_cat_preferences_model_cover_cache_clear(value->app);
    bongo_cat_preferences_assets_clear(value);
    bongo_cat_preferences_assets_load(value);
    return true;
}
