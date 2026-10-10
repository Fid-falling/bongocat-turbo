#include "linux_evdev_internal.h"
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/*
 * 内核事件 -> BongoCatInputEvent 的翻译, 以及唤醒主循环。
 *
 * 唤醒策略: 只在"队列由空变非空"的那一刻推一次 SDL 用户事件, 否则高回报率鼠标会把
 * 事件队列塞满。
 */
static void wake_consumer(LinuxEvdevState *state) {
    SDL_Event wake = {0};
    wake.type = state->platform->wake_event_type;
    SDL_PushEvent(&wake);
}

/* 鼠标按键名必须与配置文件以及 Windows 侧共用同一套(Left/Right/Middle/Back/Forward),
   这样快捷键配置才能跨平台通用。 */
static const char *button_name(unsigned code) {
    switch (code) {
    case BTN_LEFT: return "Left";
    case BTN_MIDDLE: return "Middle";
    case BTN_RIGHT: return "Right";
    case BTN_SIDE: return "Back";
    case BTN_EXTRA: return "Forward";
    default: return NULL;
    }
}

/*
 * 单个按键/按钮的状态变化 -> 事件。三道过滤:
 *   - 与这台设备上次的状态相同(重复报文)直接丢弃;
 *   - 键名映射不到(未收录的键)丢弃;
 *   - 键的种类与设备类型不匹配(键盘设备报鼠标键, 或反之)丢弃。
 * held[] 是跨设备的引用计数: 只有 0->1 才发按下、1->0 才发抬起, 于是两把键盘按同一个键
 * 不会互相打断。
 */
static void key_event(LinuxEvdevState *state, EvdevDevice *device,
    unsigned code, bool down) {
    if (code >= KEY_CNT || device->down[code] == down) return;
    bool mouse = code >= BTN_MISC;
    const char *name = mouse ? button_name(code) : bongo_cat_evdev_key_name(code);
    if (!name || (mouse ? !device->pointer : !device->keyboard)) return;
    unsigned index = bongo_cat_evdev_key_index(code);
    device->down[code] = down;
    if (down) {
        if (state->held[index]++ > 0) return;
    } else {
        if (!state->held[index] || --state->held[index] > 0) return;
    }
    BongoCatInputEvent input = {
        .kind = mouse ? (down ? BONGO_CAT_INPUT_MOUSE_DOWN : BONGO_CAT_INPUT_MOUSE_UP) :
            (down ? BONGO_CAT_INPUT_KEY_DOWN : BONGO_CAT_INPUT_KEY_UP),
        .timestamp_ms = SDL_GetTicks(), .value = down ? 1.0f : 0.0f
    };
    snprintf(input.name, sizeof(input.name), "%s", name);
    if (bongo_cat_input_push(state->platform->input, &input)) wake_consumer(state);
}

/* 把该设备当前按着的所有键补发抬起。设备被拔掉、内核丢事件(SYN_DROPPED)、程序退出时
   都要靠它兜底 —— 否则猫会永远保持按下姿势。 */
void bongo_cat_evdev_release(LinuxEvdevState *state, EvdevDevice *device) {
    for (unsigned code = 0; code < KEY_CNT; ++code)
        if (device->down[code]) key_event(state, device, code, false);
}

void bongo_cat_evdev_motion_reset(LinuxEvdevState *state) {
    SDL_LockMutex(state->motion_lock);
    state->relative_x = state->relative_y = 0;
    SDL_UnlockMutex(state->motion_lock);
}

/* 单条事件的分发。SYN_DROPPED 表示内核事件队列溢出, 此时设备状态已不可信: 先补发所有
   抬起来止损, 再进入 desynchronized, 忽略这一包不完整的数据, 直到看见完整的 SYN_REPORT
   才恢复采信(见下面原有的英文注释)。
   相对位移会钳到 ±32768: 内核给的是 int, 但一个异常大的值足以把猫甩出屏幕。 */
void bongo_cat_evdev_event(LinuxEvdevState *state, EvdevDevice *device,
    const struct input_event *event) {
    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        bongo_cat_evdev_release(state, device);
        bongo_cat_evdev_motion_reset(state);
        device->desynchronized = true;
        return;
    }
    /* No state-query ioctl is permitted. After overflow, release known keys
       and ignore the incomplete packet; held keys resume on their next press. */
    if (device->desynchronized) {
        if (event->type == EV_SYN && event->code == SYN_REPORT)
            device->desynchronized = false;
        return;
    }
    if (event->type == EV_KEY && (event->value == 0 || event->value == 1)) {
        key_event(state, device, event->code, event->value == 1);
    } else if (event->type == EV_REL && device->relative &&
        (event->code == REL_X || event->code == REL_Y) && event->value) {
        SDL_LockMutex(state->motion_lock);
        bool wake = state->relative_x == 0 && state->relative_y == 0;
        double *axis = event->code == REL_X ? &state->relative_x : &state->relative_y;
        *axis = SDL_clamp(*axis + (double)event->value, -32768.0, 32768.0);
        SDL_UnlockMutex(state->motion_lock);
        if (wake) wake_consumer(state);
    }
}

/* 一次读一批事件(64 条)。返回值表示"这台设备还能不能继续用": 读空或 EINTR/EAGAIN 都是
   正常情况(非阻塞 fd), 真正的错误、以及"字节数不是事件大小的整数倍"才让调用方摘掉
   设备。 */
bool bongo_cat_evdev_read(LinuxEvdevState *state, EvdevDevice *device) {
    struct input_event events[64];
    ssize_t bytes = read(device->fd, events, sizeof(events));
    if (bytes < 0) return errno == EAGAIN || errno == EINTR;
    if (!bytes || (size_t)bytes % sizeof(events[0])) return false;
    size_t count = (size_t)bytes / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i)
        bongo_cat_evdev_event(state, device, &events[i]);
    return true;
}
