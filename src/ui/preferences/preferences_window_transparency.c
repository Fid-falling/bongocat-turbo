/*
 * 设置窗口 [画面] 区 (宠物页的子区) 里的"透明背景"开关 (window.transparentBackground)。
 *
 * 为什么单独一个文件: preferences_pages.c 已经贴着 500 行的策略上限 (见
 * cmake/CheckLines.cmake), 而这一项从文案到"改完立刻生效"都自成一块, 正好搬出来。
 *
 * 它背后是 Windows 上的代理分层呈现 (src/platform/windows/windows_layered_window.c):
     * 普通窗口的缓冲 alpha 会被系统忽略, 猫以外的区域只能显示成黑块; 打开这一项之后
     * 画面改由一个分层代理窗口显示, 黑块才真正透明。
     *
     * OpenGL 与 Vulkan 走的是同一个开关: 两个后端都读 platform->transparent_background, 只要
     * 开了就切到代理分层 (Vulkan: 整帧回读后由分层窗口呈现, 见 vk_scene.c; 不开
     * 则走交换链, 零开销)。因为只有 Windows 有代理分层这种等价机制, 这里整块包在
     * _WIN32 里 —— 其它平台不显示这一行, 免得给用户一个点了没反应的开关 (与"仅在
     * 录屏软件中显示"同一套做法)。
 *
 * 文案上的两条硬要求: 不出现实现术语(每帧/1x/DWM/alpha 之类), 但必须写明可能影响
 * 录屏软件对这个窗口的采集 —— 那正是它的代价, 也是默认开启(透明是桌宠的应有之义)
 * 却仍要把代价写进提示里的原因。
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
    bool transparent = app->settings.window.transparent_background != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_PASS_THROUGH);
    if (!bongo_cat_pref_toggle(context, "transparent-background", tr(app,
        "pages.preference.cat.labels.transparentBackground", "Transparent background"),
        tr(app, "pages.preference.cat.labels.transparentBackgroundHint",
        "Makes the pet background truly transparent; may affect capture "
        "software recording this window"), &transparent)) return;
    app->settings.window.transparent_background = transparent ? 1 : 0;
    app->dirty = true;
    /*
     * 立刻下发一次: 打开/关闭都不需要重启, 用户唯一能看到的差别(猫周围是黑的还是透的)
     * 必须马上跟着变。这里走的是专门的"透明背景下发"入口, 不是穿透同步 ——
     * 呈现路径的开关只由设置决定, 挂在逐帧的命中测试路径上会被光标位置来回推翻。
     */
    bongo_cat_window_apply_transparency(app);
}
#endif