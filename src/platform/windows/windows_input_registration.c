#include "windows_input_internal.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <stdlib.h>

/*
 * Raw Input 的注册与恢复。
 *
 * 目标状态: 鼠标与键盘两类都以 RIDEV_INPUTSINK 注册到我们自己的窗口, 也就是窗口
 * 不在前台时也能收到输入 —— 这正是相对全局钩子的优势, 不会拖慢系统输入。
 *
 * 两个绕不开的现实:
 *   - GetRegisteredRawInputDevices 的大小查询可能返回 ERROR_INSUFFICIENT_BUFFER,
 *     所以要先查一次、分配、再查一次, 并且最多重试 3 轮(两次查询之间设备表还可能
 *     变化);
 *   - 注册是进程级的, 别的模块(或上一次异常退出)可能占着这两类设备, 那就只能放弃
 *     并如实上报(ERROR_BUSY), 绝不能强行抢。
 */
static bool registrations(WindowsInputState *state, RAWINPUTDEVICE **items,
    UINT *count) {
    *items = NULL;
    *count = 0;
    state->registration_error = ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        UINT queried = GetRegisteredRawInputDevices(NULL, count, sizeof(**items));
        if (queried == (UINT)-1) {
            DWORD error = GetLastError();
            /* A size query may report the required count as insufficient buffer. */
            if (error != ERROR_INSUFFICIENT_BUFFER || !*count) {
                state->registration_error = error;
                return false;
            }
        }
        if (!*count) return true;
        if (*count > 1024) {
            state->registration_error = ERROR_INVALID_DATA;
            return false;
        }
        *items = calloc(*count, sizeof(**items));
        if (!*items) {
            state->registration_error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }
        UINT received = GetRegisteredRawInputDevices(*items, count, sizeof(**items));
        if (received != (UINT)-1) { *count = received; return true; }
        DWORD error = GetLastError();
        free(*items);
        *items = NULL;
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            state->registration_error = error;
            return false;
        }
    }
    state->registration_error = ERROR_INSUFFICIENT_BUFFER;
    return false;
}

/* 只关心 HID 的通用桌面设备(usage page = 1): mouse = usage 2, keyboard = usage 6。
   返回值是位掩码的位号, 0 表示"不是我们要的设备"。 */
static unsigned device_class(const RAWINPUTDEVICE *device) {
    if (device->usUsagePage != 1) return 0;
    return device->usUsage == 2 ? 1u : device->usUsage == 6 ? 2u : 0u;
}

/* 查一遍当前生效的注册, 判断这两类设备里哪些确实是我们以 INPUTSINK 注册的, 同时
   把鼠标/键盘的 dwFlags 记下来 —— 恢复时要按原样还原, 不能一律改成 INPUTSINK。 */
unsigned bongo_cat_windows_input_ownership(WindowsInputState *state) {
    RAWINPUTDEVICE *items;
    UINT count;
    if (!registrations(state, &items, &count)) return 0;
    unsigned owned = 0;
    state->mouse_registration_flags = state->keyboard_registration_flags = 0;
    for (UINT i = 0; i < count; ++i) {
        if (items[i].hwndTarget != state->window) continue;
        unsigned kind = device_class(&items[i]);
        if (kind == 1) state->mouse_registration_flags = items[i].dwFlags;
        if (kind == 2) state->keyboard_registration_flags = items[i].dwFlags;
        /* Windows may omit DEVNOTIFY when querying a successful registration. */
        if (items[i].dwFlags & RIDEV_INPUTSINK) owned |= kind;
    }
    free(items);
    return owned;
}

/* 注册前先确认没人占着这两类设备: 注册是进程级的, 抢别人的注册等于破坏别人的输入。
   被占用时报 ERROR_BUSY, 上层据此把"没有全局输入"降级成警告而不是致命错误。
   BONGO_CAT_TEST_RAW_INPUT_FAILURE 是测试钩子, 用来在 CI 上模拟注册失败。 */
bool bongo_cat_windows_input_register(WindowsInputState *state) {
    if (SDL_getenv("BONGO_CAT_TEST_RAW_INPUT_FAILURE")) {
        state->startup_error = ERROR_ACCESS_DENIED;
        return false;
    }
    RAWINPUTDEVICE *items;
    UINT count;
    if (!registrations(state, &items, &count)) {
        state->startup_error = state->registration_error;
        return false;
    }
    bool occupied = false;
    for (UINT i = 0; i < count; ++i)
        if (device_class(&items[i])) occupied = true;
    free(items);
    if (occupied) { state->startup_error = ERROR_BUSY; return false; }
    const RAWINPUTDEVICE devices[] = {
        {1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, state->window},
        {1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, state->window}
    };
    if (!RegisterRawInputDevices(devices, 2, sizeof(devices[0]))) {
        state->startup_error = GetLastError();
        return false;
    }
    return true;
}

/* 失去前台焦点后, 系统可能把 INPUTSINK 注册降级为仅前台, 于是后台收不到按键。
   这里按"我们还缺哪些类别"补齐(见下面原有的英文注释: 别人的注册一律不动), 返回
   更新后的 owned 掩码, 补齐成功的类别被加进去。 */
unsigned bongo_cat_windows_input_restore(WindowsInputState *state, unsigned owned) {
    if (owned == 3) return owned;
    RAWINPUTDEVICE *items;
    UINT count;
    if (!registrations(state, &items, &count)) return owned;
    unsigned blocked = 0;
    for (UINT i = 0; i < count; ++i) {
        /* Registrations are process-local. Leave another receiver (including
           a foreground-only registration with no target) alone. */
        if (items[i].hwndTarget != state->window)
            blocked |= device_class(&items[i]);
    }
    free(items);
    unsigned missing = 3u & ~(owned | blocked);
    if (!missing) return owned;
    RAWINPUTDEVICE devices[2];
    UINT pending = 0;
    if (missing & 1u) devices[pending++] = (RAWINPUTDEVICE){
        1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, state->window};
    if (missing & 2u) devices[pending++] = (RAWINPUTDEVICE){
        1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, state->window};
    if (!RegisterRawInputDevices(devices, pending, sizeof(devices[0]))) {
        DWORD error = GetLastError();
        if (error != state->recovery_error) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
            "[input] Raw Input recovery failed: classes=%u error=%lu",
            missing, (unsigned long)error);
        state->recovery_error = error;
        return owned;
    }
    state->recovery_error = ERROR_SUCCESS;
    SDL_LogInfo(BONGO_CAT_LOG_INPUT,
        "[input] Raw Input background registration restored: classes=%u", missing);
    return owned | missing;
}

/* 注销只处理属于我们窗口的那些注册(RIDEV_REMOVE 且 hwndTarget 置空), 别人的注册
   一律不碰。注销失败只记警告: 进程退出时系统也会清理。 */
void bongo_cat_windows_input_unregister(WindowsInputState *state) {
    RAWINPUTDEVICE *items;
    UINT count;
    if (!registrations(state, &items, &count)) return;
    for (UINT i = 0; i < count; ++i) {
        if (!device_class(&items[i]) || items[i].hwndTarget != state->window)
            continue;
        items[i].dwFlags = RIDEV_REMOVE;
        items[i].hwndTarget = NULL;
        if (!RegisterRawInputDevices(&items[i], 1, sizeof(items[i])))
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "[input] Raw Input unregister failed: error=%lu",
                (unsigned long)GetLastError());
    }
    free(items);
}
#endif
