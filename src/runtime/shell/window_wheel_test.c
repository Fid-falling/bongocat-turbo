/* 滚轮与菜单这类"窗口行为"自测文件之一, 由 lifecycle/app.c 的 smoke 流程调用
 * (bongo_cat_window_wheel_self_test); 返回值会和其它自测一起汇总成 CI 致命错误,
 * 不是"仅供参考"的检查。
 * 为什么注入时间而不是等真实时钟: 断言里有"帧间隔内必须一动不动"和"若干帧后必须收敛到
 * 某值"这类精确时序, 真实时钟下必然抖动。合成事件靠 timestamp=0 走平台侧的简化路径。
 * 最容易改错: 还原部分 —— 漏掉任意一项 (几何、透明度、修饰键、控制位) 都会污染后面的
 * 自测用例, 而失败明细只出现在 stderr。 */
#include "runtime.h"
#include "window_wheel_internal.h"

#include <stdio.h>

/* 滚轮自测 (smoke): 在真实窗口上验证手势判定、Ctrl 切换、动画收敛、目标上下限、反向滚动、
 * 方向翻转与取整。副作用很重 —— 会改窗口几何/不透明度/修饰键/Ctrl 原子位与 session 快照,
 * 所以开头备份、结尾逐项还原, 失败也不能把状态留在半路。
 * 时间全部由测试注入 (started + N ns), 不依赖真实时钟, 才能稳定断言"帧间隔内不推进"。 */
bool bongo_cat_window_wheel_self_test(BongoCatApp *app) {
    if (!app || !app->window) return false;
    BongoCatWindowState backup = app->session.window;
    int original_x, original_y, original_width, original_height;
    SDL_GetWindowPosition(app->window, &original_x, &original_y);
    SDL_GetWindowSize(app->window, &original_width, &original_height);
    SDL_Keymod modifiers = SDL_GetModState();
    uint_fast8_t original_control = atomic_load(&app->input.control);
    atomic_store(&app->input.control, 1);
    /* 同时置原子控制位与 SDL 修饰键: Windows 路径只信 input.control, 其它平台还看修饰键。 */
    SDL_SetModState(modifiers | SDL_KMOD_CTRL);
    app->session.window.opacity_percent = 80.0f;
    app->session.window.scale_percent = 100.0f;
    bongo_cat_window_cancel_wheel_animation(app);
    /* timestamp 保持 0 —— 平台侧判定为合成事件, 会跳过桌面指针复检 (自测时指针通常不在
     * 窗口上); 坐标取窗口中心, windowID 先填本窗口, 稍后改成 0 验证"别的窗口滚轮不生效"。 */
    SDL_MouseWheelEvent wheel = {.y = -1.0f,
        .windowID = SDL_GetWindowID(app->window),
        .mouse_x = original_width * 0.5f,
        .mouse_y = original_height * 0.5f};
    float untouched_scale = app->session.window.scale_percent;
    SDL_WindowID own_window = wheel.windowID;
    wheel.windowID = 0;
    bongo_cat_window_wheel(app, &wheel);
    bool foreign = !app->wheel_animation_active &&
        app->session.window.scale_percent == untouched_scale;
    wheel.windowID = own_window;
    bongo_cat_window_wheel(app, &wheel);
    uint64_t started = app->wheel_animation_ns;
    /* started 是事件之后的动画基准时间戳, 后面所有 tick 都相对它推进, 断言与真实时钟无关。 */
    for (int i = 1; i <= 30; ++i)
        bongo_cat_window_update_wheel_animation(app, started + i * 16666667ull);
    bool opacity = SDL_fabsf(
        app->session.window.opacity_percent - 75.0f) < 0.1f;
    atomic_store(&app->input.control, 0);
#ifndef _WIN32
    SDL_SetModState(modifiers & ~SDL_KMOD_CTRL);
#endif
    /* On Windows leave SDL Ctrl set to reproduce a stale unfocused cache. */
    wheel.y = 1.0f;
    bongo_cat_window_wheel(app, &wheel);
    started = app->wheel_animation_ns;
    float scale_before_frame = app->session.window.scale_percent;
    for (uint64_t offset = 1000000ull;
        offset < BONGO_CAT_WHEEL_FRAME_INTERVAL_NS; offset += 1000000ull)
        bongo_cat_window_update_wheel_animation(app, started + offset);
    /* 帧间隔内的多次唤醒不能推进动画 (窗口 resize/expose 消息会造成这种唤醒)。 */
    bool coalesced = app->session.window.scale_percent == scale_before_frame &&
        app->wheel_animation_ns == started;
    bongo_cat_window_update_wheel_animation(app,
        started + BONGO_CAT_WHEEL_FRAME_INTERVAL_NS);
    coalesced = coalesced && app->session.window.scale_percent > scale_before_frame &&
        app->session.window.scale_percent < app->wheel_scale_target;
    for (int i = 1; i <= 12; ++i)
        bongo_cat_window_update_wheel_animation(app, started + i * 16666667ull);
    bool responsive = SDL_fabsf(
        app->session.window.scale_percent - 102.0f) < 0.1f;
    bongo_cat_window_update_wheel_animation(app,
        started + 250000000ull);
    bool scale = responsive && !app->wheel_animation_active &&
        SDL_fabsf(app->session.window.opacity_percent - 75.0f) < 0.1f;
    /* Ctrl 手势到此结束: 后面都按"普通滚轮 = 缩放"继续验证。 */
    SDL_SetModState(modifiers & ~SDL_KMOD_CTRL);
    bongo_cat_window_cancel_wheel_animation(app);
    bongo_cat_window_apply_geometry(app, original_x, original_y, 100.0f,
        original_width, original_height);
    wheel.y = 1000.0f;
    for (int i = 0; i < 2; ++i) bongo_cat_window_wheel(app, &wheel);
    bool burst = app->wheel_scale_target >= 103.5f &&
        app->wheel_scale_target <= 104.1f;
    bongo_cat_window_cancel_wheel_animation(app);
    bongo_cat_window_apply_geometry(app, original_x, original_y, 100.0f,
        original_width, original_height);
    wheel.y = 3.0f;
    bongo_cat_window_wheel(app, &wheel);
    bool aggregated = app->wheel_scale_target >= 101.5f &&
        app->wheel_scale_target <= 102.1f;
    bongo_cat_window_cancel_wheel_animation(app);
    bongo_cat_window_apply_geometry(app, original_x, original_y, 100.0f,
        original_width, original_height);
    wheel.y = 1.0f;
    bongo_cat_window_wheel(app, &wheel);
    wheel.y = -1.0f;
    bongo_cat_window_wheel(app, &wheel);
    started = app->wheel_animation_ns;
    for (int i = 1; i <= 30; ++i)
        bongo_cat_window_update_wheel_animation(app, started + i * 16666667ull);
    bool reversal = !app->wheel_animation_active &&
        SDL_fabsf(app->session.window.scale_percent - 100.0f) < 0.1f;
    wheel.y = 1.0f;
    wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    bongo_cat_window_wheel(app, &wheel);
    started = app->wheel_animation_ns;
    for (int i = 1; i <= 30; ++i)
        bongo_cat_window_update_wheel_animation(app, started + i * 16666667ull);
    bool flipped = SDL_fabsf(
        app->session.window.scale_percent - 98.0f) < 0.1f;
    wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    bongo_cat_window_cancel_wheel_animation(app);
    /* 直接把 session 里的缩放推到上限, 验证"已到上下限时滚轮不再启动动画",
     * 不必真的改窗口几何 (改几何会牵动平台窗口, 自测里代价太高)。 */
    app->session.window.scale_percent = 500.0f;
    bongo_cat_window_wheel(app, &wheel);
    bool maximum = !app->wheel_animation_active;
    bongo_cat_window_cancel_wheel_animation(app);
    app->session.window.scale_percent = 10.0f;
    wheel.y = -1.0f;
    bongo_cat_window_wheel(app, &wheel);
    bool minimum = !app->wheel_animation_active;
    bongo_cat_window_cancel_wheel_animation(app);
    /* Right Ctrl must work even when SDL has not received the key press. */
    atomic_store(&app->input.control, 2);
    /* 不透明度同样验证上下限: 已经在 100 时向上滚不应启动动画。 */
    app->session.window.opacity_percent = 100.0f;
    wheel.y = 1.0f;
    bongo_cat_window_wheel(app, &wheel);
    bool opacity_maximum = !app->wheel_animation_active;
    bongo_cat_window_cancel_wheel_animation(app);
    app->session.window.opacity_percent = 10.0f;
    wheel.y = -1.0f;
    bongo_cat_window_wheel(app, &wheel);
    bool opacity_minimum = !app->wheel_animation_active;
    bool rounding = bongo_cat_window_wheel_round_position(-10.6f) == -11 &&
        bongo_cat_window_wheel_round_position(10.6f) == 11;
    SDL_SetModState(modifiers);
    atomic_store(&app->input.control, original_control);
    app->session.window = backup;
    bongo_cat_window_cancel_wheel_animation(app);
    bongo_cat_platform_set_opacity(&app->platform,
        backup.opacity_percent / 100.0f);
    bongo_cat_platform_set_geometry(&app->platform, original_x, original_y,
        original_width, original_height);
    /* 全部子项都必须通过; 失败时会把每一项打到 stderr, 便于在 smoke 日志里定位回归。 */
    bool passed = foreign && opacity && scale && burst && aggregated &&
        reversal && flipped && maximum && minimum && opacity_maximum &&
        opacity_minimum && rounding && coalesced;
    if (!passed)
        fprintf(stderr, "wheel self-test: foreign=%d opacity=%d scale=%d "
            "burst=%d aggregated=%d reversal=%d flipped=%d maximum=%d "
            "minimum=%d opacity_max=%d opacity_min=%d rounding=%d coalesced=%d\n",
            foreign, opacity, scale, burst, aggregated, reversal, flipped,
            maximum, minimum, opacity_maximum, opacity_minimum, rounding, coalesced);
    return passed;
}
