#ifndef BONGO_CAT_TRAY_H
#define BONGO_CAT_TRAY_H

#include "bongo_cat/common.h"

typedef struct BongoCatApp BongoCatApp;
typedef struct BongoCatTray BongoCatTray;

BongoCatTray *bongo_cat_tray_create(BongoCatApp *app, BongoCatError *error);
void bongo_cat_tray_destroy(BongoCatTray *tray);
void bongo_cat_tray_sync(BongoCatTray *tray);
void bongo_cat_tray_prepare_menu(BongoCatTray *tray, void *native_handle);
bool bongo_cat_tray_self_test(BongoCatTray *tray);
/*
 * 真实执行一次托盘里的"渲染后端"那一项 (等同于用户点了它): 写配置 -> 弹重启确认框 ->
 * 按用户的选择决定要不要立刻重启。只给自动化验证用 —— 真实点击托盘图标没法脚本化,
 * 而这条路径必须能端到端跑 (见 app.c 里的 BONGO_CAT_TEST_TRAY_RENDERER)。
 * 容忍 NULL (没有托盘时什么都不做)。
 */
void bongo_cat_tray_renderer_action(BongoCatTray *tray);

#endif
