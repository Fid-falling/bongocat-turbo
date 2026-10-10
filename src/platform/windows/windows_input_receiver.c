#include "windows_input_internal.h"

#ifdef _WIN32
#include <SDL3/SDL_log.h>
/*
 * 接收线程的窗口与消息循环。
 *
 * 用 message-only 窗口(HWND_MESSAGE)而不是普通窗口: 它不进 Z 序、不显示、不参与
 * 激活, 却照样能收到 WM_INPUT 与 WM_INPUT_DEVICE_CHANGE —— 这正是 RIDEV_INPUTSINK
 * 需要的目标窗口。窗口与状态对象的关联放在 GWLP_USERDATA 里: WM_NCCREATE 时写入,
 * WM_NCDESTROY 时清空。
 */
static const wchar_t receiver_class[] = L"BongoCat Raw Input Receiver";

/* 这个窗口过程跑在接收线程上, 因此里面每一件事都必须很快: 解析报文、推事件、
   必要时唤醒主线程。不要在这里加锁等待或做 I/O。 */
static LRESULT CALLBACK receiver_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    WindowsInputState *state = (WindowsInputState *)GetWindowLongPtrW(window,
        GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        state = create->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (state && message == WM_INPUT) {
        RAWINPUT packet = {0};
        UINT bytes = sizeof(packet);
        UINT received = GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &packet,
            &bytes, sizeof(RAWINPUTHEADER));
        if (received == (UINT)-1) {
            DWORD error = GetLastError();
            state->diagnostic_read_failures++;
            if (error != state->read_error) SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                "Raw Input read failed: error=%lu", (unsigned long)error);
            state->read_error = error;
        } else bongo_cat_windows_input_packet(state, &packet, received);
        /* DefWindowProc performs the required foreground WM_INPUT cleanup. */
    /* 设备增删: 设备表按句柄索引, 所以插入和移除都先把该句柄从表里删掉(插入时
       等下一个报文再惰性重建), 并清空当前运动缓存 —— 旧设备的位移不能算到新设备
       头上。 */
    } else if (state && message == WM_INPUT_DEVICE_CHANGE) {
        if (wparam == GIDC_REMOVAL || wparam == GIDC_ARRIVAL) {
            state->diagnostic_device_changes++;
            bongo_cat_windows_input_remove_device(state, (HANDLE)lparam);
            bongo_cat_windows_input_clear_motion(state);
        }
    } else if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* 注册窗口类 + 建 message-only 窗口 + 注册 Raw Input, 三步任一失败都算启动失败。
   类已存在是可接受的: 同一进程里重建接收器时就会遇到。 */
bool bongo_cat_windows_input_receiver_create(WindowsInputState *state) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW type = {.lpfnWndProc = receiver_proc,
        .hInstance = instance, .lpszClassName = receiver_class};
    state->window_class = RegisterClassW(&type);
    if (!state->window_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state->startup_error = GetLastError();
        return false;
    }
    state->window = CreateWindowExW(0, receiver_class, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, instance, state);
    if (!state->window) { state->startup_error = GetLastError(); return false; }
    return bongo_cat_windows_input_register(state);
}

void bongo_cat_windows_input_receiver_destroy(WindowsInputState *state) {
    if (state->window) {
        bongo_cat_windows_input_unregister(state);
        DestroyWindow(state->window);
        state->window = NULL;
    }
    if (state->window_class) {
        UnregisterClassW(receiver_class, GetModuleHandleW(NULL));
        state->window_class = 0;
    }
}

/* 接收线程的主循环体: 有界地抽取消息(原因见下面原有的英文注释: 每批最多 256 条,
   免得高回报率鼠标把关闭流程饿死)。返回 false 表示收到 WM_QUIT 或 stop 事件已
   置位, 调用方应当退出线程。 */
bool bongo_cat_windows_input_dispatch(WindowsInputState *state) {
    MSG message;
    /* Bound each batch so high-rate mice cannot starve shutdown. */
    for (unsigned i = 0; i < 256 && PeekMessageW(&message,
        NULL, 0, 0, PM_REMOVE); ++i) {
        if (message.message == WM_QUIT ||
            WaitForSingleObject(state->stop, 0) == WAIT_OBJECT_0) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}
#endif
