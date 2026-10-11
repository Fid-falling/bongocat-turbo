/*
 * 设置窗口 [窗口] 区里的"透明背景"开关 (window.glTransparency)。
 *
 * 为什么单独一个文件: preferences_pages.c 已经贴着 500 行的策略上限 (见
 * cmake/CheckLines.cmake), 而这一项从文案到"改完立刻生效"都自成一块, 正好搬出来。
 *
 * 它背后是 Windows 上的代理分层呈现 (src/platform/windows/windows_layered_window.c):
 * OpenGL 模式下普通窗口的缓冲 alpha 会被系统忽略, 猫以外的区域只能显示成黑块; 打开
 * 这一项之后画面改由一个分层代理窗口显示, 黑块才真正透明。因为只有 Windows 有等价
 * 机制, 这里整块包在 _WIN32 里 —— 其它平台不显示这一行, 免得给用户一个点了没反应的
 * 开关 (与"仅在录屏软件中显示"同一套做法)。
 *
 * 文案上的两条硬要求: 不出现实现术语(每帧/1x/DWM/alpha 之类), 但必须写明可能影响
 * 录屏软件对这个窗口的采集 —— 那正是它默认关闭的原因。
 */
#include "preferences_internal.h"
#include "preferences_widgets.h"
#include "bongo_cat/i18n.h"
#include "runtime.h"
#include "bongo_cat/render.h"

#ifdef _WIN32
static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

void bongo_cat_preferences_window_transparency(BongoCatApp *app,
    struct nk_context *context) {
    if (!app || !context) return;
    /*
     * 只在 OpenGL 后端显示这一行。Vulkan 的交换链本来就声明预乘 alpha, 透明是原生的,
     * 这一项在那边是彻底的 no-op (平台层也加了护栏) —— 给用户一个点了没有任何变化的
     * 开关只会让人困惑, 所以直接不画。
     */
    if (bongo_cat_renderer_active() != BONGO_CAT_RENDERER_OPENGL) return;
    bool transparent = app->settings.window.gl_transparency != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_PASS_THROUGH);
    if (!bongo_cat_pref_toggle(context, "gl-transparency", tr(app,
        "pages.preference.cat.labels.glTransparency", "Transparent background"),
        tr(app, "pages.preference.cat.labels.glTransparencyHint",
        "Makes the pet background truly transparent; may affect capture "
        "software recording this window"), &transparent)) return;
    app->settings.window.gl_transparency = transparent ? 1 : 0;
    app->dirty = true;
    /*
     * 立刻下发一次: 打开/关闭都不需要重启, 用户唯一能看到的差别(猫周围是黑的还是透的)
     * 必须马上跟着变。这里走的是专门的"透明背景下发"入口, 不是穿透同步 ——
     * 呈现路径的开关只由设置决定, 挂在逐帧的命中测试路径上会被光标位置来回推翻。
     */
    bongo_cat_window_apply_transparency(app);
}
#endif
