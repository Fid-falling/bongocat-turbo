#include "windows_layered.h"
#include "windows_layered_internal.h"
#include "windows_capture.h"
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_properties.h>
#include <stb_image_resize2.h>
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
 * 整条链路是"预乘 alpha": glReadPixels 取回 BGRA, 缩放用 STBIR_BGRA_PM(告诉重采样
 * 器数据已预乘), UpdateLayeredWindow 用 ULW_ALPHA(要求预乘)—— 三者必须一致, 任何
 * 一环改成直通 alpha, 透明边缘就会出现黑边或亮边。
 *
 * 只有强制点击穿透时才启用这套路径(原因见 windows_pointer.c), 平时走 SDL 直接呈现。
 */
static const wchar_t proxy_class[] = L"BongoCat.LayeredPresenter";
static const wchar_t proxy_property[] = L"BongoCat.LayeredProxy";
/* SDL's OpenGL window owns a DC, so a separate layered window presents frames. */
static HWND native_window(BongoCatPlatform *platform) {
    return platform && platform->window ? (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}
void *bongo_cat_windows_layered_create(void) {
    BongoCatWindowsLayered *value = calloc(1, sizeof(*value));
    if (!value) return NULL;
    value->memory_dc = CreateCompatibleDC(NULL);
    if (value->memory_dc) return value;
    free(value);
    return NULL;
}

/* 删除 DIB 之前必须先 SelectObject 换回原位图 —— 还选在 DC 里的位图不能删。
   同时清掉 pixels/width/height, 让 resize_bitmap 下次必定重建。 */
static void release_bitmap(BongoCatWindowsLayered *value) {
    if (!value || !value->bitmap) return;
    SelectObject(value->memory_dc, value->original_bitmap);
    DeleteObject(value->bitmap);
    value->bitmap = NULL;
    value->original_bitmap = NULL;
    value->pixels = NULL;
    value->width = value->height = 0;
}
/* 把源窗口恢复成普通窗口: 去掉 WS_EX_LAYERED / WS_EX_TRANSPARENT 与全透明属性,
   并用 SWP_FRAMECHANGED 让系统重新计算非客户区。恢复后必须重设一次 SDL 的窗口
   不透明度(分层属性会把它盖掉), 并修复 DWM 的透明合成。 */
static void restore_source(BongoCatPlatform *platform) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    HWND source = native_window(platform);
    if (!value || !source || !value->source_transparent) return;
    LONG_PTR style = GetWindowLongPtrW(source, GWL_EXSTYLE);
    SetWindowLongPtrW(source, GWL_EXSTYLE,
        style & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));
    SetWindowPos(source, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    value->source_transparent = false;
    SDL_SetWindowOpacity(platform->window, platform->window_opacity);
    bongo_cat_windows_capture_restore_transparency(source);
    bongo_cat_windows_capture_log(source, "layered-source-restored");
}
void bongo_cat_windows_layered_destroy(BongoCatPlatform *platform) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    if (!value) return;
    HWND source = native_window(platform);
    restore_source(platform);
    if (source) RemovePropW(source, proxy_property);
    if (value->proxy) DestroyWindow(value->proxy);
    release_bitmap(value);
    free(value->readback);
    if (value->memory_dc) DeleteDC(value->memory_dc);
    free(value);
    platform->presenter = NULL;
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
/* 按需创建代理窗口: 以源窗口为 owner(层级与任务栏归属跟着源窗口), 并继承标题和
   大小图标(否则任务栏上会是个无名窗口)。样式固定 TOOLWINDOW | NOACTIVATE |
   TRANSPARENT | LAYERED: 不抢焦点、不接收鼠标。句柄同时写进源窗口的属性, 方便
   其它模块(windows_pointer.c 抬窗口、诊断)按窗口找到它。 */
static bool ensure_proxy(BongoCatPlatform *platform) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    HWND source = native_window(platform);
    if (!value || !source) return false;
    if (value->proxy) return true;
    if (!register_proxy_class()) return false;
    wchar_t title[128] = L"BongoCat - Pet";
    GetWindowTextW(source, title, (int)(sizeof(title) / sizeof(title[0])));
    DWORD style = WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (value->topmost) style |= WS_EX_TOPMOST;
    value->proxy = CreateWindowExW(style, proxy_class, title, WS_POPUP,
        0, 0, 1, 1, source, NULL, GetModuleHandleW(NULL), NULL);
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
/* 把内存 DC 里的整帧提交给 DWM。destination 用源窗口的屏幕位置, 所以窗口每次移动
   后都要重新提交; blend 里的 SourceConstantAlpha 就是整窗不透明度, 提交成功后记进
   applied_alpha, 好让 set_opacity 跳过"值没变"的重复提交。
   失败细节交给 SDL_SetError, 上层只看到 false 加一条错误信息。 */
bool bongo_cat_windows_layered_update_proxy(BongoCatPlatform *platform,
    BongoCatWindowsLayered *value) {
    HWND source_window = native_window(platform);
    RECT bounds;
    if (!source_window || !value || !value->proxy || !value->bitmap ||
        !GetWindowRect(source_window, &bounds))
        return SDL_SetError("Cannot locate the Windows layered window");
    POINT destination = {bounds.left, bounds.top};
    POINT source = {0, 0};
    SIZE size = {value->width, value->height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0,
        (BYTE)(platform->window_opacity * 255.0f + 0.5f), AC_SRC_ALPHA};
    HDC screen = GetDC(NULL);
    bool presented = screen && UpdateLayeredWindow(value->proxy, screen,
        &destination, &size, value->memory_dc, &source, 0, &blend,
        ULW_ALPHA) != FALSE;
    DWORD failure = presented ? ERROR_SUCCESS : GetLastError();
    if (screen) ReleaseDC(NULL, screen);
    if (!presented) return SDL_SetError(
        "UpdateLayeredWindow failed (%lu)", (unsigned long)failure);
    value->applied_alpha = blend.SourceConstantAlpha;
    value->applied_alpha_valid = true;
    return true;
}

/* 把源窗口压成全透明(alpha=0), 桌面上就只剩代理窗口的画面。
   LWA_ALPHA 失败时必须先把样式改回去再返回 false —— 半途而废会让窗口既不是普通
   窗口也不透明, 用户会直接看到一块黑。 */
static bool make_source_transparent(BongoCatPlatform *platform) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    HWND source = native_window(platform);
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

/* 切换呈现路径。这个函数会被反复调用, 所以只在模式真的变化时才动窗口(mode_logged
   与 active 一起做记忆); 打开时先隐藏代理窗口(等第一帧成功后再显示), 关闭时直接
   恢复源窗口。 */
void bongo_cat_windows_layered_set_click_through(
    BongoCatPlatform *platform, bool enabled) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    if (!value || (value->mode_logged && value->active == enabled)) return;
    value->mode_logged = true;
    value->active = enabled;
    SDL_Log("Windows presentation path: forced_click_through=%d", enabled);
    if (enabled) {
        value->has_frame = false;
        ensure_proxy(platform);
        if (value->proxy) ShowWindow(value->proxy, SW_HIDE);
        bongo_cat_windows_capture_log(native_window(platform),
            "layered-presenter-ready");
    } else {
        if (value->proxy) ShowWindow(value->proxy, SW_HIDE);
        restore_source(platform);
        bongo_cat_windows_capture_log(native_window(platform),
            "direct-opengl-presenter");
    }
}

void bongo_cat_windows_layered_set_always_on_top(
    BongoCatPlatform *platform, bool enabled) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
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

/* 尺寸没变就复用现有 DIB。biHeight 用正值 = 自下而上的行序, 与 glReadPixels 的
   输出方向一致, 因此拷进去之后不需要翻转。 */
static bool resize_bitmap(BongoCatWindowsLayered *value, int width, int height) {
    if (value->bitmap && value->width == width && value->height == height) return true;
    release_bitmap(value);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = NULL;
    value->bitmap = CreateDIBSection(value->memory_dc, &info,
        DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!value->bitmap || !pixels) {
        if (value->bitmap) DeleteObject(value->bitmap);
        value->bitmap = NULL;
        return false;
    }
    value->original_bitmap = SelectObject(value->memory_dc, value->bitmap);
    value->pixels = pixels;
    value->width = width;
    value->height = height;
    return true;
}

/*
 * 抓取 GL 前缓冲。渲染尺寸与窗口尺寸相同时直接读进 pixels; 不同时(高 DPI 缩放、
 * supersample)先读进 readback 再缩放 —— readback_valid 就是"这一帧走了缩放路径"
 * 的标记, 逐像素 alpha 查询要靠它决定读哪块内存。
 * 缩放必须按预乘 alpha 处理(见文件头的不变量): 按预乘值插值, 透明边缘才不会出现
 * 深色或亮色描边。
 */
static bool read_frame(BongoCatWindowsLayered *value, int width, int height,
    int display_width, int display_height) {
    value->source_width = width; value->source_height = height;
    value->readback_valid = width != display_width || height != display_height;
    if (!value->readback_valid) {
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, value->pixels);
        return true;
    }
    size_t bytes = (size_t)width * (size_t)height * 4;
    if (width <= 0 || height <= 0 ||
        bytes / 4 != (size_t)width * (size_t)height) return false;
    if (value->readback_capacity < bytes) {
        unsigned char *next = realloc(value->readback, bytes);
        if (!next) return false;
        value->readback = next;
        value->readback_capacity = bytes;
    }
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, value->readback);
    return stbir_resize_uint8_linear(value->readback, width, height, width * 4,
        value->pixels, display_width, display_height, display_width * 4,
        STBIR_BGRA_PM) != NULL;
}

/*
 * 呈现入口(平台层的 present)。没有启用代理模式时就是普通的 SDL_GL_SwapWindow。
 * 启用时: 源窗口不可见或被最小化就直接返回 false —— 此时拿不到画面, 也不该让代理
 * 窗口显示一帧旧内容(隐藏状态没有可暴露的帧, 由启动路径先显示窗口再重试)。
 * 任何一步失败都会清掉 has_frame, 避免 set_opacity 误以为"已经提交过同 alpha 的
 * 帧"而跳过重提交。
 */
bool bongo_cat_platform_present(BongoCatPlatform *platform, int width, int height) {
    if (!platform || !platform->window) return false;
    BongoCatWindowsLayered *value = platform->presenter;
    if (!value || !value->active) return SDL_GL_SwapWindow(platform->window);
    HWND source = native_window(platform);
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
    if (!ensure_proxy(platform) || width <= 0 || height <= 0 ||
        display_width <= 0 || display_height <= 0 ||
        !resize_bitmap(value, display_width, display_height) ||
        !read_frame(value, width, height, display_width, display_height)) {
        value->has_frame = false;
        return SDL_SetError("Cannot allocate the Windows layered frame");
    }
    if (!bongo_cat_windows_layered_update_proxy(platform, value)) {
        value->has_frame = false;
        return false;
    }
    value->has_frame = true;
    if (!make_source_transparent(platform))
        return SDL_SetError("Cannot suppress the OpenGL source window");
    ShowWindow(value->proxy, SW_SHOWNOACTIVATE);
    return true;
}
#endif
