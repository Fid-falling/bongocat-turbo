#include "macos_internal.h"
#include "macos_keys.h"

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

/*
 * macOS 的全局输入采集: 一个 CGEventTap(只听不改)跑在自己的 run loop 线程上。
 *
 * 为什么要单独一条线程加 run loop: CGEventTap 的回调由 run loop 驱动, 而 SDL 的主 run
 * loop 属于主线程; 把 tap 挂在自己的 loop 上既不干扰 SDL, 关停时也能精确控制顺序。
 * 授权: macOS 10.15 起需要"输入监控"权限(CGPreflightListenEventAccess /
 * CGRequestListenEventAccess)。没有权限时 tap 建不起来, 但这不算致命错误 —— 程序照常
 * 运行, 只是收不到全局输入。
 *
 * 引用计数 = 2(调用方 + 工作线程): stop 时调用方释放一份, 线程结束时释放另一份。即使
 * 线程卡住(见 stop 里的超时分支)也不会泄漏, 只是状态对象晚一点回收。
 */
typedef struct MacInputState {
    BongoCatPlatform *platform;
    SDL_Thread *thread;
    SDL_Semaphore *ready;
    CFMachPortRef tap;
    CFRunLoopSourceRef source;
    CFRunLoopRef loop;
    bool key_down[BONGO_CAT_INPUT_KEY_STATE_CAP];
    atomic_bool supported;
    atomic_bool stop_requested;
    atomic_uint references;
} MacInputState;

/* 进程级标志: 当前 tap 是否真的在工作。查询接口可能被任意线程调用, 所以用原子量而不是
   去读 state —— state 在 stop 之后可能已经是悬空指针。 */
static atomic_bool global_supported = ATOMIC_VAR_INIT(false);

/* 引用计数归零才释放。信号量必须先销毁再 free: 线程已经不用它了, 但 stop 的超时分支可能
   还在等它。 */
static void release_state(MacInputState *state) {
    if (atomic_fetch_sub(&state->references, 1) != 1) return;
    SDL_DestroySemaphore(state->ready);
    free(state);
}

static void push(MacInputState *state, BongoCatInputKind kind,
    const char *name, float value) {
    if (!state || !name) return;
    BongoCatInputEvent input = {.kind = kind, .timestamp_ms = SDL_GetTicks(), .value = value};
    snprintf(input.name, sizeof(input.name), "%s", name);
    if (bongo_cat_input_push(state->platform->input, &input)) {
        SDL_Event wake = {0};
        wake.type = state->platform->wake_event_type;
        SDL_PushEvent(&wake);
    }
}

/* 修饰键(56/60 左右 Shift, 59/62 左右 Control, 58/61 左右 Option, 54/55 左右 Command,
   57 CapsLock)映射到对应的事件标志位。 */
static CGEventFlags modifier_flag(CGKeyCode code) {
    if (code == 56 || code == 60) return kCGEventFlagMaskShift;
    if (code == 59 || code == 62) return kCGEventFlagMaskControl;
    if (code == 58 || code == 61) return kCGEventFlagMaskAlternate;
    if (code == 54 || code == 55) return kCGEventFlagMaskCommand;
    if (code == 57) return kCGEventFlagMaskAlphaShift;
    return 0;
}

/*
 * tap 回调(在输入线程的 run loop 上执行)。两条硬性约束:
 *   - 必须尽快返回, 并且原样返回 event —— 我们用的是 kCGEventTapOptionListenOnly(只听),
 *     但回调超时同样会被系统摘掉 tap;
 *   - 绝不能在这里做阻塞操作。
 * 被系统禁用(超时或被其他程序的输入触发)时只能重新启用并清空已知按键状态: 禁用期间丢掉的
 * 抬起事件再也补不回来, 不清状态就会留下永远按住不放的键。
 */
static CGEventRef event_tap(CGEventTapProxy proxy, CGEventType type,
    CGEventRef event, void *userdata) {
    (void)proxy;
    MacInputState *state = userdata;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        memset(state->key_down, 0, sizeof(state->key_down));
        if (state->tap) CGEventTapEnable(state->tap, true);
        return event;
    }
    if (type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged ||
        type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDragged) {
        CGPoint point = CGEventGetLocation(event);
        if (bongo_cat_input_mouse(state->platform->input, point.x, point.y)) {
            SDL_Event wake = {0};
            wake.type = state->platform->wake_event_type;
            SDL_PushEvent(&wake);
        }
        return event;
    }
    if (type == kCGEventKeyDown || type == kCGEventKeyUp || type == kCGEventFlagsChanged) {
        CGKeyCode code = (CGKeyCode)CGEventGetIntegerValueField(event,
            kCGKeyboardEventKeycode);
        char buffer[16]; const char *name = bongo_cat_macos_key_name(code, buffer);
        bool down = type == kCGEventKeyDown;
        if (type == kCGEventFlagsChanged) {
            CGEventFlags flag = modifier_flag(code);
            down = flag && (CGEventGetFlags(event) & flag) != 0;
        }
        if (name && bongo_cat_input_edge(state->key_down, code, down))
            push(state, down ? BONGO_CAT_INPUT_KEY_DOWN :
                BONGO_CAT_INPUT_KEY_UP, name, down ? 1.0f : 0.0f);
        return event;
    }
    const char *name = type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp
        ? "Left" : type == kCGEventRightMouseDown || type == kCGEventRightMouseUp
        ? "Right" : "Middle";
    bool down = type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown ||
        type == kCGEventOtherMouseDown;
    push(state, down ? BONGO_CAT_INPUT_MOUSE_DOWN : BONGO_CAT_INPUT_MOUSE_UP,
        name, down ? 1.0f : 0.0f);
    return event;
}

/*
 * 输入线程: 建 tap(挂在 kCGSessionEventTap、追加到事件链末尾、只听不改) -> 挂到本线程的
 * run loop -> 用 CFRunLoopRunInMode 每段 0.1 秒轮询, 以便及时看到 stop_requested。
 * 授权判定只用于决定日志措辞(见下面原有的英文注释): tap 建不起来也可能是别的原因, 不能
 * 一律归咎于权限。
 * 退出时按"先摘 source、再 invalidate tap、最后逐个 CFRelease"的顺序清理 —— CFMachPort 与
 * run loop source 互相引用, 顺序错了会留下一个还在跑的 source。
 */
static int SDLCALL input_thread(void *userdata) {
    MacInputState *state = userdata;
    @autoreleasepool {
        CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) |
            CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged) |
            CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
            CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged) |
            CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged);
        /* The permission this listener is built with, kept only to word the
           warning below; the page reads the current permission itself. */
        bool permitted = false;
        if (@available(macOS 10.15, *)) permitted = CGPreflightListenEventAccess();
        state->tap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap,
            kCGEventTapOptionListenOnly, mask, event_tap, state);
        if (state->tap) {
            state->source = CFMachPortCreateRunLoopSource(NULL, state->tap, 0);
        }
        if (state->source && !atomic_load(&state->stop_requested)) {
            state->loop = CFRunLoopGetCurrent(); CFRetain(state->loop);
            CFRunLoopAddSource(state->loop, state->source, kCFRunLoopCommonModes);
            CGEventTapEnable(state->tap, true);
            atomic_store(&state->supported, true);
            atomic_store(&global_supported, true);
        }
        SDL_SignalSemaphore(state->ready);
        if (!atomic_load(&state->supported)) {
            /* Not a permission verdict: a listener can also fail to build with
               the permission already granted. */
            if (permitted)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "macOS input listener failed to initialize");
            else
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "macOS input "
                    "monitoring permission is required for global keyboard input");
        }
        while (atomic_load(&state->supported) &&
            !atomic_load(&state->stop_requested))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        if (state->loop && state->source)
            CFRunLoopRemoveSource(state->loop, state->source, kCFRunLoopCommonModes);
        if (state->tap) CFMachPortInvalidate(state->tap);
        if (state->source) CFRelease(state->source);
        if (state->tap) CFRelease(state->tap);
        if (state->loop) CFRelease(state->loop);
        atomic_store(&global_supported, false);
    }
    release_state(state);
    return 0;
}

/* 启动: 建信号量 + 起线程, 但不在这里等就绪 —— 输入还没起来也不该影响程序启动, 需要时再
   用 supported / authorized 查询。引用计数初始为 2, 理由见文件头注释。 */
bool bongo_cat_macos_input_start(BongoCatPlatform *platform, BongoCatError *error) {
    MacInputState *state = calloc(1, sizeof(*state));
    if (!state) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate macOS input state");
        return false;
    }
    state->platform = platform;
    atomic_init(&state->supported, false);
    atomic_init(&state->stop_requested, false);
    /* Caller and worker each release one reference, including after timeout. */
    atomic_init(&state->references, 2);
    state->ready = SDL_CreateSemaphore(0);
    state->thread = state->ready ? SDL_CreateThread(input_thread,
        BONGO_CAT_SLUG "-macos-input", state) : NULL;
    if (!state->ready || !state->thread) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot start the macOS input listener");
        if (state->thread) SDL_WaitThread(state->thread, NULL);
        if (state->ready) SDL_DestroySemaphore(state->ready);
        free(state);
        return false;
    }
    platform->native = state;
    return true;
}

/* 停止: 先置 stop_requested(线程会从轮询里退出), 再等它报告"已就绪"最多 3 秒 —— 只有
   确认线程进过 run loop 才敢等它结束。超时说明线程卡在建 tap 上, 这时只能 detach 并释放
   调用方那一份引用: 强杀线程会让它持有的 CoreFoundation 对象永远泄漏, 甚至崩在
   CFRelease 上。 */
void bongo_cat_macos_input_stop(BongoCatPlatform *platform) {
    MacInputState *state = platform ? platform->native : NULL;
    if (!state) return;
    atomic_store(&state->stop_requested, true);
    if (state->thread &&
        !SDL_WaitSemaphoreTimeout(state->ready, 3000)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "macOS input thread did not initialize before shutdown");
        SDL_DetachThread(state->thread);
        platform->native = NULL;
        release_state(state);
        return;
    }
    if (state->thread) SDL_WaitThread(state->thread, NULL);
    platform->native = NULL;
    release_state(state);
}

/* 下面三个是上层的查询接口: supported 判断 tap 是否真的在工作; authorized / request 分别
   对应查询与申请"输入监控"权限(macOS 10.15 以下没有这套机制, 一律返回 false)。 */
bool bongo_cat_macos_input_supported(void) { return atomic_load(&global_supported); }

bool bongo_cat_macos_input_monitoring_authorized(void) {
    if (@available(macOS 10.15, *)) return CGPreflightListenEventAccess();
    return false;
}

bool bongo_cat_macos_input_monitoring_request(void) {
    if (@available(macOS 10.15, *)) return CGRequestListenEventAccess();
    return false;
}
#endif
