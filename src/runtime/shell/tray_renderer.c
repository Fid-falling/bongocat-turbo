/*
 * 托盘里的"切换渲染后端"入口 —— 设置界面自己坏掉时唯一的 GUI 逃生通道。
 *
 * 为什么必须有: 设置窗口是用当前后端画的。Vulkan 在这台机器上画不出内容时, 用户
 * 连"渲染后端"这个下拉框都点不到, 于是永远回不去 OpenGL。托盘菜单是独立于渲染
 * 后端的原生菜单 (Windows 上是 Shell_NotifyIcon 的 HMENU), 后端再坏它也能弹出来。
 * (不依赖 GUI 的那条路是命令行 --renderer=, 见 startup_arguments.c。)
 *
 * 目标后端由"当前实际在用的后端"决定, 而不是配置里的值: 配置写着 vulkan 但已经
 * 回退成 OpenGL 的机器上, 再"切到 opengl"是没有意义的。
 *
 * 生效方式: 写进 app->settings.model.renderer, 由 config_store 在 300ms 内落盘, 但
 * **本次运行不会切** —— 运行中换后端要把主窗口、Live2D 渲染器、平台层全部重建, 代价
 * 与风险都远大于"重启一次"。所以这里明确弹窗告诉用户需要重启, 而不是假装已经生效
 * (那才是真的把用户困住)。
 *
 * 目标后端不可用时不做任何写入: 把它写成 vulkan 而机器上没有 Vulkan 设备, 用户重启
 * 后只会得到一次"回退到 OpenGL"的日志和一个没变化的界面 —— 不如当场说清楚。
 */
#include "tray_internal.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * 这一项要切到哪个后端: 永远是"当前实际在用的那一个"的反面, 而不是配置里的值的反面。
 * 配置写着 vulkan 但已经回退成 OpenGL 的机器上, 再"切到 opengl"是没有意义的。
 *
 * 单独抽出来是因为有三个消费者 (文案、点击、自测), 三处各写一遍必然会对不上 ——
 * 自测第一次就栽在这里: 它按"配置值必须改变"断言, 而配置本来就等于目标时那个断言
 * 永远是假的。
 */
BongoCatRenderer bongo_cat_tray_renderer_target(const BongoCatTray *tray) {
    (void)tray;
    return bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN
        ? BONGO_CAT_RENDERER_OPENGL : BONGO_CAT_RENDERER_VULKAN;
}

/*
 * 那一项的文案。写成 "渲染后端: Vulkan -> OpenGL" 而不是一个勾选项: 目标是什么必须
 * 写在脸上 —— 用户点这个菜单的时候, 屏幕上的界面往往已经看不清了。
 * 返回值指向 tray->renderer_label, 生命周期与本对象一致 (SDL 会拷走字符串)。
 */
const char *bongo_cat_tray_renderer_label(BongoCatTray *tray) {
    const BongoCatRenderer current = bongo_cat_renderer_active();
    snprintf(tray->renderer_label, sizeof(tray->renderer_label), "%s: %s -> %s",
        bongo_cat_i18n_get(tray->app->i18n,
            "composables.useAppMenu.labels.renderer", "Render backend"),
        bongo_cat_renderer_description(current),
        bongo_cat_renderer_description(bongo_cat_tray_renderer_target(tray)));
    return tray->renderer_label;
}

void bongo_cat_tray_on_renderer(void *userdata, SDL_TrayEntry *entry) {
    (void)entry;
    BongoCatTray *tray = userdata;
    BongoCatApp *app = tray->app;
    const BongoCatRenderer target = bongo_cat_tray_renderer_target(tray);
    const bool want_opengl = target == BONGO_CAT_RENDERER_OPENGL;
    if (want_opengl && !bongo_cat_renderer_available(BONGO_CAT_RENDERER_OPENGL))
        return;
    if (!want_opengl && !bongo_cat_renderer_available(BONGO_CAT_RENDERER_VULKAN)) {
        /* 拒绝也要留痕: 用户点了菜单却什么都没发生, 日志里必须有原因。 */
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[render] 托盘切换被拒: Vulkan 在这台机器上不可用 (%s)",
            bongo_cat_renderer_unavailable_reason(BONGO_CAT_RENDERER_VULKAN));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, BONGO_CAT_NAME,
            bongo_cat_i18n_get(app->i18n,
                "composables.useAppMenu.labels.rendererUnavailable",
                "Vulkan is not available on this machine."), app->window);
        return;
    }
    const BongoCatRenderer previous = app->settings.model.renderer;
    app->settings.model.renderer = target;
    /* 立刻把标签贴成新值, 用户点完就能看到结果 (真正的重启提示在下面的弹窗里)。 */
    bongo_cat_tray_sync(tray);
    bongo_cat_preferences_invalidate(app->preferences);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 托盘切换后端: 配置 %s -> %s (当前实际在用 %s, 重启后生效)",
        bongo_cat_renderer_name(previous), bongo_cat_renderer_name(target),
        bongo_cat_renderer_name(bongo_cat_renderer_active()));
    /*
     * 模态弹窗在无人值守运行里会把进程挂住 (--ci-menu 的自测会走这个回调, 而
     * SDL_ShowSimpleMessageBox 会阻塞调用线程直到有人点确定), 所以冒烟/--ci-* 运行
     * 与设置了 BONGO_CAT_TEST_NO_DIALOG 时只记日志。
     */
    if (app->smoke || app->automated || SDL_getenv("BONGO_CAT_TEST_NO_DIALOG"))
        return;
    char message[512];
    /* 命令行覆盖会一直压过配置文件, 这时必须把原因说清楚, 否则用户重启后会觉得
       "切了没用"。 */
    const char *override_note = app->renderer_override >= 0
        ? bongo_cat_i18n_get(app->i18n,
            "composables.useAppMenu.labels.rendererOverrideNotice",
            "Note: this session was started with a --renderer argument, which "
            "always wins over the saved setting. Start BongoCat without it to "
            "use the new value.")
        : "";
    snprintf(message, sizeof(message), "%s\n\n%s%s%s",
        bongo_cat_renderer_description(target),
        bongo_cat_i18n_get(app->i18n,
            "composables.useAppMenu.labels.rendererRestart",
            "Restart BongoCat to apply the new render backend."),
        override_note[0] ? "\n\n" : "", override_note);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, BONGO_CAT_NAME,
        message, app->window);
}
