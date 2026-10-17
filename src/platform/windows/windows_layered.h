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
 *
 * 呈现器是**按窗口**持有的: create 返回不透明句柄(void *), 真实类型见
 * windows_layered_internal.h —— 这样 platform.h 不必包含 windows.h。主窗口把它存在
 * BongoCatPlatform::presenter 上; 轮盘是独立窗口、没有 BongoCatPlatform, 用
 * bongo_cat_windows_layered_window_attach 拿自己的一份(见 windows_layered_window.c)。
 *
 * 所有接收 void *presenter 的入口都容忍 NULL(等价于"这个窗口没有代理呈现"), 调用方
 * 不必自己判空。
 */
void *bongo_cat_windows_layered_create(SDL_Window *window);
void bongo_cat_windows_layered_destroy(void *presenter);
void bongo_cat_windows_layered_set_click_through(void *presenter, bool enabled);
void bongo_cat_windows_layered_set_always_on_top(void *presenter, bool enabled);
/* 返回 source 对应的"代理窗口"句柄(没有则 NULL)。外部模块拿它做命中测试与采集
   判断时不要缓存这个句柄 —— 它可能随模式切换被重建。 */
HWND bongo_cat_windows_layered_proxy(HWND source);
/* GL 模式的逐像素透明总开关。主窗口与轮盘共用这一个判断: 写成两处就会出现
   "主窗口透明显了、轮盘还是黑底"这种半开状态。
   configured 是用户的设置项 window.glTransparency (0=关闭, 默认)。
   优先级: 环境变量 BONGO_CAT_GL_TRANSPARENCY (调试用强制覆盖) > 设置项;
   另外这套呈现要读 GL 前缓冲, 非 OpenGL 后端一律返回 false。 */
bool bongo_cat_windows_layered_requested(bool configured);
/* 代理模式下真实窗口收不到鼠标消息, 由代理把鼠标消息转投给 source(幂等)。
   preserve_transparent = true 时保留代理窗口的 WS_EX_TRANSPARENT(强制整窗穿透);
   为 false 时摘掉它, 让逐像素命中测试能收到鼠标消息。为什么必须转、以及转发的坑,
   见 windows_layered_window.c 的说明。 */
void bongo_cat_windows_layered_forward_input(HWND source,
    bool preserve_transparent);

/*
 * 第二个窗口(轮盘径向菜单)的接入口, 目前只有 src/ui/dial 用它。
 *
 * 轮盘是自带模态循环的独立 SDL 窗口, 生命周期与主窗口无关, 所以它自己 attach 一份
 * 呈现器、自己 present。释放必须发生在 SDL_DestroyWindow 之前 —— 代理窗口是源窗口的
 * owned window, 会跟着 owner 一起消失, 之后就没人能再恢复真实窗口的样式与属性了。
 * owner 传"菜单的宿主窗口"(即主窗口): 轮盘的代理窗口会挂到宿主的代理窗口之下, 这样
 * 主窗口那边无论怎么置前都盖不住菜单。Vulkan 渲染与开关关闭时这些入口不会被调用。
 */
void *bongo_cat_windows_layered_window_attach(SDL_Window *window, SDL_Window *owner,
    bool topmost);
/* 轮盘的每帧呈现入口。窗口还没显示时没有可暴露的帧, 此时返回 true(不算失败) ——
   轮盘先画一帧再 ShowWindow, 把这种情况当成错误会让菜单直接打不开。 */
bool bongo_cat_windows_layered_window_present(void *presenter, int width, int height);
/* SDL_RaiseWindow 抬的是真实窗口, 而显示画面的是代理窗口, 所以置顶要单独再走一次。 */
void bongo_cat_windows_layered_window_raise(void *presenter);
#endif

#endif
