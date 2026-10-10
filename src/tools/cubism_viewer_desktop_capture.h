#ifndef BONGO_CAT_CUBISM_VIEWER_DESKTOP_CAPTURE_H
#define BONGO_CAT_CUBISM_VIEWER_DESKTOP_CAPTURE_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <stddef.h>

/*
 * 用 DXGI 桌面复制(DuplicateOutput)抓取一块屏幕区域, 并给每一帧带上它的呈现
 * 时间戳 —— 采集类工具要靠这个时间戳对齐"请求的时刻"与"实际抓到的帧"。
 * 只在 Windows 上构建; pixels 是紧密排列的 BGRA, 由调用方负责释放。
 */
typedef struct Frame {
    LONGLONG present;
    unsigned char *pixels;
    size_t size;
} Frame;

typedef struct DesktopCapture {
    RECT crop;
    RECT output;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIOutputDuplication *duplication;
    ID3D11Texture2D *staging;
} DesktopCapture;

/* 三个入口的契约: init 失败时 Capture 已被清零, 仍然可以安全 destroy;
   at() 会等到 present 时间戳不早于 target 的那一帧, 返回的 Frame.pixels 归调用方。 */
void desktop_capture_destroy(DesktopCapture *capture);
int desktop_capture_init(DesktopCapture *capture, RECT crop);
int desktop_capture_at(DesktopCapture *capture, LONGLONG target, Frame *frame);

#endif
