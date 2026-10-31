#include "windows_input_internal.h"

#ifdef _WIN32
/*
 * "鼠标是不是被别的游戏/程序锁住了"的探测(模型线程, 由
 * bongo_cat_platform_pointer_locked 调用)。
 *
 * 每次采样光标可见性、位置、裁剪矩形与所在显示器, 交给 windows_input_detection.c 里
 * 的判定器累积判断。只在前台不属于自己进程时才真正采样(见下), 因为我们自己的窗口
 * 不可能触发游戏模式。
 */
static void observe_cursor(WindowsPointerObservation *sample) {
    CURSORINFO cursor = {.cbSize = sizeof(cursor)};
    sample->cursor_known = GetCursorInfo(&cursor) != FALSE;
    sample->cursor_flags = cursor.flags;
    if (sample->cursor_known) {
        sample->position = cursor.ptScreenPos;
        sample->position_known = true;
    } else sample->position_known = GetCursorPos(&sample->position) != FALSE;
    sample->clip_known = GetClipCursor(&sample->clip) != FALSE;
    if (sample->position_known) {
        MONITORINFO monitor = {.cbSize = sizeof(monitor)};
        sample->monitor_known = GetMonitorInfoW(MonitorFromPoint(sample->position,
            MONITOR_DEFAULTTONULL), &monitor) != FALSE;
        sample->monitor = monitor.rcMonitor;
    }
}

/* 结果缓存 16ms: 这个函数每帧都会被问一次, 而 GetCursorInfo / GetClipCursor 都是
   系统调用, 每次都问会明显浪费。 */
bool bongo_cat_windows_input_pointer_locked(BongoCatPlatform *platform) {
    WindowsInputState *state = platform ? platform->native : NULL;
    if (!state) return false;
    ULONGLONG now_ms = GetTickCount64();
    if (state->pointer_probe_ready && now_ms - state->pointer_probe_ms < 16)
        return state->pointer_detection.relative;
    state->pointer_probe_ready = true;
    state->pointer_probe_ms = now_ms;
    WindowsPointerObservation sample = {0};
    sample.foreground = GetForegroundWindow();
    if (sample.foreground) GetWindowThreadProcessId(sample.foreground, &sample.pid);
    sample.foreign = sample.pid && sample.pid != GetCurrentProcessId();
    /* Our own windows cannot trigger game mode. Still consume observations so
       old desktop movement cannot leak into the next foreground application. */
    /* 自己的窗口不会触发游戏模式, 但这次观测仍然要交给判定器(哪怕内容是空的):
       否则切到别的程序时, 之前累积的桌面移动会被误当成"在那个程序里"的运动。 */
    if (sample.foreign) observe_cursor(&sample);
    unsigned long long generation =
        bongo_cat_windows_input_take_observation(state, &sample);
    /* 接收线程重置过设备或运动状态时 generation 会变化, 这时必须把判定器清零:
       旧的采样序列已经和新设备状态对不上了。 */
    if (state->pointer_generation != generation) {
        state->pointer_detection = (WindowsPointerDetection){0};
        state->pointer_generation = generation;
    }
    bool locked = bongo_cat_windows_pointer_detect(&state->pointer_detection,
        &sample, now_ms);
    /* 只在判定变化时记一行: 每帧打日志会把日志淹掉, 而"游戏里鼠标被锁住"这类反馈
       只有这一行能定位 (锁定原因 + 证据 + 前台进程)。 */
    if ((int)locked != state->pointer_logged_locked ||
        (int)state->pointer_detection.reason != state->pointer_logged_reason) {
        state->pointer_logged_locked = (int)locked;
        state->pointer_logged_reason = (int)state->pointer_detection.reason;
        SDL_LogInfo(BONGO_CAT_LOG_INPUT,
            "[input] 指针锁定判定: locked=%d reason=%s raw=(%.0f,%.0f) samples=%llu "
            "pinned=%u win_exc=%lld win_dist=%lld win_returns=%u win_anchor=%u win_hint=%d "
            "unlocked=%d cursor=%s clip=%ld,%ld,%ld,%ld foreground_pid=%lu",
            locked ? 1 : 0,
            bongo_cat_windows_pointer_reason_name(state->pointer_detection.reason),
            state->pointer_detection.diag_raw_x, state->pointer_detection.diag_raw_y,
            (unsigned long long)state->pointer_detection.diag_moving_samples,
            state->pointer_detection.pinned_windows,
            state->pointer_detection.diag_excursion,
            state->pointer_detection.diag_distance,
            state->pointer_detection.diag_returns,
            state->pointer_detection.diag_anchor_hits,
            state->pointer_detection.diag_hint,
            state->pointer_detection.diag_unlocked ? 1 : 0,
            sample.cursor_known && sample.cursor_flags == 0 ? "hidden" : "shown",
            sample.clip_known ? (long)sample.clip.left : -1L,
            sample.clip_known ? (long)sample.clip.top : -1L,
            sample.clip_known ? (long)sample.clip.right : -1L,
            sample.clip_known ? (long)sample.clip.bottom : -1L,
            (unsigned long)sample.pid);
    }
    return locked;
}
#endif
