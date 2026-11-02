#include "windows_input_internal.h"

#ifdef _WIN32
/*
 * "现在该不该用相对位移"的采样侧: 每帧问一次, 结果缓存 16ms。
 *
 * 判定本身在 windows_input_detection.c, 这里只负责把 Win32 的事实采齐:
 *   - GetCursorInfo: 光标是显示还是隐藏 (游戏独占鼠标最直接的证据);
 *   - GetClipCursor: 有没有被裁进 2x2 的小矩形 (老式做法);
 *   - 前台窗口的进程号: 自己进程的前台不参与判定。
 * 只在前台属于别的进程时才真正采光标状态, 否则交给判定器一个空样本, 让它回到绝对模式。
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
    if (sample.foreign) observe_cursor(&sample);
    bool locked = bongo_cat_windows_pointer_detect(&state->pointer_detection,
        &sample, now_ms);
    /* 只在判定变化时记一行: 每帧打日志会把日志淹掉, 而"游戏里鼠标被锁住"这类反馈
       只有这一行能定位。 */
    if ((int)locked != state->pointer_logged_locked ||
        (int)state->pointer_detection.reason != state->pointer_logged_reason) {
        state->pointer_logged_locked = (int)locked;
        state->pointer_logged_reason = (int)state->pointer_detection.reason;
        SDL_LogInfo(BONGO_CAT_LOG_INPUT,
            "[input] 指针锁定判定: locked=%d reason=%s cursor=%s clip=%ld,%ld,%ld,%ld "
            "foreground_pid=%lu",
            locked ? 1 : 0,
            bongo_cat_windows_pointer_reason_name(state->pointer_detection.reason),
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
