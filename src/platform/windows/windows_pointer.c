#include "bongo_cat/platform.h"
#include "windows_borderless.h"
#include "windows_capture.h"
#include "windows_layered.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <windows.h>

/*
 * 命中测试、点击穿透与窗口几何。所有屏幕坐标在这里换算成客户区坐标, 上层只看
 * 像素坐标。
 *
 * 这个文件是透明窗口几个坑的集中地: 下面几段长注释各自对应一个用户可见的问题
 * (OBS 采集卡住、穿透后点不到猫、改尺寸时窗口闪动), 改动前请先读完。
 */
static HWND native_window(BongoCatPlatform *platform) {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* 屏幕坐标 -> 客户区坐标, 并用返回值说明这个点是否落在窗口内部。返回 false 时
   仍然会写出换算结果(可能是负数或超出宽高)—— 调用方可能正需要它做边缘判定。 */
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

/*
 * 代理窗口的鼠标消息转发与"是否启用代理"的总开关都搬去了 windows_layered_window.c:
 * 轮盘(独立窗口)现在也要装同一套转发, 放在这里就只能给主窗口用一份静态函数。
 * 本文件只负责命中测试与窗口几何。
 */
void bongo_cat_platform_set_click_through(BongoCatPlatform *platform,
    bool forced, bool pointer_transparent) {
    HWND window = native_window(platform);
    /*
     * 仅录屏可见时窗口在桌面上完全看不见 (DWM 隐藏), 所以必须整体穿透点击。
     * 这里同样不能走 forced 分支: 那条路会启用 layered presenter —— 它用一个代理
     * 窗口把画面重新显示到桌面上, 正好和"桌面不显示"冲突。
     */
    if (platform->capture_only) {
        bongo_cat_windows_layered_set_click_through(platform->presenter, false);
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
     *
     * 例外: 打开 BONGO_CAT_GL_TRANSPARENCY=1 时仍然启用代理 —— 普通窗口的 GL alpha
     * 会被 DWM 忽略, 不启用代理 GL 模式就只能是黑底 (见上面 helper 的说明)。
     */
    bool transparent = bongo_cat_windows_layered_requested();
    bongo_cat_windows_layered_set_click_through(platform->presenter, transparent);
    /* 代理模式下真实窗口收不到鼠标消息, 必须在代理窗口上补一条转发, 否则拖不动。 */
    if (transparent) bongo_cat_windows_layered_forward_input(window);
    HWND proxy = bongo_cat_windows_layered_proxy(window);
    bongo_cat_windows_borderless_set_click_through(proxy ? proxy : window, forced,
        pointer_transparent);
}

/* 把窗口抬到最前(托盘菜单或快捷键触发)。存在代理窗口时要一并抬起 —— 它才是
   真正把画面显示出来的那个窗口。末尾对采集属性的处理见下面原有的英文注释:
   只对登记过的采集源重做样式事务, 对设置窗口仅做透明修复。 */
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

/* 移动 / 改尺寸。三处细节: 需要改尺寸时必须临时把窗口置为可调整大小, 否则
   SetWindowPos 不会真的改变尺寸; 只改其中一维时用 SWP_NOMOVE / SWP_NOSIZE 让
   另一维保持不动; preserve_screen 包住整个调用, 免得窗口在改动过程中被"拉回
   屏幕内"的逻辑挪走。 */
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
