#include "windows_input_internal.h"

#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 设备表与"按住状态"的维护(接收线程)。
 *
 * 这里解决的核心问题: 多台设备可能同时按着同一个键(两把键盘都按 A), 所以按键状态
 * 不能是布尔量, 而要用引用计数 references; 只有计数由 0 变 1 才发按下、由 1 变 0 才
 * 发抬起, 已经发出去的结果记在 emitted 里用于抑制重复。
 *
 * 推送失败(队列满)时不清 emitted, 而是置 retry_events —— 下一轮由 flush 重试。这条
 * 不变量保证"按下了就一定会等到抬起", 否则猫会永远保持按下姿势。
 */
static void publish(WindowsInputState *state, WindowsRawHeld *held, bool mouse) {
    bool down = held->references != 0;
    if (down == held->emitted) return;
    BongoCatInputKind kind = mouse ? (down ? BONGO_CAT_INPUT_MOUSE_DOWN :
        BONGO_CAT_INPUT_MOUSE_UP) : (down ? BONGO_CAT_INPUT_KEY_DOWN :
        BONGO_CAT_INPUT_KEY_UP);
    if (!bongo_cat_windows_input_push_event(state, kind, held->name,
        down ? 1.0f : 0.0f)) {
        state->retry_events = true;
        return;
    }
    held->emitted = down;
}

void bongo_cat_windows_input_flush(WindowsInputState *state) {
    if (!state->retry_events) return;
    state->retry_events = false;
    for (unsigned i = 0; i < BONGO_CAT_WINDOWS_RAW_HELD_LIMIT; ++i)
        publish(state, &state->keys[i], false);
    for (unsigned i = 0; i < BONGO_CAT_WINDOWS_MOUSE_BUTTON_COUNT; ++i)
        publish(state, &state->buttons[i], true);
}

/* 按句柄查设备, 查不到就惰性新建并插到链表头(设备少但增删频繁, 链表足够)。
   设备数达到上限时返回 NULL, 调用方会把它计入 device_drops 并丢弃该报文。 */
WindowsRawDevice *bongo_cat_windows_input_device(WindowsInputState *state,
    HANDLE handle) {
    for (WindowsRawDevice *device = state->devices; device; device = device->next)
        if (device->handle == handle) return device;
    if (state->device_count >= BONGO_CAT_WINDOWS_RAW_DEVICE_LIMIT) return NULL;
    WindowsRawDevice *device = calloc(1, sizeof(*device));
    if (!device) return NULL;
    device->handle = handle;
    device->next = state->devices;
    state->devices = device;
    state->device_count++;
    return device;
}

/* 按键名找引用计数槽位(最多 256 个)。已被占用/尚未复位的槽位按名字匹配; 空槽位
   里取第一个可用的, 顺便把名字写进去。返回 -1 表示没有空位, 调用方丢事件并计数。 */
static int held_slot(WindowsInputState *state, const char *name) {
    int available = -1;
    for (unsigned i = 0; i < BONGO_CAT_WINDOWS_RAW_HELD_LIMIT; ++i) {
        WindowsRawHeld *held = &state->keys[i];
        if (held->references || held->emitted) {
            if (strcmp(held->name, name) == 0) return (int)i;
        } else if (available < 0) available = (int)i;
    }
    if (available >= 0) snprintf(state->keys[available].name,
        sizeof(state->keys[available].name), "%s", name);
    return available;
}

/*
 * 一条键盘报文 -> 按下 / 抬起事件。三处平台怪癖:
 *   - 带 E0 前缀的右 Ctrl / 右 Alt(MakeCode 0x2a / 0x36)要过滤: 那是伪造的修饰键
 *     报文, 真正的修饰键另有报文;
 *   - E1 前缀是 Pause 键独有的两段式序列, 先记 pending_e1, 下一条(0x45)才补成
 *     VK_PAUSE;
 *   - 标准键盘的 Pause 没有硬件抬起报文, 所以按下之后要立刻自己补一次抬起。
 * 另外用 device->keys[index] 做按下/抬起去重: 系统重复发的报文会在这里被挡掉。
 */
void bongo_cat_windows_input_key(WindowsInputState *state,
    WindowsRawDevice *device, const RAWKEYBOARD *key) {
    if ((key->Flags & RI_KEY_E0) &&
        (key->MakeCode == 0x2a || key->MakeCode == 0x36)) {
        state->diagnostic_filtered_keys++;
        return;
    }
    if ((key->Flags & RI_KEY_E1) && key->MakeCode == 0x1d) {
        state->diagnostic_filtered_keys++;
        device->pending_e1 = true;
        return;
    }
    RAWKEYBOARD normalized = *key;
    if (device->pending_e1) {
        device->pending_e1 = false;
        if (key->MakeCode == 0x45) normalized.VKey = VK_PAUSE;
    }
    key = &normalized;
    char buffer[16];
    const char *name = bongo_cat_windows_key_name(key, buffer);
    if (!name) { state->diagnostic_filtered_keys++; return; }
    unsigned index = bongo_cat_windows_key_index(key);
    if (index >= BONGO_CAT_WINDOWS_RAW_KEY_COUNT) {
        state->diagnostic_filtered_keys++;
        return;
    }
    unsigned slot = device->keys[index];
    bool down = !(key->Flags & RI_KEY_BREAK);
    if (down == (slot != 0)) { state->diagnostic_duplicate_keys++; return; }
    if (down) {
        int found = held_slot(state, name);
        if (found < 0) { state->diagnostic_filtered_keys++; return; }
        slot = (unsigned)found + 1;
        device->keys[index] = (unsigned short)slot;
        state->keys[slot - 1].references++;
    } else {
        device->keys[index] = 0;
        state->keys[slot - 1].references--;
    }
    publish(state, &state->keys[slot - 1], false);
    /* Pause has no hardware break packet on standard keyboards. */
    if (down && key->VKey == VK_PAUSE) {
        device->keys[index] = 0;
        state->keys[slot - 1].references--;
        publish(state, &state->keys[slot - 1], false);
    }
}

/* 鼠标按键: 一条报文可能同时带多个 down/up 位, 所以逐位判断; 同样先用
   device->buttons[i] 去重。这些名字必须与配置文件里用户写的保持一致。 */
void bongo_cat_windows_input_buttons(WindowsInputState *state,
    WindowsRawDevice *device, USHORT flags) {
    if (!flags) return;
    static const USHORT presses[] = {RI_MOUSE_LEFT_BUTTON_DOWN,
        RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN,
        RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
    static const USHORT releases[] = {RI_MOUSE_LEFT_BUTTON_UP,
        RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP,
        RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
    static const char *names[] = {"Left", "Right", "Middle", "Back", "Forward"};
    for (unsigned i = 0; i < BONGO_CAT_WINDOWS_MOUSE_BUTTON_COUNT; ++i) {
        WindowsRawHeld *held = &state->buttons[i];
        snprintf(held->name, sizeof(held->name), "%s", names[i]);
        if ((flags & presses[i]) && !device->buttons[i]) {
            device->buttons[i] = true;
            held->references++;
            publish(state, held, true);
        }
        if ((flags & releases[i]) && device->buttons[i]) {
            device->buttons[i] = false;
            held->references--;
            publish(state, held, true);
        }
    }
}

/* 设备移除时把它占用的引用还回去: 该设备按着的键与鼠标按键计数各减一, 于是"最后
   一个按住者消失"会正常产生抬起事件 —— 否则拔掉键盘会让猫一直举着手。
   末尾置 retry_events 并立刻 flush, 让补发的抬起尽快送出去。 */
void bongo_cat_windows_input_remove_device(WindowsInputState *state, HANDLE handle) {
    WindowsRawDevice **link = &state->devices;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) return;
    WindowsRawDevice *device = *link;
    for (unsigned i = 0; i < BONGO_CAT_WINDOWS_RAW_KEY_COUNT; ++i)
        if (device->keys[i]) state->keys[device->keys[i] - 1].references--;
    for (unsigned i = 0; i < BONGO_CAT_WINDOWS_MOUSE_BUTTON_COUNT; ++i)
        if (device->buttons[i]) state->buttons[i].references--;
    *link = device->next;
    free(device);
    state->device_count--;
    state->retry_events = true;
    bongo_cat_windows_input_flush(state);
}

void bongo_cat_windows_input_clear_devices(WindowsInputState *state) {
    while (state->devices)
        bongo_cat_windows_input_remove_device(state, state->devices->handle);
    bongo_cat_windows_input_clear_motion(state);
}
#endif
