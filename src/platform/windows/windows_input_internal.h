#ifndef BONGO_CAT_WINDOWS_INPUT_INTERNAL_H
#define BONGO_CAT_WINDOWS_INPUT_INTERNAL_H

#include "windows_input.h"
#include "windows_input_detection.h"
#include "windows_keys.h"
#include "bongo_cat/log.h"

#ifdef _WIN32
/*
 * 输入接收线程的内部状态与跨文件接口(只给同目录的 windows_input*.c 使用)。
 *
 * 线程模型:
 *   - 接收线程: 注册 Raw Input, 跑消息循环解析报文, 往 BongoCatInputState 推事件,
 *     并通过 SetEvent / PostMessage 唤醒主线程;
 *   - 模型线程(主线程): 只读取相对位移和观察样本(pointer_detection)。
 * 字段的所有权写在各自注释里: platform_lock 保护 platform 指针, relative_lock 保护
 * 运动缓冲区, 其余字段要么只属于接收线程, 要么只属于模型线程 —— 不要跨线程直接读写。
 */
#define BONGO_CAT_WINDOWS_RAW_DEVICE_LIMIT 64
/* 这三个上限都是定长数组的容量: 设备数、被按住的键数、鼠标按键数。
   超出时宁可丢弃并计数(diagnostic_*), 也不动态分配 —— 这条路径跑在窗口消息回调里,
   不允许失败, 也不允许阻塞。 */
#define BONGO_CAT_WINDOWS_RAW_HELD_LIMIT 256
#define BONGO_CAT_WINDOWS_MOUSE_BUTTON_COUNT 5

typedef struct WindowsRawHeld {
    char name[16];
    unsigned references;
    bool emitted;
} WindowsRawHeld;

/* 每台原始输入设备一份状态: keys/buttons 用于过滤重复和识别"按下还没抬起";
   absolute_* 缓存绝对坐标模式下的换算结果(虚拟桌面可能带负坐标, 所以必须用 bounds
   而不是 0..屏幕宽高); generation 让主线程知道设备表被重建过 —— 那之前的观测样本
   必须丢弃。 */
typedef struct WindowsRawDevice {
    struct WindowsRawDevice *next;
    HANDLE handle;
    unsigned short keys[BONGO_CAT_WINDOWS_RAW_KEY_COUNT];
    bool buttons[BONGO_CAT_WINDOWS_MOUSE_BUTTON_COUNT];
    bool absolute_known, pending_e1;
    RECT absolute_bounds;
    double absolute_x, absolute_y;
    unsigned long long generation;
} WindowsRawDevice;

typedef struct WindowsInputState {
    BongoCatPlatform *platform;
    SRWLOCK platform_lock;
    SRWLOCK relative_lock;
    HANDLE thread, stop, ready;
    volatile LONG references;
    HWND window;
    ATOM window_class;
    bool registered;
    /* 各阶段的错误码先存在这里: 接收线程没法把错误直接抛给主线程, 由主线程在合适的
       时机读取并写日志。 */
    DWORD startup_error, read_error, registration_error;
    DWORD recovery_error;
    /* Receiver-thread-only cumulative diagnostics; never store typed content. */
    unsigned long long diagnostic_keys, diagnostic_mouse, diagnostic_invalid;
    unsigned long long diagnostic_background_keys, diagnostic_filtered_keys;
    unsigned long long diagnostic_duplicate_keys;
    unsigned long long diagnostic_device_drops, diagnostic_read_failures;
    unsigned long long diagnostic_queued_keys, diagnostic_queue_failures;
    unsigned long long diagnostic_wake_failures, diagnostic_device_changes;
    ULONGLONG diagnostic_ms;
    DWORD mouse_registration_flags, keyboard_registration_flags;
    DWORD test_start_delay_ms;
    /* ownership 是"我们临时改过哪些注册标志"的位掩码: 失去前台/退出时按它还原,
       否则会把用户的系统设置改坏(见 windows_input_registration.c)。 */
    unsigned ownership;
    WindowsRawDevice *devices;
    unsigned device_count;
    WindowsRawHeld keys[BONGO_CAT_WINDOWS_RAW_HELD_LIMIT];
    WindowsRawHeld buttons[BONGO_CAT_WINDOWS_MOUSE_BUTTON_COUNT];
    bool desktop_unavailable, wake_pending, retry_events;
    /* Shared motion buffers are protected by relative_lock. Detection and
       model consumption have separate totals; neither drains the other's data. */
    bool relative_active, receiving;
    long long relative_x, relative_y;
    double absolute_x, absolute_y;
    unsigned long long relative_samples, absolute_samples, generation;
    long long observed_x, observed_y;
    double observed_absolute_x, observed_absolute_y;
    unsigned long long observed_motion, observed_generation;
    /* Model-thread-only state. The receiver publishes resets through generation. */
    WindowsPointerDetection pointer_detection;
    ULONGLONG pointer_probe_ms;
    unsigned long long pointer_generation;
    bool pointer_probe_ready;
} WindowsInputState;

/* 下面的入口按调用线程分成三组: 接收线程(register / unregister / packet / key /
   buttons / motion / flush / wake)、模型线程(take_observation 与 pointer 相关), 以及
   生命周期(receiver_create / destroy / dispatch)。跨组调用前先确认锁与所有权。 */
bool bongo_cat_windows_input_push_event(WindowsInputState *state,
    BongoCatInputKind kind, const char *name, float value);
void bongo_cat_windows_input_wake(WindowsInputState *state);
void bongo_cat_windows_input_flush(WindowsInputState *state);
void bongo_cat_windows_input_packet(WindowsInputState *state,
    const RAWINPUT *packet, UINT bytes);
WindowsRawDevice *bongo_cat_windows_input_device(WindowsInputState *state,
    HANDLE handle);
void bongo_cat_windows_input_remove_device(WindowsInputState *state, HANDLE handle);
void bongo_cat_windows_input_clear_devices(WindowsInputState *state);
void bongo_cat_windows_input_key(WindowsInputState *state,
    WindowsRawDevice *device, const RAWKEYBOARD *key);
void bongo_cat_windows_input_buttons(WindowsInputState *state,
    WindowsRawDevice *device, USHORT flags);
void bongo_cat_windows_input_motion(WindowsInputState *state,
    WindowsRawDevice *device, const RAWMOUSE *mouse, const RECT *bounds);
void bongo_cat_windows_input_clear_motion(WindowsInputState *state);
unsigned long long bongo_cat_windows_input_take_observation(
    WindowsInputState *state, WindowsPointerObservation *sample);
bool bongo_cat_windows_input_register(WindowsInputState *state);
void bongo_cat_windows_input_unregister(WindowsInputState *state);
unsigned bongo_cat_windows_input_ownership(WindowsInputState *state);
unsigned bongo_cat_windows_input_restore(WindowsInputState *state, unsigned owned);
bool bongo_cat_windows_input_receiver_create(WindowsInputState *state);
void bongo_cat_windows_input_receiver_destroy(WindowsInputState *state);
bool bongo_cat_windows_input_dispatch(WindowsInputState *state);
#endif
#endif
