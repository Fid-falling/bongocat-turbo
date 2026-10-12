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
 * 生效方式: 写进 app->settings.model.renderer, 由 config_store 在 300ms 内落盘,
 * **本次运行不切** —— 运行中换后端要把主窗口、Live2D 渲染器、平台层全部重建, 代价与
 * 风险都远大于一次重启。所以这里改成"落盘 + 自动重启": 新设置先写出去, 主循环末尾用
 * 同一份 exe 拉起新进程 (application_restart.c), 用户不需要自己去关再开。
 * 自动重启走不成时 (带防循环标记 / 无人值守运行) 才退回弹窗提示手动重启 —— 绝不能
 * 假装已经生效, 那才是真的把用户困住。
 *
 * 目标后端不可用时不做任何写入: 把它写成 vulkan 而机器上没有 Vulkan 设备, 用户重启
 * 后只会得到一次"回退到 OpenGL"的日志和一个没变化的界面 —— 不如当场说清楚。
 */
#include "tray_internal.h"
#include "preferences_notice.h"
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

/* 自动化入口: 真实点一次那一项 (走的是和用户点击完全相同的回调)。 */
void bongo_cat_tray_renderer_action(BongoCatTray *tray) {
    if (!tray || !tray->renderer) return;
    bongo_cat_tray_on_renderer(tray, tray->renderer);
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
    /* 立刻把标签贴成新值, 用户点完就能看到结果。 */
    bongo_cat_tray_sync(tray);
    bongo_cat_preferences_invalidate(app->preferences);
    /*
     * 切换生效 = 重启。先问用户"现在重启还是稍后手动重启" (确认框, 见
     * application_restart.c): 点了这一项不代表他同意现在就关掉正在跑的窗口。
     * 只有他选了"立即重启"才安排重启 —— 新设置先由 config_store 落盘, 主循环末尾再
     * 拉起新进程。
     */
    bool restarting = bongo_cat_app_confirm_restart(app) &&
        bongo_cat_app_request_restart(app);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 托盘切换后端: 配置 %s -> %s (当前实际在用 %s, %s)",
        bongo_cat_renderer_name(previous), bongo_cat_renderer_name(target),
        bongo_cat_renderer_name(bongo_cat_renderer_active()),
        restarting ? "将立即重启" : "已保存, 稍后手动重启生效");
    /* 重启已经安排好了就不该再提示: 用户看到提示的时候进程正在退出。 */
    if (restarting) return;
    /*
     * "稍后手动重启"(或者无人值守运行里不能弹框): 只写配置 + 提示。
     * 这里不再弹第二个模态框 —— 用户刚在确认框里做过选择, 再弹一个只是在烦他。
     */
    bongo_cat_preferences_notice_show(app, bongo_cat_i18n_get(app->i18n,
        "composables.useAppMenu.labels.rendererRestart",
        "Restart BongoCat to apply the new render backend."), false);
}
