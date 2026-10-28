#include "windows_input_internal.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/*
 * Raw Input 接收器的生命周期与主循环(报文解析在别的 windows_input_*.c 里)。
 *
 * 一个进程只能有一个接收器: RegisterRawInputDevices 是进程级的, 注册两次没有意义,
 * 所以用 receiver_claimed 这个进程级标记挡住并发启动(失败路径必须把它清回去, 否则
 * 之后再也起不来)。
 * 状态对象用引用计数管理(references = 2: 调用方一份、工作线程一份), 最后一份释放时
 * 才真正 free 并清掉 claimed 标记。
 */
static volatile LONG receiver_claimed;

static void free_state(WindowsInputState *state);

/* 引用计数归零才真正释放。这条路径可能跑在接收线程, 也可能跑在调用方线程上, 所以
   free_state 里只做 CloseHandle / free, 不再碰任何共享状态。 */
static void release_state(WindowsInputState *state) {
    if (InterlockedDecrement(&state->references) == 0) free_state(state);
}

/* 唤醒主循环: 往 SDL 事件队列推一个自定义事件。wake_pending 保证"推过但还没被消费"
   时不重复推; 队列满时不清标记, 下一轮会再试。 */
void bongo_cat_windows_input_wake(WindowsInputState *state) {
    if (!state->wake_pending) return;
    AcquireSRWLockShared(&state->platform_lock);
    BongoCatPlatform *platform = state->platform;
    if (platform && platform->wake_event_type >= SDL_EVENT_USER) {
        SDL_Event wake;
        SDL_zero(wake);
        wake.type = platform->wake_event_type;
        if (SDL_PushEvent(&wake)) state->wake_pending = false;
        else state->diagnostic_wake_failures++;
    }
    ReleaseSRWLockShared(&state->platform_lock);
}

/* 把已归一化的输入事件交给 core 的输入队列。整个调用持 platform_lock 共享锁 ——
   platform 指针会被 stop 置空, 这里读到的指针必须保证本次调用期间有效。
   推送失败只计数: 队列满时的丢弃策略由 core 决定(那里会优先保住抬起事件)。 */
bool bongo_cat_windows_input_push_event(WindowsInputState *state,
    BongoCatInputKind kind, const char *name, float value) {
    if (!state || !name) return false;
    AcquireSRWLockShared(&state->platform_lock);
    BongoCatPlatform *platform = state->platform;
    bool pushed = false;
    if (platform) {
        BongoCatInputEvent event = {0};
        event.kind = kind;
        event.timestamp_ms = SDL_GetTicks();
        event.value = value;
        snprintf(event.name, sizeof(event.name), "%s", name);
        pushed = bongo_cat_input_push(platform->input, &event);
        if (!pushed) state->diagnostic_queue_failures++;
        else if (kind == BONGO_CAT_INPUT_KEY_DOWN || kind == BONGO_CAT_INPUT_KEY_UP)
            state->diagnostic_queued_keys++;
    }
    ReleaseSRWLockShared(&state->platform_lock);
    if (pushed) state->wake_pending = true;
    return pushed;
}

/* 判断接收线程是否还在"输入桌面"上: UAC 提示、锁屏、切换用户都会把前台切到另一个
   桌面, 那时窗口收不到 WM_INPUT。比较的是桌面对象名而不是句柄 —— 同一个桌面可以有
   多个句柄, 句柄比较会误判。 */
static bool input_desktop_available(void) {
    HDESK active = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!active) return false;
    wchar_t active_name[256] = {0}, receiver_name[256] = {0};
    DWORD needed = 0;
    bool available = GetUserObjectInformationW(active, UOI_NAME,
        active_name, sizeof(active_name), &needed) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),
            UOI_NAME, receiver_name, sizeof(receiver_name), &needed) &&
        wcscmp(active_name, receiver_name) == 0;
    CloseDesktop(active);
    return available;
}

/*
 * 每秒一次的自愈检查, 做三件事:
 *   - 重新读一遍注册状态(别的程序可能把这两类设备抢走);
 *   - 注册状态或桌面可用性发生变化时清空设备表 —— 这会让所有"按住"引用归零并补发
 *     抬起事件, 免得用户切走再回来时猫一直举着手;
 *   - 在正常输入桌面上尝试补回丢失的订阅(见下面原有的英文注释: 不能凭"收不到消息"
 *     就断定注册失败)。
 */
static void check_receiver(WindowsInputState *state) {
    unsigned ownership = bongo_cat_windows_input_ownership(state);
    bool inspected = state->registration_error == ERROR_SUCCESS;
    if (!inspected) ownership = state->ownership;
    bool unavailable = !input_desktop_available();
    if (ownership != state->ownership || unavailable != state->desktop_unavailable) {
        if (ownership != state->ownership) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
            "[input] Raw Input ownership changed: previous=%u current=%u "
            "mouse_flags=%lu keyboard_flags=%lu", state->ownership, ownership,
            (unsigned long)state->mouse_registration_flags,
            (unsigned long)state->keyboard_registration_flags);
        if (ownership != state->ownership || unavailable)
            bongo_cat_windows_input_clear_devices(state);
        else bongo_cat_windows_input_clear_motion(state);
        state->ownership = ownership;
        state->desktop_unavailable = unavailable;
    }
    /* Repair a lost subscription on the normal input desktop, after releasing
       stale keys. Never infer failure from silence or foreground monitor. */
    if (inspected && !unavailable)
        ownership = bongo_cat_windows_input_restore(state, ownership);
    state->ownership = ownership;
    AcquireSRWLockExclusive(&state->relative_lock);
    /* Desktop inspection may be denied even when valid WM_INPUT still arrives. */
    state->receiving = (ownership & 1u) != 0;
    ReleaseSRWLockExclusive(&state->relative_lock);
}

/* 周期性诊断日志。只记录计数器和"前台窗口的进程号 / 所在显示器", 不记窗口标题、
   设备标识或按键内容(见下面原有的英文注释)。 */
static void log_receiver(WindowsInputState *state, const char *phase) {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    bool monitor_known = foreground && GetMonitorInfoW(
        MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL), &monitor);
    /* Snapshot only: counters span the whole interval, not just this window.
       No process access, titles, device identifiers, or key values are logged. */
    SDL_LogInfo(BONGO_CAT_LOG_INPUT,
        "[input] receiver phase=%s uptime_ms=%llu keys=%llu mouse=%llu "
        "background_keys=%llu filtered_keys=%llu duplicate_keys=%llu "
        "queued_keys=%llu queue_failures=%llu wake_failures=%llu "
        "invalid=%llu device_drops=%llu read_failures=%llu device_changes=%llu "
        "owned=%u mouse_flags=%lu keyboard_flags=%lu desktop_unavailable=%d "
        "query_error=%lu recovery_error=%lu last_read_error=%lu devices=%u "
        "monitors=%d foreground_pid=%lu foreground_monitor_known=%d "
        "foreground_primary=%d foreground_rect=%ld,%ld,%ld,%ld",
        phase, (unsigned long long)GetTickCount64(),
        state->diagnostic_keys, state->diagnostic_mouse,
        state->diagnostic_background_keys, state->diagnostic_filtered_keys,
        state->diagnostic_duplicate_keys,
        state->diagnostic_queued_keys, state->diagnostic_queue_failures,
        state->diagnostic_wake_failures, state->diagnostic_invalid,
        state->diagnostic_device_drops, state->diagnostic_read_failures,
        state->diagnostic_device_changes, state->ownership,
        (unsigned long)state->mouse_registration_flags,
        (unsigned long)state->keyboard_registration_flags,
        state->desktop_unavailable, (unsigned long)state->registration_error,
        (unsigned long)state->recovery_error, (unsigned long)state->read_error,
        state->device_count,
        GetSystemMetrics(SM_CMONITORS), (unsigned long)pid, monitor_known,
        (monitor.dwFlags & MONITORINFOF_PRIMARY) != 0,
        monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right, monitor.rcMonitor.bottom);
}

/*
 * 接收线程主体: 先建接收窗口并注册, 然后进入 MsgWaitForMultipleObjectsEx 循环 ——
 * 同时等 stop 事件和新消息, 每轮 dispatch 一批消息、每秒自检一次、每 10 秒打一次
 * 诊断日志、把补发的事件 flush 出去、最多每 8ms 唤醒一次主循环。
 * 两个超时值的分工: 有积压工作时用 8ms(尽快再跑一轮), 空闲时用 1000ms 省电。有
 * WM_INPUT 到来时 MsgWait 会立刻返回, 所以收输入并不依赖这个超时。
 * 退出时先清掉 receiving 标志再清空设备(补发抬起), 最后唤醒一次主循环让它处理这些
 * 收尾事件。
 */
static DWORD WINAPI input_thread(void *context) {
    WindowsInputState *state = context;
    if (WaitForSingleObject(state->stop, state->test_start_delay_ms) == WAIT_OBJECT_0) {
        SetEvent(state->ready);
        release_state(state);
        return 0;
    }
    state->registered = bongo_cat_windows_input_receiver_create(state);
    if (!state->registered) SDL_LogError(SDL_LOG_CATEGORY_INPUT,
        "Raw Input initialization failed: error=%lu", (unsigned long)state->startup_error);
    if (state->registered) {
        state->ownership = 3;
        check_receiver(state);
        log_receiver(state, "started");
        state->diagnostic_ms = GetTickCount64();
    }
    SetEvent(state->ready);
    ULONGLONG last_check = GetTickCount64(), last_wake = 0;
    while (state->registered) {
        DWORD timeout = state->wake_pending || state->retry_events ? 8 : 1000;
        DWORD wait = MsgWaitForMultipleObjectsEx(1, &state->stop, timeout,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                "[input] Raw Input wait failed: error=%lu", (unsigned long)GetLastError());
            break;
        }
        if (!bongo_cat_windows_input_dispatch(state)) break;
        ULONGLONG now = GetTickCount64();
        if (now - last_check >= 1000) {
            check_receiver(state);
            last_check = now;
        }
        if (now - state->diagnostic_ms >= 10000) {
            log_receiver(state, "periodic");
            state->diagnostic_ms = now;
        }
        bongo_cat_windows_input_flush(state);
        if (now - last_wake >= 8) {
            bongo_cat_windows_input_wake(state);
            last_wake = now;
        }
    }
    AcquireSRWLockExclusive(&state->relative_lock);
    state->receiving = false;
    ReleaseSRWLockExclusive(&state->relative_lock);
    bongo_cat_windows_input_clear_devices(state);
    bongo_cat_windows_input_wake(state);
    if (state->registered) log_receiver(state, "stopping");
    bongo_cat_windows_input_receiver_destroy(state);
    release_state(state);
    return 0;
}

static void free_state(WindowsInputState *state) {
    if (state->thread) CloseHandle(state->thread);
    if (state->ready) CloseHandle(state->ready);
    if (state->stop) CloseHandle(state->stop);
    free(state);
    InterlockedExchange(&receiver_claimed, 0);
}

/* 启动: 抢进程级标记 -> 关掉 SDL 自己的原始键盘处理(原因见下面原有的英文注释) ->
   建两个事件和线程 -> 等线程就绪(带超时, 免得拖住启动流程)。
   任何一步失败都统一走 stop 回收, 保证不会留下半个线程或半个状态对象。 */
bool bongo_cat_windows_input_start(BongoCatPlatform *platform) {
    if (!platform || !platform->input || platform->native) return false;
    if (InterlockedCompareExchange(&receiver_claimed, 1, 0) != 0) return false;
    /* SDL retains legacy keyboard messages; this receiver owns Raw Input. */
    SDL_SetHintWithPriority(SDL_HINT_WINDOWS_RAW_KEYBOARD, "0", SDL_HINT_OVERRIDE);
    WindowsInputState *state = calloc(1, sizeof(*state));
    if (!state) { InterlockedExchange(&receiver_claimed, 0); return false; }
    InitializeSRWLock(&state->platform_lock);
    InitializeSRWLock(&state->relative_lock);
    state->platform = platform;
    const char *delay_text = SDL_getenv("BONGO_CAT_TEST_RAW_INPUT_DELAY_MS");
    if (delay_text) {
        unsigned long delay = strtoul(delay_text, NULL, 10);
        state->test_start_delay_ms = delay > 10000 ? 10000 : (DWORD)delay;
    }
    state->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    state->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* The caller retains ownership until the thread handle is published and
       stop finishes waiting; the worker retains ownership until cleanup. */
    state->references = 2;
    state->thread = state->stop && state->ready ?
        CreateThread(NULL, 0, input_thread, state, 0, NULL) : NULL;
    if (!state->thread) { free_state(state); return false; }
    platform->native = state;
    DWORD wait = WaitForSingleObject(state->ready, 1500);
    if (wait == WAIT_OBJECT_0 && state->registered) return true;
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
        "[input] Raw Input startup failed: wait=%lu", (unsigned long)wait);
    bongo_cat_windows_input_stop(platform);
    return false;
}

/*
 * 只读状态查询 (设置 → 应用 → 运行自检用): 接收线程还在不在。
 * 判据用"线程句柄还活着"而不是 state->registered —— 后者只在接收线程里读写, 跨线程
 * 读它是数据竞争; 而 WaitForSingleObject(0) 是内核状态, 没有这个问题。它也不会做任何
 * 注册/反注册动作, 自检因此不会扰动正在运行的输入通道。
 */
bool bongo_cat_platform_input_receiver_active(const BongoCatPlatform *platform) {
    WindowsInputState *state = platform ? (WindowsInputState *)platform->native : NULL;
    if (!state || !state->thread) return false;
    return WaitForSingleObject(state->thread, 0) == WAIT_TIMEOUT;
}

/* 停止: 先把 platform 指针置空(此后不会再有新事件进队列), 再置 stop 事件并等线程
   退出。线程若 3 秒内没退, 只记错误并延后清理 —— 强杀线程会让它持有的锁处于未定义
   状态, 宁可等它自然结束(引用计数保证内存最终会被释放)。 */
void bongo_cat_windows_input_stop(BongoCatPlatform *platform) {
    WindowsInputState *state = platform ? platform->native : NULL;
    if (!state) return;
    AcquireSRWLockExclusive(&state->platform_lock);
    state->platform = NULL;
    ReleaseSRWLockExclusive(&state->platform_lock);
    SetEvent(state->stop);
    platform->native = NULL;
    if (WaitForSingleObject(state->thread, 3000) != WAIT_OBJECT_0) {
        SDL_LogError(SDL_LOG_CATEGORY_INPUT,
            "[input] Raw Input thread did not stop; cleanup is deferred until thread exit");
    }
    release_state(state);
}
#endif
