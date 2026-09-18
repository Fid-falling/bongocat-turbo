#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "validation_image.h"
#include "windows_tool.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { FRAME_COUNT = 14 };

typedef struct WindowSearch { DWORD process; HWND window; } WindowSearch;

typedef struct Frame {
    const wchar_t *name;
    double requested;
    double begin;
    double end;
    BongoCatValidationImage image;
} Frame;

static LARGE_INTEGER frequency;

static BOOL CALLBACK find_window(HWND window, LPARAM data) {
    WindowSearch *search = (WindowSearch *)data;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == search->process && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == NULL) {
        search->window = window; return FALSE;
    }
    return TRUE;
}

static HWND process_window(DWORD process) {
    WindowSearch search = {process, NULL};
    EnumWindows(find_window, (LPARAM)&search);
    return search.window;
}

static double now_milliseconds(void) {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart * 1000.0 / frequency.QuadPart;
}

static void click(void) {
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

static void key(BYTE code) {
    keybd_event(code, 0, 0, 0);
    keybd_event(code, 0, KEYEVENTF_KEYUP, 0);
    Sleep(80);
}

static void select_expression(int index) {
    SetCursorPos(100, 194); click(); Sleep(150); key(VK_RIGHT); Sleep(150);
    SetCursorPos(120, 222 + index * 29); click(); Sleep(1000);
}

static void fit_and_center(void) {
    SetCursorPos(313, 659); click(); Sleep(300);
    SetCursorPos(426, 659); click(); Sleep(500);
}

static wchar_t *output_path(const wchar_t *directory, const wchar_t *name,
    const wchar_t *extension) {
    size_t length = wcslen(directory) + wcslen(name) + wcslen(extension) + 3;
    wchar_t *path = (wchar_t *)malloc(length * sizeof(*path));
    if (path) swprintf(path, length, L"%ls\\%ls%ls", directory, name, extension);
    return path;
}

static int capture_image(int left, int top, int width, int height,
    double origin, Frame *frame) {
    BITMAPINFO bitmap = {0};
    HDC screen = NULL, memory = NULL;
    HBITMAP surface = NULL, previous = NULL;
    void *bits = NULL;
    int copied = 0, x, y;
    bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    memory = screen ? CreateCompatibleDC(screen) : NULL;
    surface = memory ? CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS,
        &bits, NULL, 0) : NULL;
    if (!surface || !bits) goto done;
    previous = (HBITMAP)SelectObject(memory, surface);
    frame->begin = origin ? now_milliseconds() - origin : 0.0;
    copied = BitBlt(memory, 0, 0, width, height, screen, left, top, SRCCOPY);
    frame->end = origin ? now_milliseconds() - origin : 0.0;
    if (!copied) goto done;
    frame->image.width = width; frame->image.height = height;
    frame->image.bgra = (unsigned char *)malloc((size_t)width * height * 4);
    if (!frame->image.bgra) { copied = 0; goto done; }
    memcpy(frame->image.bgra, bits, (size_t)width * height * 4);
    for (y = 0; y < height; ++y) for (x = 0; x < width; ++x)
        frame->image.bgra[((size_t)y * width + x) * 4 + 3] = 255;
done:
    if (previous) SelectObject(memory, previous);
    if (surface) DeleteObject(surface);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(NULL, screen);
    return copied && frame->image.bgra != NULL;
}

static void wait_until(double origin, double milliseconds) {
    for (;;) {
        double remaining = milliseconds - (now_milliseconds() - origin);
        if (remaining <= 0.0) return;
        if (remaining > 2.0) Sleep(1);
        else YieldProcessor();
    }
}

static int save_frames(Frame *frames, int count, const wchar_t *directory,
    int press_x, int press_y, int target_x, int target_y,
    int left, int top, int width, int height) {
    wchar_t *trace_path = output_path(directory, L"trace", L".csv");
    FILE *trace = trace_path ? _wfopen(trace_path, L"w") : NULL;
    double drag_x = 2.0 * (target_x - left) / width - 1.0;
    double drag_y = 1.0 - 2.0 * (target_y - top) / height;
    int index, ok = trace != NULL;
    free(trace_path);
    if (!trace) return 0;
    fprintf(trace, "frame,phase_ms,capture_mid_ms,requested_ms,capture_end_ms,"
        "press_x,press_y,target_x,target_y,target_drag_x,target_drag_y,"
        "capture_left,capture_top,capture_width,capture_height\n");
    for (index = 0; index < count; ++index) {
        wchar_t *path = output_path(directory, frames[index].name, L".png");
        if (!path || !bongo_cat_validation_image_save(path,
            &frames[index].image, 1)) ok = 0;
        free(path);
        if (fprintf(trace, "%ls,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%.6f,%.6f,"
            "%d,%d,%d,%d\n", frames[index].name, frames[index].begin,
            (frames[index].begin + frames[index].end) * 0.5,
            frames[index].requested, frames[index].end, press_x, press_y,
            target_x, target_y, drag_x, drag_y, left, top, width, height) < 0)
            ok = 0;
        bongo_cat_validation_image_free(&frames[index].image);
    }
    if (fclose(trace) != 0) ok = 0;
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    static const wchar_t *track_names[] = {L"track-001", L"track-002",
        L"track-004", L"track-008", L"track-015", L"track-030"};
    static const wchar_t *return_names[] = {L"return-001", L"return-002",
        L"return-004", L"return-008", L"return-015", L"return-030"};
    static const double times[] = {16.667, 33.333, 66.667, 133.333, 250.0, 500.0};
    Frame frames[FRAME_COUNT] = {0};
    DWORD process;
    HWND window;
    int left, top, width, height, press_x, press_y, target_x, target_y;
    int settle, expression, count = 0, index, result = 1;
    double origin;
    setlocale(LC_NUMERIC, "C");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 3) {
        fwprintf(stderr, L"usage: capture.exe output-directory viewer-pid "
            L"[left top width height press-x press-y target-x target-y "
            L"press-settle-ms expression-index]\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
    QueryPerformanceFrequency(&frequency);
    process = (DWORD)_wtoi(argv[2]);
    left = argc > 3 ? _wtoi(argv[3]) : 142;
    top = argc > 4 ? _wtoi(argv[4]) : 80;
    width = argc > 5 ? _wtoi(argv[5]) : 1138;
    height = argc > 6 ? _wtoi(argv[6]) : 640;
    press_x = argc > 7 ? _wtoi(argv[7]) : 700;
    press_y = argc > 8 ? _wtoi(argv[8]) : 500;
    target_x = argc > 9 ? _wtoi(argv[9]) : 985;
    target_y = argc > 10 ? _wtoi(argv[10]) : 308;
    settle = argc > 11 ? _wtoi(argv[11]) : 150;
    expression = argc > 12 ? _wtoi(argv[12]) : -1;
    if (!bongo_cat_tool_ensure_directory(argv[1]) || width <= 0 || height <= 0)
        goto done;
    window = process_window(process);
    if (!window) { fwprintf(stderr, L"Viewer window not found\n"); goto done; }
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 1280, 720, SWP_SHOWWINDOW);
    SetForegroundWindow(window); Sleep(1200);
    if (expression >= 0) select_expression(expression);
    fit_and_center(); SetCursorPos(press_x, press_y); Sleep(800);
    frames[count].name = L"idle";
    if (!capture_image(left, top, width, height, 0.0, &frames[count++])) goto done;
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); Sleep(settle);
    frames[count].name = L"track-000";
    if (!capture_image(left, top, width, height, 0.0, &frames[count++])) goto done;
    SetCursorPos(target_x, target_y); origin = now_milliseconds();
    for (index = 0; index < 6; ++index) {
        wait_until(origin, times[index]);
        frames[count].name = track_names[index]; frames[count].requested = times[index];
        if (!capture_image(left, top, width, height, origin, &frames[count++])) goto done;
    }
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); origin = now_milliseconds();
    for (index = 0; index < 6; ++index) {
        wait_until(origin, times[index]);
        frames[count].name = return_names[index]; frames[count].requested = times[index];
        if (!capture_image(left, top, width, height, origin, &frames[count++])) goto done;
    }
    if (!save_frames(frames, count, argv[1], press_x, press_y, target_x,
        target_y, left, top, width, height)) goto done;
    wprintf(L"frames=%d output=%ls\n", count, argv[1]);
    result = 0;
done:
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    if (result) for (index = 0; index < count; ++index)
        bongo_cat_validation_image_free(&frames[index].image);
    CoUninitialize();
    return result;
}
