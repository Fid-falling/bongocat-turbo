#ifndef BONGO_CAT_WINDOWS_LAYERED_H
#define BONGO_CAT_WINDOWS_LAYERED_H

#include "bongo_cat/platform.h"

#ifdef _WIN32
#include <windows.h>
/*
 * 分层窗口(layered window)呈现器的对外接口。
 *
 * 为什么需要它: SDL 的普通呈现路径只能给整个窗口一个统一的不透明度, 而桌宠要按
 * 像素透明(猫以外的区域必须真透明, 并且能让鼠标点穿)。分层窗口用
 * UpdateLayeredWindow 提交一张预乘 alpha 的位图, 这套语义只有 Windows 有, 所以
 * 整个模块包在 #ifdef _WIN32 里。
 * create 返回不透明句柄(void *), 真实类型见 windows_layered_internal.h —— 这样
 * platform.h 不必包含 windows.h。
 */
void *bongo_cat_windows_layered_create(void);
void bongo_cat_windows_layered_destroy(BongoCatPlatform *platform);
void bongo_cat_windows_layered_set_click_through(
    BongoCatPlatform *platform, bool enabled);
void bongo_cat_windows_layered_set_always_on_top(
    BongoCatPlatform *platform, bool enabled);
/* 返回 source 对应的"代理窗口"句柄(没有则 NULL)。外部模块拿它做命中测试与采集
   判断时不要缓存这个句柄 —— 它可能随模式切换被重建。 */
HWND bongo_cat_windows_layered_proxy(HWND source);
#endif

#endif
