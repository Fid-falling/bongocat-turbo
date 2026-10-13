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
 * 本文件只负责"托盘这一侧"的三件事: 目标怎么算、文案怎么写、点击往哪送。真正的切换
 * (校验可用性 -> 写配置 -> 落盘 -> 日志 -> 确认框 -> 重启或提示) 在
 * runtime/lifecycle/renderer_switch.c, 与设置页的分段选择器共用同一份 —— 两个入口
 * 各写一遍必然会出现"托盘能切、设置里点了没反应"这类不一致, 而且被拒时只有一边留痕。
 */
#include "tray_internal.h"
#include "runtime.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"

#include <stdio.h>

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
    if (!tray) return;
    bongo_cat_app_switch_renderer(tray->app, bongo_cat_tray_renderer_target(tray),
        BONGO_CAT_RENDERER_ENTRY_TRAY);
}
