#include "bongo_cat/platform.h"
#include "windows_borderless.h"
#include "windows_capture.h"
#include "windows_layered.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <windows.h>

static HWND native_window(BongoCatPlatform *platform) {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

bool bongo_cat_platform_pointer_local(BongoCatPlatform *platform, double screen_x,
    double screen_y, float *local_x, float *local_y) {
    if (!platform || !local_x || !local_y) return false;
    HWND window = native_window(platform);
    POINT point = {(LONG)screen_x, (LONG)screen_y};
    RECT client;
    if (!window || !ScreenToClient(window, &point) || !GetClientRect(window, &client))
        return false;
    *local_x = (float)point.x; *local_y = (float)point.y;
    return point.x >= client.left && point.x < client.right &&
        point.y >= client.top && point.y < client.bottom;
}

void bongo_cat_platform_set_click_through(BongoCatPlatform *platform,
    bool forced, bool pointer_transparent) {
    HWND window = native_window(platform);
    /*
     * 仅录屏可见时窗口在桌面上完全看不见 (DWM 隐藏), 所以必须整体穿透点击。
     * 这里同样不能走 forced 分支: 那条路会启用 layered presenter —— 它用一个代理
     * 窗口把画面重新显示到桌面上, 正好和"桌面不显示"冲突。
     */
    if (platform->capture_only) {
        bongo_cat_windows_layered_set_click_through(platform, false);
        bongo_cat_windows_borderless_set_click_through(window, true, true);
        return;
    }
    /*
     * 穿透点击不再启用 layered presenter。
     *
     * presenter 的做法是把真实窗口压成完全透明 (SetLayeredWindowAttributes
     * alpha=0), 再用一个代理窗口把画面重新显示出来 —— 代理的帧来自 GL 前缓冲的
     * glReadPixels (windows_layered.c 的 read_frame)。实践中有两个问题:
     *   1) 用户在 OBS 里采集的是那个真实窗口(按标题选), 一开鼠标穿透它就变成全
     *      透明, OBS 里看到的画面就"卡住"不动了 (用户报告);
     *   2) Vulkan 渲染时根本没有 GL 前缓冲可读, 代理窗口永远拿不到新帧。
     * 而真实窗口本身就能被采集 (游戏采集 / 窗口采集), 像素级透明的实现也早就
     * 修好了, 所以这里只做点击穿透, 让窗口保持在正常的合成路径上。
     */
    bongo_cat_windows_layered_set_click_through(platform, false);
    bongo_cat_windows_borderless_set_click_through(window, forced,
        pointer_transparent);
}

void bongo_cat_platform_raise_window(SDL_Window *window) {
    if (!window) return;
    SDL_ShowWindow(window);
    HWND handle = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!handle) return;
    HWND proxy = bongo_cat_windows_layered_proxy(handle);
    if (IsIconic(handle)) ShowWindow(handle, SW_RESTORE);
    BringWindowToTop(handle);
    SetForegroundWindow(handle);
    if (proxy) BringWindowToTop(proxy);
    /* Preferences windows are transparent too, but are not OBS capture
       sources. Running the capture style transaction on them can recreate the
       DWM surface and reintroduce an opaque edge. Only configure windows that
       were explicitly registered as capture sources. */
    if (bongo_cat_windows_capture_is_configured(handle))
        bongo_cat_windows_capture_configure(handle);
    else
        bongo_cat_windows_capture_repair_transparency(handle);
}

bool bongo_cat_platform_set_geometry(BongoCatPlatform *platform,
    int x, int y, int width, int height) {
    if (!platform || !platform->window) return false;
    int current_width, current_height;
    bool size_known = SDL_GetWindowSize(platform->window,
        &current_width, &current_height);
    int current_x, current_y;
    bool position_known = SDL_GetWindowPosition(platform->window,
        &current_x, &current_y);
    bool size_changed = !size_known || current_width != width || current_height != height;
    bool position_changed = !position_known || current_x != x || current_y != y;
    if (!size_changed && !position_changed) return true;
    HWND window = native_window(platform);
    if (!window) return false;
    if (size_changed &&
        !(SDL_GetWindowFlags(platform->window) & SDL_WINDOW_RESIZABLE) &&
        !SDL_SetWindowResizable(platform->window, true))
        return false;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!position_changed) flags |= SWP_NOMOVE;
    if (!size_changed) flags |= SWP_NOSIZE;
    bool preserve_screen = size_changed && position_changed &&
        bongo_cat_windows_borderless_preserve_screen(window, true);
    bool changed = SetWindowPos(window, NULL, position_changed ? x : current_x,
        position_changed ? y : current_y, size_changed ? width : current_width,
        size_changed ? height : current_height,
        flags) != 0;
    if (preserve_screen)
        bongo_cat_windows_borderless_preserve_screen(window, false);
    if (!changed) return false;
    /* Geometry changes retain DWM alpha composition. Reapplying it here
       invalidates the surface on every animation frame. */
    return SDL_SyncWindow(platform->window);
}
#endif
