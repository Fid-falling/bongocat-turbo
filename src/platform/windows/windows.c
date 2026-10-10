#include "bongo_cat/platform.h"
#include "windows_borderless.h"
#include "windows_capture.h"
#include "windows_input.h"
#include "windows_layered.h"
#include "windows_startup.h"
/*
 * Windows 平台层的装配点: 生命周期(init / shutdown)、窗口层级, 以及把各个子模块
 * 接到 platform.h 的统一接口上。
 *
 * 职责边界: 这里只做"接线", 真正的实现在同目录的 windows_*.c 里。初始化顺序是
 * 刻意的(先建 presenter, 再注册唤醒事件, 再启动输入, 最后才动窗口样式与采集属性),
 * 每一步失败都必须回到 shutdown 能安全处理的状态。
 */
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <string.h>
#include <windows.h>
static HWND native_window(BongoCatPlatform *platform) {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* SDL 的消息钩子: 给采集模块一个机会吃掉它自己关心的消息(它按窗口属性过滤, 只对
   登记过的采集源生效)。返回 false 表示这条消息不再交给 SDL 处理。 */
static bool SDLCALL windows_message_hook(void *userdata, MSG *message) {
    (void)userdata;
    if (!message || !message->hwnd) return true;
    /* The capture handler filters by its per-window properties. Returning
       false only consumes the refresh/timer messages that it owns. */
    return !bongo_cat_windows_capture_handle_message(message->hwnd,
        message->message, message->wParam);
}

/* 设置窗口也走透明路径, 但必须先显式登记(mark_transparent): 采集模块靠这个标记
   决定要不要对它做样式事务。透明窗口还要装自己的窗口过程并立刻修复一次合成,
   否则第一帧会闪出不透明的底。 */
void bongo_cat_platform_configure_preferences_window(SDL_Window *window) {
    if (!window) return;
    HWND handle = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    bool transparent = (SDL_GetWindowFlags(window) &
        SDL_WINDOW_TRANSPARENT) != 0;
    bongo_cat_windows_capture_mark_transparent(handle, transparent);
    if (transparent) {
        bongo_cat_windows_capture_install_transparency_handler(handle);
        bongo_cat_windows_capture_repair_transparency(handle);
    }
}

/*
 * 平台初始化。顺序不能随意调换:
 *   1) 先建 presenter —— 后面的窗口设置都可能用到它;
 *   2) 预留一个 SDL 用户事件作为输入线程的唤醒信号, 拿不到就直接失败(否则输入
 *      线程无法安全退出);
 *   3) 启动输入采集, 失败只记警告: 没有全局输入时应用仍然能跑, 只是猫不会动;
 *   4) 最后才改窗口标题 / 样式 / 采集属性 —— 这些操作会触发消息, 前面几步必须
 *      已经就绪。
 */
BongoCatResult bongo_cat_platform_init(BongoCatPlatform *platform, SDL_Window *window,
    BongoCatInputState *input, BongoCatError *error) {
    if (!platform || !window || !input) return BONGO_CAT_ERROR_ARGUMENT;
    memset(platform, 0, sizeof(*platform));
    platform->window = window; platform->input = input;
    platform->window_opacity = 1.0f; platform->presenter = bongo_cat_windows_layered_create();
    if (!platform->presenter) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate the Windows layered presenter");
        return BONGO_CAT_ERROR_MEMORY;
    }
    platform->wake_event_type = SDL_RegisterEvents(1);
    if (platform->wake_event_type == (Uint32)-1) {
        bongo_cat_windows_layered_destroy(platform);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot reserve the Windows input wake event");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    if (!bongo_cat_windows_input_start(platform)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Raw Input is unavailable; the window will continue without global input");
    }
    HWND hwnd = native_window(platform);
    SetWindowTextW(hwnd, bongo_cat_windows_instance_title());
    bongo_cat_windows_borderless_install(hwnd);
    bool transparent = (SDL_GetWindowFlags(window) &
        SDL_WINDOW_TRANSPARENT) != 0;
    /* Mark transparency before any OBS style transaction. Removing
       WS_EX_TOOLWINDOW may recreate the DWM surface, so the configure path
       must be able to repair it before the window is first shown. */
    bongo_cat_windows_capture_mark_transparent(hwnd, transparent);
    bongo_cat_windows_capture_configure(hwnd);
    SDL_SetWindowsMessageHook(windows_message_hook, NULL);
    if (!SDL_SetWindowResizable(window, true)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Borderless resize is unavailable: %s", SDL_GetError());
    }
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE |
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    bongo_cat_windows_capture_repair_transparency(hwnd);
    bongo_cat_windows_capture_log(hwnd, "initialized");
    return BONGO_CAT_OK;
}
/* 卸载顺序与 init 相反: 先摘掉窗口过程(不再有新消息进来), 再停输入线程, 最后销毁
   presenter; 消息钩子也必须摘掉, 否则 SDL 还会往已销毁的窗口上派发消息。 */
void bongo_cat_platform_shutdown(BongoCatPlatform *platform) {
    if (!platform) return;
    HWND window = native_window(platform);
    if (window) bongo_cat_windows_borderless_uninstall(window);
    bongo_cat_windows_input_stop(platform);
    bongo_cat_windows_layered_destroy(platform);
    SDL_SetWindowsMessageHook(NULL, NULL);
}

/* "不置顶"时把窗口挂到桌面 WorkerW 之下(而不是简单取消 topmost), 于是它能待在
   桌面图标层之上、其它窗口之下。Progman 的 WorkerW 结构各 Windows 版本不同, 所以
   先找带 SHELLDLL_DefView 的那个 WorkerW, 找不到再退回 Progman。 */
static HWND desktop_anchor(void) {
    HWND program_manager = FindWindowW(L"Progman", NULL);
    if (!program_manager) return NULL;
    HWND worker = NULL;
    while ((worker = FindWindowExW(NULL, worker, L"WorkerW", NULL)) != NULL)
        if (FindWindowExW(worker, NULL, L"SHELLDLL_DefView", NULL))
            return worker;
    return program_manager;
}

/* 改 owner(层级的实现手段)会让系统重新摆放窗口, 所以调用前后各记一次位置, 变了
   就挪回去 —— 否则用户每次切换"置顶", 猫都会跳一下。 */
static void apply_window_level(HWND window, bool topmost) {
    if (!window) return;
    RECT before = {0};
    bool positioned = GetWindowRect(window, &before) != FALSE;
    HWND owner = topmost ? NULL : desktop_anchor();
    SetWindowLongPtrW(window, GWLP_HWNDPARENT, (LONG_PTR)owner);
    SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
        SWP_FRAMECHANGED);
    if (!topmost)
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (positioned) {
        RECT after = {0};
        if (GetWindowRect(window, &after) &&
            (after.left != before.left || after.top != before.top))
            SetWindowPos(window, NULL, before.left, before.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* 置顶要走三条路: SDL 的属性(它自己也要记账)、apply_window_level(真正改 owner 与
   层级)、以及代理窗口(如果存在, 它才是显示画面的那个)。最后重新配置一次采集
   属性, 因为改层级会重建 DWM 的合成表面。 */
void bongo_cat_platform_set_always_on_top(BongoCatPlatform *platform, bool enabled) {
    if (!platform || !platform->window) return;
    bool applied = SDL_SetWindowAlwaysOnTop(platform->window, enabled);
    HWND window = native_window(platform);
    if (!applied && window)
        SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    apply_window_level(window, enabled);
    bongo_cat_windows_layered_set_always_on_top(platform, enabled);
    bongo_cat_windows_capture_configure(window);
    bongo_cat_windows_capture_log(window, "window-level");
}
/* 拖动交给 windows_borderless.c 的模态拖动(原因见那里: 无边框窗口只能借用系统的
   模态消息循环才能拿到系统级拖动)。 */
void bongo_cat_platform_begin_drag(BongoCatPlatform *platform,
    BongoCatModalTick modal_tick, void *userdata) {
    HWND hwnd = native_window(platform);
    bongo_cat_windows_begin_drag(hwnd, modal_tick, userdata);
}
/* Windows 支持按像素判定命中(见 windows_pointer.c), 所以固定返回 true。 */
bool bongo_cat_platform_dynamic_hit_supported(void) { return true; }
/* 下面四个函数是给"鼠标被别的程序锁住或独占"的场景准备的转发: 实现都在
   windows_input*.c 里, 平台层只做同名适配。 */
bool bongo_cat_platform_pointer_locked(BongoCatPlatform *platform) {
    return bongo_cat_windows_input_pointer_locked(platform);
}
bool bongo_cat_platform_relative_pointer(BongoCatPlatform *platform,
    double *x, double *y) {
    return bongo_cat_windows_input_take_relative(platform, x, y, NULL);
}
void bongo_cat_platform_relative_pointer_reset(BongoCatPlatform *platform) {
    bongo_cat_windows_input_reset_relative(platform);
}
void bongo_cat_platform_relative_pointer_release(BongoCatPlatform *platform) {
    bongo_cat_windows_input_release_relative(platform);
}
#endif
