/*
 * Windows 上的"拖动边框实时重绘"。
 *
 * 为什么需要它: 用户拖窗口边框时, 系统会进入 WM_ENTERSIZEMOVE 的**嵌套消息循环**,
 * 应用自己的主循环在这段时间里根本转不动 —— 表现就是拖边框时窗口一片白、宠物也
 * 卡住不动。所以这里子类化 (subclass) 窗口过程, 在嵌套循环内部自己驱动帧:
 * - WM_SIZE 只置 live_resize_pending, 真正的重绘交给 16ms 定时器做, 把拖动
 * 过程中密集的 WM_SIZE 合并掉 (每个都画一帧会明显拖慢拖动);
 * - 旧的帧先被截成纹理, 每帧贴回去当"暂无更新"的替身, 避免闪白;
 * - 宠物窗口的动画必须在这里手动 tick (pump_pet), 否则拖动期间宠物是静止的。
 * 这一整套只在 GL 下有效: Vulkan 没有帧缓存回拷这条路, 各函数会返回 false,
 * 退化成普通的整帧重绘。
 */
#include "preferences_state.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <SDL3/SDL_properties.h>

static const wchar_t original_property[] = L"BongoCat.PreferenceResizeProc";
static const wchar_t value_property[] = L"BongoCat.PreferenceResizeValue";
#define BONGO_CAT_LIVE_RESIZE_TIMER ((UINT_PTR)0xBC50)
#define BONGO_CAT_LIVE_RESIZE_INTERVAL_MS 16

static HWND native_window(BongoCatPreferences *value) {
    return value && value->window ? (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(value->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}

/*
 * 从嵌套消息循环里同步渲染一帧。
 * live_resize_rendering 是重入闸: 渲染过程本身会泵消息, 泵到下一个 WM_SIZE 就会
 * 递归回到这里, 没有这个闸会无限递归。
 * live_resize_fast 标记"正在拖动中", 让绘制层走廉价路径 (拖动时画质可以让步);
 * 无论走哪条路都必须复位, 不能留到最后。
 */
static void render_live(BongoCatPreferences *value) {
    if (!value || !value->window || value->live_resize_rendering) return;
    value->live_resize_rendering = true;
    bool fast = value->live_resize_active;
    value->ui.live_resize_fast = fast;
    if (!value->input_active) bongo_cat_preferences_input_begin(value);
    value->render_dirty = true;
    bongo_cat_preferences_render(value);
    value->ui.live_resize_fast = false;
    if (fast) value->live_resize_layout_frames++;
    value->live_resize_rendering = false;
}

/* 把上一帧截成纹理; 截不了就返回 false, 调用方退回整帧重绘 */
static bool capture_live(BongoCatPreferences *value) {
    /* Vulkan 模式没有帧缓存回拷这条路, 直接走正常重绘 */
    if (!value || bongo_cat_ui_uses_vulkan(&value->ui)) return false;
    if (!SDL_GL_MakeCurrent(value->window, value->gl_context))
        return false;
    bool result = bongo_cat_ui_resize_cache_capture(&value->ui);
    SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
    return result;
}

/*
 * 只贴缓存纹理 + 交换缓冲, 不重画 UI 树 —— 拖动中的大部分 WM_SIZE 靠它应付,
 * 这是"拖边框不卡"的关键。注意这里也补记一次帧心跳, 否则内存策略会以为窗口空闲。
 */
static bool present_live(BongoCatPreferences *value) {
    if (!value || bongo_cat_ui_uses_vulkan(&value->ui)) return false;
    if (!SDL_GL_MakeCurrent(value->window, value->gl_context))
        return false;
    bool result = bongo_cat_ui_resize_cache_present(&value->ui) &&
        SDL_GL_SwapWindow(value->window);
    SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
    if (result) bongo_cat_preferences_record_frame(value);
    return result;
}

/* 嵌套循环里手动推进宠物窗口的动画帧, 否则拖动期间宠物是静止的 */
static void pump_pet(BongoCatPreferences *value) {
    if (value && value->live_resize_modal_ready)
        bongo_cat_modal_frame_tick(&value->live_resize_modal_frame);
}

/*
 * 子类化后的窗口过程。
 * 先调原过程再处理: WM_SIZE 类消息要等默认处理把尺寸真正应用完, 我们读到的
 * 客户区大小才是新的。消息处理完返回原过程的返回值, 不改动它的语义。
 */
static LRESULT CALLBACK live_resize_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    WNDPROC original = (WNDPROC)GetPropW(window, original_property);
    BongoCatPreferences *value = (BongoCatPreferences *)GetPropW(
        window, value_property);
    bool enter = value && message == WM_ENTERSIZEMOVE;
    bool resize = value && value->live_resize_active && message == WM_SIZE;
    bool timer = value && value->live_resize_active && message == WM_TIMER &&
        wparam == BONGO_CAT_LIVE_RESIZE_TIMER;
    LRESULT result = CallWindowProcW(original ? original : DefWindowProcW,
        window, message, wparam, lparam);
    if (enter) {
        /* 进入拖动: 起定时器 + 先截一张当前帧当替身, 后面每个 WM_SIZE 都贴它 */
        value->live_resize_active = true;
        value->live_resize_pending = false;
        value->live_resize_timer = SetTimer(window,
            BONGO_CAT_LIVE_RESIZE_TIMER,
            BONGO_CAT_LIVE_RESIZE_INTERVAL_MS, NULL) != 0;
        bongo_cat_modal_frame_init(&value->live_resize_modal_frame,
            value->app);
        value->live_resize_modal_ready = true;
        capture_live(value);
        pump_pet(value);
    }
    if (resize) {
        value->live_resize_pending = true;
        if (!present_live(value) || !value->live_resize_timer) {
            value->live_resize_pending = false;
            render_live(value);
            capture_live(value);
        }
        pump_pet(value);
    }
    if (timer) {
        /* 定时器到点才真正重绘: 拖动中堆积的 WM_SIZE 在这里被合并成一次 */
        if (value->live_resize_pending) {
            value->live_resize_pending = false;
            render_live(value);
            capture_live(value);
        }
        pump_pet(value);
    }
    if (value && message == WM_EXITSIZEMOVE) {
        /* 收尾: 停定时器, 画一帧最终尺寸的干净画面, 然后扔掉拖动期间用的缓存纹理 */
        KillTimer(window, BONGO_CAT_LIVE_RESIZE_TIMER);
        value->live_resize_active = false;
        value->live_resize_pending = false;
        value->live_resize_timer = false;
        render_live(value);
        pump_pet(value);
        value->live_resize_modal_ready = false;
        if (!bongo_cat_ui_uses_vulkan(&value->ui) &&
        SDL_GL_MakeCurrent(value->window, value->gl_context)) {
            bongo_cat_ui_resize_cache_destroy(&value->ui);
            SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
        }
    }
    return result;
}

/*
 * 安装子类化。必须幂等: 每次显示窗口都会调一次, 已经装过就直接返回 ——
 * 重复安装会把我们自己的过程当成"原过程"存起来, 越套越深且再也没法还原。
 * 属性用 SetPropW 挂在 HWND 上而不是存全局变量: 多窗口/多实例下不会串,
 * 窗口销毁时属性也会跟着没。任一步失败都要把已设的属性回滚干净。
 */
void bongo_cat_preferences_live_resize_install(BongoCatPreferences *value) {
    HWND window = native_window(value);
    if (!window || GetPropW(window, original_property)) return;
    WNDPROC original = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (!original || !SetPropW(window, original_property, (HANDLE)original)) return;
    if (!SetPropW(window, value_property, (HANDLE)value)) {
        RemovePropW(window, original_property);
        return;
    }
    if (!SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)live_resize_proc)) {
        RemovePropW(window, value_property);
        RemovePropW(window, original_property);
    }
}

/*
 * 卸载。必须在 SDL_DestroyWindow 之前调用 (release_window 就是这么排的):
 * HWND 一旦销毁, 这里拿不到原过程, 窗口类上就永久留着指向 live_resize_proc 的
 * 过程指针, 而 proc 里的 value 已经 free —— 之后的消息会跳到野指针上。
 * 最后把所有相关状态清干净, 保证下次 install 从零开始。
 */
void bongo_cat_preferences_live_resize_uninstall(BongoCatPreferences *value) {
    if (!value) return;
    HWND window = native_window(value);
    WNDPROC original = window ? (WNDPROC)GetPropW(window, original_property) : NULL;
    if (window) KillTimer(window, BONGO_CAT_LIVE_RESIZE_TIMER);
    if (original) SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)original);
    if (window) {
        RemovePropW(window, value_property);
        RemovePropW(window, original_property);
    }
    value->live_resize_active = false;
    value->live_resize_pending = false;
    value->live_resize_timer = false;
    value->live_resize_modal_ready = false;
    value->ui.live_resize_fast = false;
    value->live_resize_rendering = false;
}
#else
/* 非 Windows 平台没有这种嵌套消息循环, 两个函数保留为空实现以维持调用方不变 */
void bongo_cat_preferences_live_resize_install(BongoCatPreferences *value) {
    (void)value;
}
void bongo_cat_preferences_live_resize_uninstall(BongoCatPreferences *value) {
    (void)value;
}
#endif
