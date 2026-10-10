#ifndef BONGO_CAT_LINUX_EVDEV_INTERNAL_H
#define BONGO_CAT_LINUX_EVDEV_INTERNAL_H

#include "linux_internal.h"
#include <SDL3/SDL.h>
#include <linux/input.h>

/*
 * evdev 后端的内部状态与跨文件接口(只给 linux_evdev*.c 使用)。
 *
 * 线程模型: 接收线程独占 devices[] / held[] / epoll_fd; 只有 relative_x/y 会被主线程
 * 读(用 motion_lock 保护), pointer_active 是原子标志。
 * 三个上限都是定长数组(设备数 32、节点名 32 字节、按键计数表按 KEY_CNT), 全部静态分配 ——
 * 这条路径跑在事件循环里, 不允许失败也不允许阻塞。
 * EVDEV_SCAN_INTERVAL_NS(2 秒)重扫一次设备: 不依赖 inotify/udev, 代价是热插拔最多延迟
 * 两秒; EVDEV_POLL_MS 是 epoll 的超时, 让线程能及时看到 running 变成 false。
 */
#define EVDEV_MAX_DEVICES 32
#define EVDEV_NODE_CAP 32
#define EVDEV_SCAN_INTERVAL_NS 2000000000ull
#define EVDEV_POLL_MS 250

/* 每台设备一份状态。down[KEY_CNT] 记录"这台设备按着哪些键", held[] 是全局引用计数 ——
   与 Windows 侧同一个理由: 多台设备可能同时按同一个键, 只有计数归零才算真的抬起。
   desynchronized 表示内核丢弃过事件(SYN_DROPPED), 此时设备状态已不可信, 要等到一个
   完整的 SYN_REPORT 才能重新采信。 */
typedef struct EvdevDevice {
    int fd;
    bool keyboard;
    bool pointer;
    bool relative;
    bool desynchronized;
    bool down[KEY_CNT];
    char node[EVDEV_NODE_CAP];
} EvdevDevice;

/* held[] 由接收线程独占访问(无需加锁); relative_x/y 由 motion_lock 保护, 因为主线程
   会在每一帧取走它。 */
struct LinuxEvdevState {
    BongoCatPlatform *platform;
    SDL_Thread *thread;
    SDL_Mutex *motion_lock;
    atomic_bool running;
    atomic_bool pointer_active;
    int epoll_fd;
    EvdevDevice devices[EVDEV_MAX_DEVICES];
    size_t device_count;
    size_t reported_count;
    bool denied_reported;
    unsigned held[KEY_CNT];
    double relative_x;
    double relative_y;
};

const char *bongo_cat_evdev_key_name(unsigned code);
unsigned bongo_cat_evdev_key_index(unsigned code);
bool bongo_cat_evdev_node_valid(const char *node);
bool bongo_cat_evdev_mask_bit(const char *text, unsigned word_bits, unsigned code);
void bongo_cat_evdev_event(LinuxEvdevState *state, EvdevDevice *device,
    const struct input_event *event);
void bongo_cat_evdev_release(LinuxEvdevState *state, EvdevDevice *device);
bool bongo_cat_evdev_read(LinuxEvdevState *state, EvdevDevice *device);
void bongo_cat_evdev_scan(LinuxEvdevState *state);
void bongo_cat_evdev_remove(LinuxEvdevState *state, size_t index);
void bongo_cat_evdev_motion_reset(LinuxEvdevState *state);

#endif
