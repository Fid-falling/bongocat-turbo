#include "windows_capture.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shobjidl.h>

static UINT taskbar_created_message;
static UINT capture_refresh_message;
static const wchar_t capture_property[] = L"BongoCat.CaptureWindow";
static bool removal_warning_emitted;
static bool style_warning_emitted;
#define BONGO_CAT_CAPTURE_REFRESH_TIMER ((UINT_PTR)0xBC51)

/* 用窗口属性(capture_property)标记"这个窗口被登记为采集源": 后面的样式事务、
   任务栏刷新、以及窗口过程里的消息分发都以它为准, 于是没登记过的窗口(设置窗口等)
   不会被误改样式。 */
static bool has_property(HWND window, const wchar_t *name) {
    return window && name && GetPropW(window, name) != NULL;
}

bool bongo_cat_windows_capture_is_configured(HWND window) {
    return has_property(window, capture_property);
}

/* GetWindowLongPtr 返回 0 既可能是"样式本来就是 0"也可能是"调用失败", 必须配合
   SetLastError(ERROR_SUCCESS) + GetLastError 才能区分; SetWindowLongPtr 同理(它返回
   旧值, 成功时旧值也可能为 0)。这两个包装函数把这条容易写错的规则固定下来。 */
static bool read_extended_style(HWND window, LONG_PTR *style) {
    SetLastError(ERROR_SUCCESS);
    LONG_PTR value = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!value && GetLastError() != ERROR_SUCCESS) return false;
    *style = value;
    return true;
}

static bool write_extended_style(HWND window, LONG_PTR style, DWORD *error) {
    SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = SetWindowLongPtrW(window, GWL_EXSTYLE, style);
    DWORD result = GetLastError();
    if (error) *error = result;
    return previous || result == ERROR_SUCCESS;
}

/* 两条自定义消息都只注册一次并缓存: TaskbarCreated 是资源管理器重启时广播的系统
   消息(名字必须完全一致才收得到), 另一条是我们自己用来异步安排任务栏刷新的。 */
static void register_messages(void) {
    if (!taskbar_created_message)
        taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    if (!capture_refresh_message)
        capture_refresh_message = RegisterWindowMessageW(
            L"BongoCat.CaptureWindow.RefreshTaskbar");
}

/* 用 ITaskbarList::DeleteTab 把窗口从任务栏和 Alt+Tab 列表里摘掉。
   CoInitializeEx 返回 RPC_E_CHANGED_MODE 是可接受的: 说明本线程已经用别的套间模式
   初始化过 COM, 仍然可以创建对象, 但不该由我们 CoUninitialize。 */
static HRESULT remove_taskbar_tab(HWND window) {
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return initialized;
    ITaskbarList *taskbar = NULL;
    HRESULT result = CoCreateInstance(&CLSID_TaskbarList, NULL,
        CLSCTX_INPROC_SERVER, &IID_ITaskbarList, (void **)&taskbar);
    if (SUCCEEDED(result)) result = taskbar->lpVtbl->HrInit(taskbar);
    if (SUCCEEDED(result)) result = taskbar->lpVtbl->DeleteTab(taskbar, window);
    if (taskbar) taskbar->lpVtbl->Release(taskbar);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

/* 只有 video 日志级别为 debug 时才收集这些信息(每次都要查 DWM 和窗口属性, 不便宜)。
   输出包含 cloaked(是否被 DWM 隐藏)、display affinity(是否禁止截屏)、样式与 owner
   —— 排查"OBS 抓不到"时这几个字段最关键。 */
void bongo_cat_windows_capture_log(HWND window, const char *stage) {
    if (!window || !IsWindow(window)) return;
    if (SDL_GetLogPriority(SDL_LOG_CATEGORY_VIDEO) > SDL_LOG_PRIORITY_DEBUG) return;
    RECT bounds = {0};
    DWORD cloaked = 0, affinity = 0;
    HRESULT cloak_result = DwmGetWindowAttribute(window, DWMWA_CLOAKED,
        &cloaked, sizeof(cloaked));
    bool affinity_known = GetWindowDisplayAffinity(window, &affinity) != FALSE;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    GetWindowRect(window, &bounds);
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
        "Windows capture state (%s): hwnd=%p visible=%d iconic=%d "
        "cloaked=%lu cloak_known=%d rect=%ld,%ld %ldx%ld style=0x%llx "
        "exstyle=0x%llx owner=%p affinity=0x%lx affinity_known=%d",
        stage ? stage : "unknown", (void *)window,
        IsWindowVisible(window) != FALSE, IsIconic(window) != FALSE,
        (unsigned long)cloaked, SUCCEEDED(cloak_result),
        (long)bounds.left, (long)bounds.top,
        (long)(bounds.right - bounds.left),
        (long)(bounds.bottom - bounds.top),
        (unsigned long long)style, (unsigned long long)extended,
        (void *)GetWindowLongPtrW(window, GWLP_HWNDPARENT),
        (unsigned long)affinity, affinity_known);
}

static void refresh_taskbar(HWND window) {
    if (!window) return;
    HRESULT result = remove_taskbar_tab(window);
    if (FAILED(result) && IsWindowVisible(window) && !removal_warning_emitted) {
        removal_warning_emitted = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Cannot remove the capture window from the taskbar (0x%08lx)",
            (unsigned long)result);
    }
}

/* 立刻投递一次自定义消息, 同时起一个 250ms 的一次性定时器兜底: 资源管理器重启期间
   消息可能投不进去, 定时器保证最终还会再试一次。 */
static void schedule_refresh(HWND window) {
    register_messages();
    if (!window) return;
    if (capture_refresh_message)
        PostMessageW(window, capture_refresh_message, 0, 0);
    SetTimer(window, BONGO_CAT_CAPTURE_REFRESH_TIMER, 250, NULL);
}

/*
 * 把窗口配置成可被 OBS 发现: 先打上标记, 再去掉 WS_EX_TOOLWINDOW。
 * 去掉样式时窗口会被隐藏再显示(原因见下面原有的英文注释: 不引入 APPWINDOW, 也尽量
 * 避免普通 SDL 窗口的隐藏/显示切换), 因为这个改动会重建 DWM 重定向表面。
 * 结尾检查两个会让窗口采集彻底不可用的样式位(TOOLWINDOW / NOREDIRECTIONBITMAP), 用
 * 返回值表示"是否配置成功"; 失败只警告一次, 不反复刷日志。
 * 这个函数是幂等的, 会被反复调用(见 windows_pointer.c), 所以"样式没变"的路径上也
 * 要执行透明修复与任务栏刷新。
 */
bool bongo_cat_windows_capture_configure(HWND window) {
    if (!window || !IsWindow(window)) return false;
    register_messages();
    SetPropW(window, capture_property, (HANDLE)1);
    LONG_PTR style = 0;
    if (!read_extended_style(window, &style)) {
        if (!style_warning_emitted) {
            style_warning_emitted = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Cannot read the window style required for OBS discovery");
        }
        bongo_cat_windows_capture_repair_transparency(window);
        return false;
    }
    /* OBS does not require APPWINDOW. Avoid a hide/show style transition for
       ordinary SDL windows because it can recreate their DWM redirection. */
    LONG_PTR next = style & ~WS_EX_TOOLWINDOW;
    if (next != style) {
        bool shown = IsWindowVisible(window) != FALSE;
        if (shown) ShowWindow(window, SW_HIDE);
        DWORD style_error = ERROR_SUCCESS;
        if (write_extended_style(window, next, &style_error)) {
            SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (!style_warning_emitted) {
            style_warning_emitted = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Cannot update the window style required for OBS discovery "
                "(Win32 error %lu, old=0x%llx, new=0x%llx)",
                (unsigned long)style_error, (unsigned long long)style,
                (unsigned long long)next);
        }
        if (shown) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
        }
    }
    /* Style/frame changes can recreate the DWM redirection surface. Repair
       after the complete hide/show transaction, and on the idempotent path
       as well so repeated configure calls remain safe. */
    bongo_cat_windows_capture_repair_transparency(window);
    LONG_PTR applied = 0;
    bool ready = read_extended_style(window, &applied) &&
        !(applied & (WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP));
    if (!ready && !style_warning_emitted) {
        style_warning_emitted = true;
        if (applied & WS_EX_NOREDIRECTIONBITMAP)
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Window capture is incompatible with WS_EX_NOREDIRECTIONBITMAP "
                "(exstyle=0x%llx)", (unsigned long long)applied);
        else
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Cannot configure the window for OBS discovery");
    }
    refresh_taskbar(window);
    schedule_refresh(window);
    return ready;
}

/* 采集相关消息的分发。返回 true 表示消息已被吃掉, 调用方不要再交给原窗口过程。
   顺序有讲究: 透明消息优先(它对 WM_ERASEBKGND 之类有硬性要求), 然后才是我们自己的
   两条刷新消息与 TaskbarCreated 广播; TaskbarCreated 只对登记过的窗口返回 true。 */
bool bongo_cat_windows_capture_handle_message(
    HWND window, UINT message, WPARAM wparam) {
    register_messages();
    if (bongo_cat_windows_capture_handle_transparency_message(
            window, message, wparam)) return true;
    if (capture_refresh_message && message == capture_refresh_message) {
        if (!has_property(window, capture_property)) return false;
        refresh_taskbar(window);
        return true;
    }
    if (message == WM_TIMER &&
        wparam == BONGO_CAT_CAPTURE_REFRESH_TIMER &&
        has_property(window, capture_property)) {
        KillTimer(window, BONGO_CAT_CAPTURE_REFRESH_TIMER);
        refresh_taskbar(window);
        return true;
    }
    if (taskbar_created_message && message == taskbar_created_message) {
        bool capture_window = has_property(window, capture_property);
        if (capture_window) {
            refresh_taskbar(window);
            schedule_refresh(window);
        }
        bongo_cat_windows_capture_repair_transparency(window);
        return capture_window;
    }
    return false;
}
#endif
