/*
 * [性能] 区的「纹理质量」下拉 (settings.model.texture_quality)。
 *
 * 为什么单独一行而不是塞在 preferences_pages.c 里: 那个文件已经贴着行数上限, 而这一行
 * 有三档文字 + 一句"对下一个加载的模型生效"的说明, 还要顺手把新上限下发给 media 层。
 *
 * 这一项解决的是"模型自带的艺术字/署名/小图标发糊": 图集上传前会被钳到工作尺寸上限
 * (默认 2048), 而署名这类小图元只有几十个像素, 缩掉之后屏幕上再放大回来就糊了。实测
 * (A-清宵, Vulkan, 337px 窗口): 上限 2048 → 4096 让署名区域的梯度能量 +20%, 4096 → 原生
 * 再 +7%; OpenGL 有 mip 链, 小窗口下几乎无感, 675px 窗口才有 +20%。代价是显存 (8192×4096
 * 的图集 8.4MB → 33.6MB → 134MB), 所以默认必须是标准档。
 */
#include "preferences_internal.h"
#include "preferences_widgets.h"
#include "preferences_controls.h"

#include "bongo_cat/config.h"
#include "bongo_cat/i18n.h"

#include "runtime.h"

/* 与 preferences_pages.c 同一个取词约定 (逐文件一份小静态函数, 不跨文件共享宏) */
static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

void bongo_cat_preferences_texture_quality_row(BongoCatApp *app,
    struct nk_context *context) {
    const char *const labels[] = {
        tr(app, "pages.preference.cat.labels.tqStandard", "Standard"),
        tr(app, "pages.preference.cat.labels.tqHigh", "High"),
        tr(app, "pages.preference.cat.labels.tqNative", "Native")};
    const int count = (int)(sizeof(labels) / sizeof(labels[0]));
    int selected = app->settings.model.texture_quality;
    if (selected < 0 || selected >= count) selected = 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
    /* 用下拉而不是分段控件: 三档要显示"标准/高/原生"文字, 与上面的平滑度一致 */
    selected = bongo_cat_pref_combo(context, "texture-quality",
        tr(app, "pages.preference.cat.labels.textureQuality", "Texture quality"),
        tr(app, "pages.preference.cat.hints.textureQuality",
            "越高模型上的小字越清楚，但更吃显存；对下一个加载的模型生效"),
        labels, count, selected);
    if (selected == app->settings.model.texture_quality) return;
    app->settings.model.texture_quality = selected;
    app->dirty = true;
    /* 下发新上限 (顺带按新档位算一次"这个窗口值得的上限") */
    bongo_cat_window_apply(app);
    /* 图集尺寸在解码那一刻定死, 但用户刚明确选了一档 —— 现在就用异步切换把当前模型按新档
       重新加载一遍, 否则他会以为"选了原生没反应"(窗口小时上限还会被显示尺寸压住)。
       两个方向都算: 选了更省的那档, 也应该真的降下来。 */
    bongo_cat_app_refresh_model_textures(app);
}
