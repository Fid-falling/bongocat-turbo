#include "windows_layered.h"
#include "bongo_cat/log.h"
#include "windows_layered_internal.h"
#include "windows_capture.h"
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <stdlib.h>
#include <windows.h>
/*
 * 分层窗口呈现器: 用代理窗口把 GL 画面按像素透明地贴到桌面上。
 *
 * 为什么需要代理窗口: SDL 的 OpenGL 窗口自带一个 DC, 而 UpdateLayeredWindow 必须由
 * 拥有窗口的线程用内存 DC 提交, 直接在 GL 窗口上做会把 GL 的呈现路径顶掉。所以做法
 * 是: 把真实(源)窗口压成全透明(SetLayeredWindowAttributes alpha=0), 再用一个
 * WS_EX_LAYERED 的代理窗口把抓到的帧显示出来。
 *
 * 本文件负责窗口与模式(建/销毁、真实窗口的压制与恢复、代理窗口、每帧入口); 像素通路
 * (抓帧 + 提交)在 windows_layered_frame.c, 平台适配在 windows_layered_state.c。
 *
 * 实例是按窗口持有的: 主窗口一份, 轮盘一份(见 windows_layered_window.c), 所以这里
 * 不出现 BongoCatPlatform, 也不假定全进程只有一个代理窗口 —— 代理句柄按源窗口属性存,
 * 各窗口互不干扰。
 *
 * 只有强制点击穿透时才启用这套路径(原因见 windows_pointer.c), 平时走 SDL 直接呈现。
 */
static const wchar_t proxy_class[] = L"BongoCat.LayeredPresenter";
static const wchar_t proxy_property[] = L"BongoCat.LayeredProxy";
/* SDL's OpenGL window owns a DC, so a separate layered window presents frames. */
HWND bongo_cat_windows_layered_window_handle(SDL_Window *window) {
    return window ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}
HWND bongo_cat_windows_layered_native_window(const BongoCatWindowsLayered *value) {
    return value ? bongo_cat_windows_layered_window_handle(value->window) : NULL;
}
void *bongo_cat_windows_layered_create(SDL_Window *window) {
    if (!window) return NULL;
    BongoCatWindowsLayered *value = calloc(1, sizeof(*value));
    if (!value) return NULL;
    value->window = window;
    /* 不透明度的初值必须与 windows.c 里 platform->window_opacity 的初值一致, 否则
       第一次"恢复源窗口"会把窗口设成另一个亮度。 */
    value->opacity = 1.0f;
    value->memory_dc = CreateCompatibleDC(NULL);
    if (value->memory_dc) return value;
    free(value);
    return NULL;
}

/* 把源窗口恢复成普通窗口: 去掉 WS_EX_LAYERED / WS_EX_TRANSPARENT 与全透明属性,
   并用 SWP_FRAMECHANGED 让系统重新计算非客户区。恢复后必须重设一次 SDL 的窗口
   不透明度(分层属性会把它盖掉), 并修复 DWM 的透明合成。 */
static void restore_source(BongoCatWindowsLayered *value) {
    HWND source = bongo_cat_windows_layered_native_window(value);
    if (!value || !source || !value->source_transparent) return;
    LONG_PTR style = GetWindowLongPtrW(source, GWL_EXSTYLE);
    SetWindowLongPtrW(source, GWL_EXSTYLE,
        style & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));
    SetWindowPos(source, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    value->source_transparent = false;
    SDL_SetWindowOpacity(value->window, value->opacity);
    bongo_cat_windows_capture_restore_transparency(source);
    bongo_cat_windows_capture_log(source, "layered-source-restored");
}
void bongo_cat_windows_layered_destroy(void *presenter) {
    BongoCatWindowsLayered *value = presenter;
    if (!value) return;
    HWND source = bongo_cat_windows_layered_native_window(value);
    restore_source(value);
    /* 源窗口可能已经先一步被销毁(调用方的清理顺序不保证), 所以属性和代理窗口都要
       各自判一次有效性 —— 对死句柄调 RemovePropW/DestroyWindow 只会静默失败。 */
    if (source && IsWindow(source)) RemovePropW(source, proxy_property);
    if (value->proxy && IsWindow(value->proxy)) DestroyWindow(value->proxy);
    bongo_cat_windows_layered_release_frame(value);
    free(value->readback);
    if (value->memory_dc) DeleteDC(value->memory_dc);
    free(value);
}
/* 代理窗口类只注册一次; 已经存在就直接复用。窗口过程用 DefWindowProcW: 代理窗口
   不接收输入(TRANSPARENT + NOACTIVATE), 不需要自己的消息处理。 */
static bool register_proxy_class(void) {
    WNDCLASSEXW existing = {.cbSize = sizeof(existing)};
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (GetClassInfoExW(instance, proxy_class, &existing)) return true;
    WNDCLASSEXW type = {0};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    type.lpszClassName = proxy_class;
    return RegisterClassExW(&type) != 0;
}
/* 按需创建代理窗口: 以 proxy_owner(默认源窗口) 为 owner, 层级与任务栏归属跟着它, 并
   继承标题和大小图标(否则任务栏上会是个无名窗口)。样式固定 TOOLWINDOW | NOACTIVATE |
   TRANSPARENT | LAYERED: 不抢焦点、不接收鼠标。句柄同时写进源窗口的属性, 方便
   其它模块(windows_pointer.c 抬窗口、诊断)按窗口找到它。 */
static bool ensure_proxy(BongoCatWindowsLayered *value) {
    HWND source = bongo_cat_windows_layered_native_window(value);
    if (!value || !source) return false;
    if (value->proxy) return true;
    if (!register_proxy_class()) return false;
    HWND owner = value->proxy_owner ? value->proxy_owner : source;
    wchar_t title[128] = L"BongoCat - Pet";
    GetWindowTextW(source, title, (int)(sizeof(title) / sizeof(title[0])));
    DWORD style = WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (value->topmost) style |= WS_EX_TOPMOST;
    value->proxy = CreateWindowExW(style, proxy_class, title, WS_POPUP,
        0, 0, 1, 1, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!value->proxy) return false;
    HICON large = (HICON)SendMessageW(source, WM_GETICON, ICON_BIG, 0);
    HICON small_icon = (HICON)SendMessageW(source, WM_GETICON, ICON_SMALL, 0);
    if (large) SendMessageW(value->proxy, WM_SETICON, ICON_BIG, (LPARAM)large);
    if (small_icon)
        SendMessageW(value->proxy, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    SetPropW(source, proxy_property, value->proxy);
    bongo_cat_windows_capture_log(value->proxy, "layered-proxy-created");
    return true;
}

/* 把源窗口压成全透明(alpha=0), 桌面上就只剩代理窗口的画面。
   LWA_ALPHA 失败时必须先把样式改回去再返回 false —— 半途而废会让窗口既不是普通
   窗口也不透明, 用户会直接看到一块黑。 */
static bool make_source_transparent(BongoCatWindowsLayered *value) {
    HWND source = bongo_cat_windows_layered_native_window(value);
    if (!value || !source) return false;
    if (value->source_transparent) return true;
    LONG_PTR style = GetWindowLongPtrW(source, GWL_EXSTYLE);
    SetWindowLongPtrW(source, GWL_EXSTYLE,
        style | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    if (!SetLayeredWindowAttributes(source, 0, 0, LWA_ALPHA)) {
        SetWindowLongPtrW(source, GWL_EXSTYLE, style);
        return false;
    }
    SetWindowPos(source, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    value->source_transparent = true;
    bongo_cat_windows_capture_log(source, "layered-source-suppressed");
    return true;
}

/* 切换呈现路径(不含日志, 幂等)。这个函数会被反复调用, 所以只在模式真的变化时才动
   窗口(mode_logged 与 active 一起做记忆); 打开时先隐藏代理窗口(等第一帧成功后再显示),
   关闭时直接恢复源窗口。
   调用方直接用它 = 不往日志里写"呈现路径"那一行: 轮盘每次打开都会新建一个呈现器实例,
   而日志里那行是给主窗口看的(轮盘有自己的"轮盘呈现路径"), 否则每开一次菜单都像开关翻转。 */
void bongo_cat_windows_layered_apply_mode(BongoCatWindowsLayered *value,
    bool enabled) {
    if (!value || (value->mode_logged && value->active == enabled)) return;
    value->mode_logged = true;
    value->active = enabled;
    if (enabled) {
        value->has_frame = false;
        ensure_proxy(value);
        if (value->proxy) ShowWindow(value->proxy, SW_HIDE);
        bongo_cat_windows_capture_log(
            bongo_cat_windows_layered_native_window(value),
            "layered-presenter-ready");
    } else {
        if (value->proxy) ShowWindow(value->proxy, SW_HIDE);
        restore_source(value);
        bongo_cat_windows_capture_log(
            bongo_cat_windows_layered_native_window(value),
            "direct-opengl-presenter");
    }
}

/* 主窗口的呈现路径开关 = 唯一会写那条诊断日志的入口(见上面 apply_mode 的说明)。 */
void bongo_cat_windows_layered_set_click_through(
    void *presenter, bool enabled) {
    BongoCatWindowsLayered *value = presenter;
    if (!value || (value->mode_logged && value->active == enabled)) return;
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[probe] 呈现路径: 代理分层=%d (0=直接呈现)", enabled);
    bongo_cat_windows_layered_apply_mode(value, enabled);
}

void bongo_cat_windows_layered_set_always_on_top(
    void *presenter, bool enabled) {
    BongoCatWindowsLayered *value = presenter;
    if (!value) return;
    value->topmost = enabled;
    if (!value->proxy) return;
    SetWindowPos(value->proxy, enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* 其它模块只能通过这个入口拿到代理窗口(句柄就存在源窗口的属性里)。 */
HWND bongo_cat_windows_layered_proxy(HWND source) {
    return source ? (HWND)GetPropW(source, proxy_property) : NULL;
}

/*
 * 呈现入口(平台层的 present)。没有启用代理模式时就是普通的 SDL_GL_SwapWindow。
 * 启用时: 源窗口不可见或被最小化就直接返回 false —— 此时拿不到画面, 也不该让代理
 * 窗口显示一帧旧内容(隐藏状态没有可暴露的帧, 由启动路径先显示窗口再重试)。
 * 任何一步失败都会清掉 has_frame, 避免 set_opacity 误以为"已经提交过同 alpha 的
 * 帧"而跳过重提交。
 *
 * 代价提示: 这里每调用一次就是一次整窗 glReadPixels(GPU->CPU 同步) + 一次
 * UpdateLayeredWindow 拷贝, 是这条路径上最贵的一步 —— 调用方必须只在画面真的变了
 * 的时候调它(轮盘就是用 d->dirty 把关的, 静止时一次都不回读)。
 */
bool bongo_cat_windows_layered_present(void *presenter, int width, int height) {
    BongoCatWindowsLayered *value = presenter;
    if (!value || !value->window) return false;
    if (!value->active) return SDL_GL_SwapWindow(value->window);
    HWND source = bongo_cat_windows_layered_native_window(value);
    if (!source || !value->visible || !IsWindowVisible(source) || IsIconic(source)) {
        if (value->proxy) ShowWindow(value->proxy, SW_HIDE);
        /* A hidden source has no frame to expose through the proxy. Let the
           startup path show it before retrying the first presentation. */
        return false;
    }
    RECT bounds;
    if (!GetWindowRect(source, &bounds)) return false;
    int display_width = bounds.right - bounds.left,
        display_height = bounds.bottom - bounds.top;
    if (!ensure_proxy(value) || width <= 0 || height <= 0 ||
        display_width <= 0 || display_height <= 0 ||
        !bongo_cat_windows_layered_capture(value, width, height,
            display_width, display_height)) {
        value->has_frame = false;
        return SDL_SetError("Cannot allocate the Windows layered frame");
    }
    /* 菜单窗口(模态)要求整块矩形都能被点到; 主窗口不设这个标记, 保持"透明处点穿"。
       必须在提交之前改像素 —— UpdateLayeredWindow 拷走的是此刻的内存 DC。 */
    if (value->solid_hit_area) bongo_cat_windows_layered_solidify_hit_area(value);
    if (!bongo_cat_windows_layered_update_proxy(value)) {
        value->has_frame = false;
        return false;
    }
    value->has_frame = true;
    if (!make_source_transparent(value))
        return SDL_SetError("Cannot suppress the OpenGL source window");
    ShowWindow(value->proxy, SW_SHOWNOACTIVATE);
    return true;
}
#endif
