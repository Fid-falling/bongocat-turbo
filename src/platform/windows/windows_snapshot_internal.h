#ifndef BONGO_CAT_WINDOWS_SNAPSHOT_INTERNAL_H
#define BONGO_CAT_WINDOWS_SNAPSHOT_INTERNAL_H

#include "windows_snapshot.h"
#include <windows.h>

/*
 * snapshot 模式的内部状态(只给 windows_snapshot*.c 使用)。
 *
 * 不变量: window / handle / renderer / texture 要么全为 NULL(创建中或已销毁), 要么
 * 全部有效; pixels 是按源窗口像素尺寸分配的 BGRA 缓冲, 命中测试一直读它, 因此
 * width/height 必须与 pixels 匹配 —— 它与预览窗口覆盖的桌面尺寸(desktop)是两回事。
 * suppressed 表示"源窗口已经被我们改成全透明", 只有它为真时才需要恢复。
 */
#define SNAPSHOT_MAX_PIXELS (16u * 1024u * 1024u)

struct BongoCatWindowsSnapshot {
    SDL_Window *source, *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    HWND source_handle, handle;
    WNDPROC original;
    unsigned char *pixels;
    int width, height;
    RECT desktop;
    SDL_FRect destination, presented;
    float opacity;
    bool suppressed, has_frame, custom_destination;
};

/* 三个内部入口: 抓图(要求源窗口的 GL 上下文处于 current)、绑定 / 解绑预览窗口的输入
   转发(见 windows_snapshot_input.c)。 */
bool bongo_cat_windows_snapshot_capture(BongoCatWindowsSnapshot *snapshot);
bool bongo_cat_windows_snapshot_bind_input(BongoCatWindowsSnapshot *snapshot);
void bongo_cat_windows_snapshot_unbind_input(BongoCatWindowsSnapshot *snapshot);

#endif
