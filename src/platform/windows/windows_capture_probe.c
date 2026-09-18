#include "windows_diagnostics.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <dwmapi.h>
#include <stdint.h>
#include <string.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

typedef struct CaptureSurface {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous;
    unsigned char *pixels;
    size_t bytes;
    int width;
    int height;
} CaptureSurface;

typedef struct CaptureObservation {
    bool called;
    bool wrote_pixels;
    bool nonblack;
    unsigned long long hash;
} CaptureObservation;

static bool capture_surface_create(CaptureSurface *surface,
    int width, int height) {
    if (!surface || width < 1 || height < 1 || width > 8192 || height > 8192)
        return false;
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels / (size_t)width != (size_t)height || pixels > SIZE_MAX / 4)
        return false;
    *surface = (CaptureSurface){.width = width, .height = height,
        .bytes = pixels * 4};
    surface->dc = CreateCompatibleDC(NULL);
    if (!surface->dc) return false;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    surface->bitmap = CreateDIBSection(surface->dc, &info,
        DIB_RGB_COLORS, &bits, NULL, 0);
    if (!surface->bitmap || !bits) {
        if (surface->bitmap) DeleteObject(surface->bitmap);
        DeleteDC(surface->dc);
        *surface = (CaptureSurface){0};
        return false;
    }
    surface->pixels = bits;
    surface->previous = SelectObject(surface->dc, surface->bitmap);
    if (!surface->previous || surface->previous == HGDI_ERROR) {
        DeleteObject(surface->bitmap);
        DeleteDC(surface->dc);
        *surface = (CaptureSurface){0};
        return false;
    }
    return true;
}

static void capture_surface_destroy(CaptureSurface *surface) {
    if (!surface) return;
    if (surface->dc && surface->previous)
        SelectObject(surface->dc, surface->previous);
    if (surface->bitmap) DeleteObject(surface->bitmap);
    if (surface->dc) DeleteDC(surface->dc);
    *surface = (CaptureSurface){0};
}

static void capture_surface_prepare(CaptureSurface *surface) {
    for (size_t index = 0; index < surface->bytes; index += 4) {
        surface->pixels[index] = 0x5a;
        surface->pixels[index + 1] = 0xa5;
        surface->pixels[index + 2] = 0x3c;
        surface->pixels[index + 3] = 0xc7;
    }
}

static CaptureObservation log_capture_pixels(const char *stage,
    const char *method, const CaptureSurface *surface,
    bool called, DWORD error) {
    CaptureObservation observation = {.called = called};
    if (!surface || !surface->pixels || !surface->bytes) return observation;
    unsigned long long total = (unsigned long long)surface->width *
        (unsigned long long)surface->height;
    unsigned long long untouched = 0, black = 0, colored = 0;
    unsigned long long key_green = 0, key_blue = 0;
    unsigned long long key_red = 0, key_magenta = 0;
    unsigned long long red = 0, green = 0, blue = 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < surface->bytes; index += 4) {
        unsigned b = surface->pixels[index];
        unsigned g = surface->pixels[index + 1];
        unsigned r = surface->pixels[index + 2];
        bool unchanged = b == 0x5a && g == 0xa5 && r == 0x3c &&
            surface->pixels[index + 3] == 0xc7;
        untouched += unchanged;
        for (int component = 0; component < 4; ++component) {
            hash ^= surface->pixels[index + (size_t)component];
            hash *= UINT64_C(1099511628211);
        }
        if (unchanged) continue;
        unsigned maximum = r > g ? (r > b ? r : b) : (g > b ? g : b);
        unsigned minimum = r < g ? (r < b ? r : b) : (g < b ? g : b);
        unsigned sum = r + g + b;
        black += sum <= 24;
        colored += sum > 40 && maximum - minimum > 16;
        key_green += g > 160 && r < 96 && b < 96;
        key_blue += b > 160 && r < 96 && g < 96;
        key_red += r > 160 && g < 96 && b < 96;
        key_magenta += r > 160 && b > 160 && g < 96;
        red += r; green += g; blue += b;
    }
    unsigned long long written = total - untouched;
    unsigned long long nonblack = written - black;
    unsigned long long measured = written ? written : 1;
    observation.wrote_pixels = written > 0;
    observation.nonblack = written > 0 && nonblack * 1000 >= written;
    observation.hash = hash;
    SDL_Log("Legacy capture probe (%s/%s): call=%d error=%lu wrote_pixels=%d "
        "size=%dx%d total=%llu written=%llu (%.2f%%) nonblack=%llu "
        "(%.2f%%) colored=%llu (%.2f%%) key_green=%llu key_blue=%llu "
        "key_red=%llu key_magenta=%llu avg_rgb=%.1f,%.1f,%.1f hash=0x%016llx",
        stage ? stage : "unknown", method, called, (unsigned long)error,
        observation.wrote_pixels, surface->width, surface->height, total,
        written, 100.0 * written / total, nonblack,
        100.0 * nonblack / measured, colored, 100.0 * colored / measured,
        key_green, key_blue, key_red, key_magenta,
        (double)red / measured, (double)green / measured,
        (double)blue / measured,
        (unsigned long long)hash);
    return observation;
}

static CaptureObservation probe_bitblt(HDC source, DWORD source_error,
    CaptureSurface *surface, const char *stage, const char *method,
    DWORD operation) {
    capture_surface_prepare(surface);
    if (source) SetLastError(ERROR_SUCCESS);
    bool called = source && BitBlt(surface->dc, 0, 0,
        surface->width, surface->height, source, 0, 0, operation) != FALSE;
    DWORD error = called ? ERROR_SUCCESS : source ? GetLastError() : source_error;
    GdiFlush();
    return log_capture_pixels(stage, method, surface, called, error);
}

static CaptureObservation probe_print_window(HWND window,
    CaptureSurface *surface, const char *stage, const char *method,
    UINT flags) {
    capture_surface_prepare(surface);
    SetLastError(ERROR_SUCCESS);
    bool called = PrintWindow(window, surface->dc, flags) != FALSE;
    DWORD error = called ? ERROR_SUCCESS : GetLastError();
    GdiFlush();
    return log_capture_pixels(stage, method, surface, called, error);
}

bool bongo_cat_windows_diagnostics_probe_capture(
    HWND window, const char *stage) {
    if (!window || !IsWindow(window)) return false;
    RECT client = {0};
    if (!GetClientRect(window, &client)) return false;
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    CaptureSurface surface;
    if (!capture_surface_create(&surface, width, height)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Legacy capture probe (%s): cannot allocate %dx%d surface "
            "(Win32 error %lu)", stage ? stage : "unknown", width, height,
            (unsigned long)GetLastError());
        return false;
    }
    SDL_Log("Legacy capture probe begin: stage=%s hwnd=%p visible=%d "
        "iconic=%d client=%dx%d",
        stage ? stage : "unknown", (void *)window,
        IsWindowVisible(window) != FALSE, IsIconic(window) != FALSE,
        width, height);
    HRESULT flush = DwmFlush();
    SDL_Log("Legacy capture probe composition sync: stage=%s result=0x%08lx",
        stage ? stage : "unknown", (unsigned long)flush);
    SetLastError(ERROR_SUCCESS);
    HDC source = GetDC(window);
    DWORD source_error = source ? ERROR_SUCCESS : GetLastError();
    CaptureObservation bitblt = probe_bitblt(source, source_error, &surface,
        stage, "bitblt", SRCCOPY);
    CaptureObservation captureblt = probe_bitblt(source, source_error, &surface,
        stage, "bitblt-captureblt", SRCCOPY | CAPTUREBLT);
    if (source) ReleaseDC(window, source);
    CaptureObservation print_client = probe_print_window(window, &surface,
        stage, "printwindow-client", PW_CLIENTONLY);
    CaptureObservation print_full = probe_print_window(window, &surface,
        stage, "printwindow-fullcontent", PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    const CaptureObservation observations[] = {
        bitblt, captureblt, print_client, print_full};
    unsigned called = 0, wrote = 0, nonblack = 0;
    for (size_t index = 0; index < 4; ++index) {
        called += observations[index].called;
        wrote += observations[index].wrote_pixels;
        nonblack += observations[index].nonblack;
    }
    SDL_Log("Legacy capture probe summary: stage=%s calls=%u/4 writes=%u/4 "
        "nonblack=%u/4 bitblt=%d captureblt=%d print_client=%d print_full=%d",
        stage ? stage : "unknown", called, wrote, nonblack,
        bitblt.nonblack, captureblt.nonblack,
        print_client.nonblack, print_full.nonblack);
    if (!called) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "Legacy capture diagnosis (%s): every API call failed",
        stage ? stage : "unknown");
    else if (!wrote) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "Legacy capture diagnosis (%s): APIs reported success without writing pixels",
        stage ? stage : "unknown");
    else if (!nonblack) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "Legacy capture diagnosis (%s): written results contain only black pixels",
        stage ? stage : "unknown");
    else if (nonblack < wrote) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "Legacy capture diagnosis (%s): pixel availability depends on the API",
        stage ? stage : "unknown");
    else SDL_Log("Legacy capture diagnosis (%s): every written result contains "
        "nonblack pixels; this does not prove a third-party capturer uses that path",
        stage ? stage : "unknown");
    capture_surface_destroy(&surface);
    return true;
}
#endif
