/*
 * 设置窗口用到的图片资源 (logo / 图标图集 / 支持者素材)。
 *
 * 所有纹理 id 都来自 bongo_cat_ui_texture_* 这一套后端无关的接口, 它内部把 id 记在
 * BONGO_CAT_UI_VK_TEXTURES 纹理表里 —— GL 与 Vulkan 下同一个数字的含义不同, 所以
 * 绝不能把 id 直接交给 GL 或 Vulkan 的原始 API, 释放也必须走 release_id。
 *
 * 另一条不变量: 每张图都有一个"按光栅倍率算出来的像素尺寸" (raster_size)。
 * 图集/封面必须按 raster_scale 加载, 否则高 DPI 下会糊; 而 raster_scale 变了以后
 * 旧纹理不会被自动重载, 得靠 refresh_raster 这条路重新走一遍 assets_load。
 */
#include "preferences_state.h"
#include "ui_icons.h"
#include "bongo_cat/image.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <math.h>

/*
 * 加载单张图片。size 传 0 表示按原图尺寸 (图标图集要按像素格对齐, 不能被缩放)。
 * 失败时要保证 "要么 id 有效、要么是 0", 所以创建成功但上传出错的那条路上必须
 * 显式释放 id —— 这里用的是"先当成功、再回滚"的写法, 免得调用方拿到半个纹理。
 */
static unsigned int load(BongoCatPreferences *value, const char *name,
    int size, int *width, int *height) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), value->app->asset_root,
        name)) return 0;
    BongoCatError error = {0};
    unsigned int texture = (unsigned int)bongo_cat_ui_texture_from_file(&value->ui,
        path, size, size, width, height, &error);
    GLenum upload_error = texture ? GL_NO_ERROR : GL_INVALID_VALUE;
    if (texture && upload_error != GL_NO_ERROR) {
        bongo_cat_ui_texture_release_id(&value->ui, (int)texture);
        texture = 0;
    }
    if (upload_error != GL_NO_ERROR)
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "UI asset upload failed for %s (0x%x)", name,
            (unsigned)upload_error);
    if (!texture && error.message[0])
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
    return texture;
}

/* 光栅尺寸 = 逻辑尺寸 x raster_scale, 夹在 [logical, logical*4] 之间: 上界防显存爆炸 */
static int raster_size(const BongoCatPreferences *value, int logical) {
    float scale = value->ui.raster_scale > 0.0f ? value->ui.raster_scale : 1.0f;
    return SDL_clamp((int)ceilf(logical * scale), logical, logical * 4);
}

void bongo_cat_preferences_assets_load(BongoCatPreferences *value) {
    value->logo_texture = load(value, "bongocat.png", raster_size(value, 192),
        &value->logo_width, &value->logo_height);
    int width = 0, height = 0;
    value->icon_texture = load(value, "ui-symbols.png", 0, &width, &height);
    value->icon_hidpi_attempted = false;
}

/*
 * @4x 图标图集是"按需加载"的: 只在真的需要大图标或高 DPI 时才去读文件, 且只试一次
 * (icon_hidpi_attempted)。用一次性的标志而不是每次判断文件是否存在, 是因为这个函数
 * 是每个图标每个绘制帧都会调的, 不能在里面做文件系统访问。
 */
void bongo_cat_preferences_icon_draw(BongoCatPreferences *value,
    struct nk_command_buffer *canvas, int icon, struct nk_rect bounds,
    struct nk_color color) {
    if (!value || !value->icon_texture || icon < 0 ||
        icon >= BONGO_CAT_UI_ICON_COUNT) return;
    bool large = bounds.w > 24.0f || bounds.h > 24.0f;
    bool needs_hidpi = large || value->ui.raster_scale > 1.05f;
    if (needs_hidpi && !value->icon_hidpi_attempted) {
        int width = 0, height = 0;
        value->icon_hidpi_attempted = true;
        value->icon_texture_hidpi = load(value, "ui-symbols@4x.png", 0,
            &width, &height);
    }
    bool hidpi = value->icon_texture_hidpi && needs_hidpi;
    int cell = hidpi ? 96 : 24;
    int atlas_width = cell * BONGO_CAT_UI_ICON_COUNT;
    unsigned int texture = hidpi ? value->icon_texture_hidpi : value->icon_texture;
    struct nk_image image = nk_subimage_id((int)texture, (nk_ushort)atlas_width,
        (nk_ushort)cell, nk_recti(icon * cell, 0, cell, cell));
    nk_draw_image(canvas, bounds, &image, color);
}

/*
 * 支持者素材是"关于"页专有的重资源, 所以单独一对 load/clear, 由 page_cache_clear
 * 在离开该页时调用。support_assets_loaded 兼作"已尝试过"的标记: 素材缺失时也置真,
 * 否则每次进这一页都会重新去读一遍磁盘。
 */
void bongo_cat_preferences_support_assets_load(BongoCatPreferences *value) {
    if (value->support_assets_loaded) return;
    value->support_assets_loaded = true;
    int image_size = raster_size(value, 192);
    value->catime_texture = load(value, "catime.png", image_size,
        &value->catime_width, &value->catime_height);
    char path[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(path, sizeof(path), value->app->asset_root,
        "vlaina.jpg")) {
        BongoCatError error = {0};
        if (bongo_cat_ui_uses_vulkan(&value->ui))
            value->vlaina_texture = (unsigned int)bongo_cat_ui_texture_from_file(
                &value->ui, path, image_size, image_size, &value->vlaina_width,
                &value->vlaina_height, &error);
        else
            value->vlaina_texture = bongo_cat_image_texture_resampled(path,
                image_size, image_size, (float)raster_size(value, 48),
                &value->vlaina_width, &value->vlaina_height, &error);
        if (!value->vlaina_texture && error.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
    }
}

static void clear_texture(BongoCatPreferences *value, unsigned int *texture) {
    if (*texture) bongo_cat_ui_texture_release_id(&value->ui, (int)*texture);
    *texture = 0;
}

void bongo_cat_preferences_support_assets_clear(BongoCatPreferences *value) {
    if (!value) return;
    clear_texture(value, &value->catime_texture);
    clear_texture(value, &value->vlaina_texture);
    value->support_assets_loaded = false;
}

/*
 * 关窗/销毁时把本窗口持有的全部纹理交还后端。
 * 顺序上没有依赖, 但必须把 support_assets_loaded / icon_hidpi_attempted 一起复位,
 * 否则下次开窗会以为素材还在, 于是画出一堆 id 为 0 的空白图。
 */
void bongo_cat_preferences_assets_clear(BongoCatPreferences *value) {
    if (!value) return;
    clear_texture(value, &value->logo_texture);
    clear_texture(value, &value->icon_texture);
    clear_texture(value, &value->icon_texture_hidpi);
    value->icon_hidpi_attempted = false;
    bongo_cat_preferences_support_assets_clear(value);
}

/*
 * abandon 与 clear 的区别: clear 是在上下文可用时走的正常释放 (会把纹理从
 * BONGO_CAT_UI_VK_TEXTURES 表里摘掉); abandon 用在上下文已经不可用的时候 ——
 * 这时后端马上要被整体销毁, 表也跟着没了, 再逐个 release 反而会去碰失效的驱动对象。
 * 两者都必须把标志位复位, 不能只把 id 清零。
 */
void bongo_cat_preferences_assets_abandon(BongoCatPreferences *value) {
    if (!value) return;
    value->logo_texture = 0;
    value->icon_texture = 0;
    value->icon_texture_hidpi = 0;
    value->catime_texture = 0;
    value->vlaina_texture = 0;
    value->icon_hidpi_attempted = false;
    value->support_assets_loaded = false;
}

