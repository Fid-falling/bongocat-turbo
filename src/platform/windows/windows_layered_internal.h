#ifndef BONGO_CAT_WINDOWS_LAYERED_INTERNAL_H
#define BONGO_CAT_WINDOWS_LAYERED_INTERNAL_H

#include "windows_layered.h"

#ifdef _WIN32
#include <windows.h>

/*
 * 分层窗口呈现器的内部状态, 只给同目录的 windows_layered*.c 使用。
 *
 * 关键字段: pixels 是自己维护的 DIB 像素缓冲(memory_dc / bitmap 与它绑定);
 * readback 是"最近一次提交给 DWM 的内容"的副本, readback_valid 表示它可用 ——
 * 命中测试与逐像素 alpha 都读 readback, 因为提交之后 pixels 很快会被下一帧覆盖。
 * 不变量: active 为真时, pixels / bitmap / memory_dc 三件套一定同时存在;
 *         source_width/source_height 是渲染源的尺寸, width/height 是真正提交给
 *         DWM 的尺寸, 两者可能不相等(有缩放), 不要混用。
 */
typedef struct BongoCatWindowsLayered {
    HDC memory_dc;
    HBITMAP bitmap;
    HGDIOBJ original_bitmap;
    unsigned char *pixels;
    unsigned char *readback;
    size_t readback_capacity;
    HWND proxy;
    int width, height, source_width, source_height;
    bool readback_valid, active, has_frame, mode_logged;
    bool source_transparent, visible, topmost;
    BYTE applied_alpha;
    bool applied_alpha_valid;
} BongoCatWindowsLayered;

/* 重新提交整帧(实现在 windows_layered.c)。调用方必须是渲染/主线程 ——
   UpdateLayeredWindow 在窗口所属线程上同步生效。 */
bool bongo_cat_windows_layered_update_proxy(BongoCatPlatform *platform,
    BongoCatWindowsLayered *value);
#endif

#endif
