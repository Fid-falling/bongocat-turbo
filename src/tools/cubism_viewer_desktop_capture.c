#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cubism_viewer_desktop_capture.h"
#include "windows_tool.h"

typedef struct WindowSearch { DWORD process; HWND window; } WindowSearch;

typedef struct Sample {
    char name[24];
    Frame frame;
    LONGLONG phase;
    double requested;
} Sample;

static LARGE_INTEGER timer_frequency;

static LONGLONG ticks(void) {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static double milliseconds(LONGLONG value) {
    return value * 1000.0 / timer_frequency.QuadPart;
}

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

static int move_cursor(int x, int y) {
    return SetCursorPos(x, y) != 0;
}

static int mouse_input(DWORD flags) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE; input.mi.dwFlags = flags;
    return SendInput(1, &input, sizeof(input)) == 1;
}

static int click(int x, int y) {
    return move_cursor(x, y) && mouse_input(MOUSEEVENTF_LEFTDOWN) &&
        mouse_input(MOUSEEVENTF_LEFTUP);
}

static int key(WORD code) {
    INPUT input[2];
    memset(input, 0, sizeof(input));
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = code;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(2, input, sizeof(INPUT)) != 2) return 0;
    Sleep(80); return 1;
}

static int prepare_viewer(HWND window, int expression) {
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 1280, 720, SWP_SHOWWINDOW);
    SetForegroundWindow(window); Sleep(1200);
    if (expression >= 0) {
        if (!click(100, 194)) return 0; Sleep(150);
        if (!key(VK_RIGHT)) return 0; Sleep(150);
        if (!click(120, 222 + expression * 29)) return 0; Sleep(80);
        if (!click(120, 222 + expression * 29)) return 0; Sleep(1000);
    }
    if (!click(313, 659)) return 0; Sleep(300);
    if (!click(426, 659)) return 0; Sleep(500);
    return 1;
}

#pragma pack(push, 1)
typedef struct BitmapFileHeader {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1, reserved2;
    uint32_t offset;
} BitmapFileHeader;
#pragma pack(pop)

static wchar_t *sample_path(const wchar_t *directory, const char *name,
    const wchar_t *extension) {
    size_t a = wcslen(directory), b = strlen(name), c = wcslen(extension), i;
    wchar_t *result = (wchar_t *)malloc((a + b + c + 3) * sizeof(*result));
    if (!result) return NULL;
    memcpy(result, directory, a * sizeof(*result)); result[a++] = L'\\';
    for (i = 0; i < b; ++i) result[a++] = (unsigned char)name[i];
    memcpy(result + a, extension, (c + 1) * sizeof(*result));
    return result;
}

static int save_bitmap(const wchar_t *path, const Frame *frame,
    int width, int height) {
    BitmapFileHeader file = {0x4D42, 0, 0, 0, 0};
    BITMAPINFOHEADER info;
    FILE *output;
    memset(&info, 0, sizeof(info));
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = (DWORD)frame->size;
    file.offset = sizeof(file) + sizeof(info); file.size = file.offset + info.biSizeImage;
    output = _wfopen(path, L"wb");
    if (!output) return 0;
    if (fwrite(&file, 1, sizeof(file), output) != sizeof(file) ||
        fwrite(&info, 1, sizeof(info), output) != sizeof(info) ||
        fwrite(frame->pixels, 1, frame->size, output) != frame->size) {
        fclose(output); return 0;
    }
    return fclose(output) == 0;
}

static void sample_name(char *name, size_t capacity, const char *phase, int frame) {
    snprintf(name, capacity, "%s-%03d", phase, frame);
}

int wmain(int argc, wchar_t **argv) {
    static const int frame_numbers[] = {1, 2, 4, 8, 15, 30};
    DesktopCapture capture;
    Sample samples[14];
    HWND window;
    DWORD process;
    int left, top, width, height, press_x, press_y, target_x, target_y;
    int settle, expression, count = 0, index, result = 1;
    LONGLONG track_start, return_start;
    FILE *trace = NULL;
    wchar_t *trace_path = NULL;
    memset(&capture, 0, sizeof(capture)); memset(samples, 0, sizeof(samples));
    if (argc < 3) {
        fwprintf(stderr, L"usage: output-directory viewer-pid [capture arguments]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&timer_frequency);
    if (!bongo_cat_tool_ensure_directory(argv[1])) goto done;
    process = (DWORD)_wtoi(argv[2]);
    left = argc > 3 ? _wtoi(argv[3]) : 247;
    top = argc > 4 ? _wtoi(argv[4]) : 82;
    width = argc > 5 ? _wtoi(argv[5]) : 1022;
    height = argc > 6 ? _wtoi(argv[6]) : 560;
    press_x = argc > 7 ? _wtoi(argv[7]) : 758;
    press_y = argc > 8 ? _wtoi(argv[8]) : 362;
    target_x = argc > 9 ? _wtoi(argv[9]) : 1014;
    target_y = argc > 10 ? _wtoi(argv[10]) : 194;
    settle = argc > 11 ? _wtoi(argv[11]) : 150;
    expression = argc > 12 ? _wtoi(argv[12]) : -1;
    window = process_window(process);
    if (!window || !prepare_viewer(window, expression) ||
        !move_cursor(press_x, press_y)) goto done;
    Sleep(800);
    {
        RECT crop = {left, top, left + width, top + height};
        if (!desktop_capture_init(&capture, crop)) goto done;
    }
    strcpy_s(samples[count].name, sizeof(samples[count].name), "idle");
    if (!desktop_capture_at(&capture, ticks(), &samples[count++].frame)) goto done;
    if (!mouse_input(MOUSEEVENTF_LEFTDOWN)) goto done; Sleep(settle);
    strcpy_s(samples[count].name, sizeof(samples[count].name), "track-000");
    if (!desktop_capture_at(&capture, ticks(), &samples[count++].frame)) goto done;
    if (!move_cursor(target_x, target_y)) goto done;
    track_start = ticks();
    for (index = 0; index < 6; ++index) {
        double requested = frame_numbers[index] * (1000.0 / 60.0);
        LONGLONG target = track_start + (LONGLONG)(requested *
            timer_frequency.QuadPart / 1000.0);
        sample_name(samples[count].name, sizeof(samples[count].name),
            "track", frame_numbers[index]);
        samples[count].phase = track_start; samples[count].requested = requested;
        if (!desktop_capture_at(&capture, target, &samples[count++].frame)) goto done;
    }
    if (!mouse_input(MOUSEEVENTF_LEFTUP)) goto done;
    return_start = ticks();
    for (index = 0; index < 6; ++index) {
        double requested = frame_numbers[index] * (1000.0 / 60.0);
        LONGLONG target = return_start + (LONGLONG)(requested *
            timer_frequency.QuadPart / 1000.0);
        sample_name(samples[count].name, sizeof(samples[count].name),
            "return", frame_numbers[index]);
        samples[count].phase = return_start; samples[count].requested = requested;
        if (!desktop_capture_at(&capture, target, &samples[count++].frame)) goto done;
    }
    mouse_input(MOUSEEVENTF_LEFTUP);
    trace_path = sample_path(argv[1], "trace", L".csv");
    trace = trace_path ? _wfopen(trace_path, L"w") : NULL;
    if (!trace) goto done;
    fprintf(trace, "frame,phase_ms,capture_mid_ms,requested_ms,capture_end_ms,"
        "press_x,press_y,target_x,target_y,target_drag_x,target_drag_y,"
        "capture_left,capture_top,capture_width,capture_height\n");
    for (index = 0; index < count; ++index) {
        double actual = samples[index].phase ? milliseconds(
            samples[index].frame.present - samples[index].phase) : 0.0;
        double drag_x = 2.0 * (target_x - left) / width - 1.0;
        double drag_y = 1.0 - 2.0 * (target_y - top) / height;
        wchar_t *path = sample_path(argv[1], samples[index].name, L".bmp");
        fprintf(trace, "%s,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%.6f,%.6f,"
            "%d,%d,%d,%d\n", samples[index].name, actual, actual,
            samples[index].requested, actual, press_x, press_y, target_x,
            target_y, drag_x, drag_y, left, top, width, height);
        if (!path || !save_bitmap(path, &samples[index].frame, width, height)) {
            free(path); goto done;
        }
        free(path);
    }
    if (fclose(trace) != 0) { trace = NULL; goto done; }
    trace = NULL;
    wprintf(L"frames=%d output=%ls\n", count, argv[1]);
    result = 0;
done:
    mouse_input(MOUSEEVENTF_LEFTUP);
    if (trace) fclose(trace);
    free(trace_path);
    for (index = 0; index < count; ++index) free(samples[index].frame.pixels);
    desktop_capture_destroy(&capture);
    if (result) fwprintf(stderr, L"desktop capture failed\n");
    return result;
}
