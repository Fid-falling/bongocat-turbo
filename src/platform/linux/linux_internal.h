#ifndef BONGO_CAT_LINUX_INTERNAL_H
#define BONGO_CAT_LINUX_INTERNAL_H

#include "bongo_cat/platform.h"

typedef struct LinuxEvdevState LinuxEvdevState;
/*
 * Linux 平台层的内部状态与跨文件接口。
 *
 * 两条并存的后端: X11(XInput2 + Xlib)与 evdev(直接读内核键鼠事件流)。选择依据见
 * bongo_cat_linux_evdev_requested: Wayland 下 X11 客户端拿不到别人的按键, 必须用 evdev;
 * X11 下则优先用 XInput2(不需要额外权限)。x11 与 evdev 两个指针可以同时非空 —— 各自由
 * 需要决定启停, 不要假设"有一个就没有另一个"。
 * always_on_top 是平台层自己维护的当前置顶状态: set_above 靠它判断是否需要真的去调用
 * X11, 所以任何绕过它直接改窗口状态的地方都必须同步这个字段。
 */
typedef struct LinuxPlatformState {
    void *x11;
    LinuxEvdevState *evdev;
    bool wayland;
    bool evdev_selected;
    bool always_on_top;
} LinuxPlatformState;

bool bongo_cat_linux_evdev_requested(const char *option, bool wayland);

bool bongo_cat_linux_x11_start(BongoCatPlatform *platform, BongoCatError *error);
void bongo_cat_linux_x11_stop(BongoCatPlatform *platform);
/* Read-only evdev keyboard listener. Wayland compositors never forward other
   applications' keys to an X11 client, so XInput2 alone cannot show key
   presses on Wayland; this backend observes the kernel key stream instead. */
bool bongo_cat_linux_evdev_start(BongoCatPlatform *platform, BongoCatError *error);
void bongo_cat_linux_evdev_stop(BongoCatPlatform *platform);
bool bongo_cat_linux_evdev_pointer_active(const BongoCatPlatform *platform);
/* Relative pointer delta accumulated by the evdev listener. Wayland never
   exposes the global cursor position to other clients, so the pet follows
   relative motion from the kernel instead of absolute coordinates. */
bool bongo_cat_linux_evdev_relative_pointer(BongoCatPlatform *platform,
    double *dx, double *dy);
void bongo_cat_linux_evdev_relative_pointer_reset(BongoCatPlatform *platform);
/* 下面这组是 X11 专有的窗口行为接口: supported / xwayland 用来判断当前环境有没有可用
   的 X11 连接(例如是否跑在 Wayland 的 XWayland 兼容层上), 其余几个分别是点击穿透、
   置顶、采集窗口配置与窗口拖动。 */
bool bongo_cat_linux_x11_supported(const BongoCatPlatform *platform);
bool bongo_cat_linux_x11_xwayland(const BongoCatPlatform *platform);
void bongo_cat_linux_x11_click_through(BongoCatPlatform *platform, bool enabled);
void bongo_cat_linux_x11_set_above(BongoCatPlatform *platform, bool enabled);
void bongo_cat_linux_x11_configure_capture_window(BongoCatPlatform *platform);
void bongo_cat_linux_x11_begin_drag(BongoCatPlatform *platform);

#endif
