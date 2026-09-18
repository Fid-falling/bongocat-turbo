#ifndef BONGO_CAT_CUBISM_VIEWER_DESKTOP_CAPTURE_H
#define BONGO_CAT_CUBISM_VIEWER_DESKTOP_CAPTURE_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <stddef.h>

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

void desktop_capture_destroy(DesktopCapture *capture);
int desktop_capture_init(DesktopCapture *capture, RECT crop);
int desktop_capture_at(DesktopCapture *capture, LONGLONG target, Frame *frame);

#endif
