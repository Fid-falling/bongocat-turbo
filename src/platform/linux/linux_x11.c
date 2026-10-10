#include "linux_internal.h"
#include "bongo_cat/common.h"
#include "bongo_cat/log.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * X11 后端的窗口行为与全局输入(XInput2 raw events)。
 *
 * 这里有两件事: 窗口行为(点击穿透、置顶、跳过任务栏、拖动)与 XInput2 的键鼠采集。采集
 * 用自己的 Display 连接和一条专用线程(见 input_thread), 而不复用 SDL 的连接 —— Xlib 不是
 * 线程安全的, 两条线程共用一个 Display 就得处处 XInitThreads/XLockDisplay, 独立连接简单
 * 得多。
 *
 * Wayland 相关的坑: XWayland 会声明一个 XWAYLAND 扩展用于识别(原生 X 服务器没有); 而且
 * 它在窗口输入区域为空时就不再转发指针事件, 所以 XWayland 下要禁用"按像素穿透"(见
 * linux.c 的 dynamic_hit_supported), 并把置顶改成显式发 EWMH 请求。
 */
typedef struct LinuxX11State {
    BongoCatPlatform *platform;
    Display *display;
    Window window;
    SDL_Thread *thread;
    bool key_down[BONGO_CAT_INPUT_KEY_STATE_CAP];
    bool xwayland;
    atomic_bool running;
    atomic_bool supported;
} LinuxX11State;

static LinuxX11State *x11_state(const BongoCatPlatform *platform) {
    const LinuxPlatformState *native = platform ? platform->native : NULL;
    return native ? native->x11 : NULL;
}

/* KeySym -> 事件名。名字必须与 Windows / evdev 两侧共用同一套字符串(配置文件要跨平台
   通用)。小键盘在 NumLock 关闭时会上报 XK_KP_Insert 这类别名, 所以 XK_KP_Insert 到
   XK_KP_Delete 那一段要单独映射回 Kp0..KpDecimal。返回 NULL 表示这个键不支持。 */
static const char *key_name(KeySym key, char output[16]) {
    if (key >= XK_a && key <= XK_z) {
        snprintf(output, 16, "Key%c", (char)('A' + key - XK_a)); return output;
    }
    if (key >= XK_0 && key <= XK_9) {
        snprintf(output, 16, "Num%c", (char)key); return output;
    }
    if (key >= XK_F1 && key <= XK_F24) {
        snprintf(output, 16, "F%lu", (unsigned long)(key - XK_F1 + 1)); return output;
    }
    switch (key) {
    case XK_Escape: return "Escape"; case XK_Tab: return "Tab";
    case XK_Pause: return "Pause"; case XK_Print: return "PrintScreen";
    case XK_Menu: return "Apps";
    case XK_Caps_Lock: return "CapsLock"; case XK_space: return "Space";
    case XK_BackSpace: return "Backspace"; case XK_Delete: return "Delete";
    case XK_Insert: return "Insert"; case XK_Home: return "Home";
    case XK_End: return "End"; case XK_Page_Up: return "PageUp";
    case XK_Page_Down: return "PageDown"; case XK_Up: return "UpArrow";
    case XK_Down: return "DownArrow"; case XK_Left: return "LeftArrow";
    case XK_Right: return "RightArrow"; case XK_Super_L: case XK_Super_R: return "Meta";
    case XK_Shift_L: return "ShiftLeft"; case XK_Shift_R: return "ShiftRight";
    case XK_Control_L: return "ControlLeft"; case XK_Control_R: return "ControlRight";
    case XK_Alt_L: return "Alt"; case XK_Alt_R: case XK_ISO_Level3_Shift: return "AltGr";
    case XK_Return: return "Return"; case XK_grave: return "BackQuote";
    case XK_Num_Lock: return "NumLock"; case XK_Scroll_Lock: return "ScrollLock";
    case XK_KP_0: return "Kp0"; case XK_KP_1: return "Kp1";
    case XK_KP_2: return "Kp2"; case XK_KP_3: return "Kp3";
    case XK_KP_4: return "Kp4"; case XK_KP_5: return "Kp5";
    case XK_KP_6: return "Kp6"; case XK_KP_7: return "Kp7";
    case XK_KP_8: return "Kp8"; case XK_KP_9: return "Kp9";
    case XK_KP_Insert: return "Kp0"; case XK_KP_End: return "Kp1";
    case XK_KP_Down: return "Kp2"; case XK_KP_Page_Down: return "Kp3";
    case XK_KP_Left: return "Kp4"; case XK_KP_Begin: return "Kp5";
    case XK_KP_Right: return "Kp6"; case XK_KP_Home: return "Kp7";
    case XK_KP_Up: return "Kp8"; case XK_KP_Page_Up: return "Kp9";
    case XK_KP_Delete: return "KpDecimal"; case XK_KP_Enter: return "Return";
    case XK_KP_Multiply: return "KpMultiply"; case XK_KP_Add: return "KpPlus";
    case XK_KP_Subtract: return "KpMinus"; case XK_KP_Decimal: return "KpDecimal";
    case XK_KP_Divide: return "KpDivide";
    case XK_minus: return "Minus"; case XK_equal: return "Equal";
    case XK_bracketleft: return "BracketLeft"; case XK_bracketright: return "BracketRight";
    case XK_backslash: return "Backslash"; case XK_semicolon: return "Semicolon";
    case XK_apostrophe: return "Quote"; case XK_comma: return "Comma";
    case XK_period: return "Period"; case XK_slash: return "Slash";
    default: return NULL;
    }
}

/* 推事件并唤醒主循环: 只在真正入队成功时才推唤醒事件, 免得队列满时刷一堆无用事件。 */
static void push(LinuxX11State *state, BongoCatInputKind kind,
    const char *name, float value) {
    if (!name) return;
    BongoCatInputEvent input = {.kind = kind, .timestamp_ms = SDL_GetTicks(), .value = value};
    snprintf(input.name, sizeof(input.name), "%s", name);
    if (bongo_cat_input_push(state->platform->input, &input)) {
        SDL_Event wake = {0};
        wake.type = state->platform->wake_event_type; SDL_PushEvent(&wake);
    }
}

static void wake_mouse(LinuxX11State *state, double x, double y) {
    if (bongo_cat_input_mouse(state->platform->input, x, y)) {
        SDL_Event wake = {0};
        wake.type = state->platform->wake_event_type;
        SDL_PushEvent(&wake);
    }
}

/* 原始移动事件本身不带坐标, 只能主动查一次根窗口的指针位置(XQueryPointer)。这也正是
   Wayland 下拿不到绝对坐标、必须改用 evdev 相对位移的原因。 */
static void pointer(LinuxX11State *state, Display *display) {
    Window root = DefaultRootWindow(display), child;
    int root_x, root_y, window_x, window_y; unsigned int mask;
    if (XQueryPointer(display, root, &root, &child, &root_x, &root_y,
        &window_x, &window_y, &mask))
        wake_mouse(state, root_x, root_y);
}

/* 原始事件分发。键盘用边沿检测去重(X 会为自动重复连发按下); 鼠标只认 1/2/3 三个键
   (X11 用的是物理按键编号, 与配置里的名字在这里对齐); 任何移动都转成一次指针查询。 */
static void raw_event(LinuxX11State *state, Display *display, XIRawEvent *raw) {
    char buffer[16]; const char *name = NULL;
    if (raw->evtype == XI_RawKeyPress || raw->evtype == XI_RawKeyRelease) {
        name = key_name(XkbKeycodeToKeysym(display, (KeyCode)raw->detail, 0, 0), buffer);
        bool down = raw->evtype == XI_RawKeyPress;
        if (bongo_cat_input_edge(state->key_down, raw->detail, down))
            push(state, down ? BONGO_CAT_INPUT_KEY_DOWN :
                BONGO_CAT_INPUT_KEY_UP, name, down ? 1.0f : 0.0f);
    } else if (raw->evtype == XI_RawButtonPress || raw->evtype == XI_RawButtonRelease) {
        if (raw->detail == 1) name = "Left";
        else if (raw->detail == 2) name = "Middle";
        else if (raw->detail == 3) name = "Right";
        if (name) {
            bool down = raw->evtype == XI_RawButtonPress;
            push(state, down ? BONGO_CAT_INPUT_MOUSE_DOWN : BONGO_CAT_INPUT_MOUSE_UP,
                name, down ? 1.0f : 0.0f);
        }
    } else if (raw->evtype == XI_RawMotion) pointer(state, display);
}

/*
 * 输入线程: 开自己的 Display 连接, 校验 XInput2 可用, 在根窗口上订阅 raw 事件, 然后轮询
 * XPending + SDL_Delay(8)。没有用 XNextEvent 阻塞, 是为了能周期性检查 running 标志 ——
 * 8ms 约等于 120Hz, 比任何人类的输入频率都高。
 * 订阅用 XIAllMasterDevices: 主设备是合并后的输入流, 不会因热插拔产生重复事件。
 */
static int SDLCALL input_thread(void *userdata) {
    LinuxX11State *state = userdata;
    Display *display = XOpenDisplay(NULL);
    int opcode, event, error, major = 2, minor = 0;
    if (!display || !XQueryExtension(display, "XInputExtension", &opcode, &event, &error) ||
        XIQueryVersion(display, &major, &minor) != Success) {
        if (display) XCloseDisplay(display);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "XInput2 is unavailable; global input monitoring is disabled");
        return 1;
    }
    unsigned char bits[(XI_LASTEVENT + 7) / 8] = {0};
    XISetMask(bits, XI_RawKeyPress); XISetMask(bits, XI_RawKeyRelease);
    XISetMask(bits, XI_RawButtonPress); XISetMask(bits, XI_RawButtonRelease);
    XISetMask(bits, XI_RawMotion);
    XIEventMask mask = {XIAllMasterDevices, sizeof(bits), bits};
    XISelectEvents(display, DefaultRootWindow(display), &mask, 1); XFlush(display);
    atomic_store(&state->supported, true);
    while (atomic_load(&state->running)) {
        while (XPending(display)) {
            XEvent next; XNextEvent(display, &next);
            if (next.xcookie.type != GenericEvent || next.xcookie.extension != opcode ||
                !XGetEventData(display, &next.xcookie)) continue;
            raw_event(state, display, next.xcookie.data);
            XFreeEventData(display, &next.xcookie);
        }
        SDL_Delay(8);
    }
    XCloseDisplay(display); return 0;
}

/*
 * 启动: 从 SDL 的窗口属性里借出 Display 与窗口 id(只读指针, 不接管所有权), 并判定是否
 * 跑在 XWayland 上(见下面原有的英文注释)。
 * 一个会话只选一条输入后端: 选了 evdev 就不再起 XInput2 线程 —— 设备热插拔不能让"谁在
 * 生产事件"中途改变, 否则按住不放的键会永远等不到抬起(见下面原有的英文注释)。
 */
bool bongo_cat_linux_x11_start(BongoCatPlatform *platform, BongoCatError *error) {
    if (!platform || !platform->native || !platform->window) return false;
    if (x11_state(platform)) return true;
    SDL_PropertiesID properties = SDL_GetWindowProperties(platform->window);
    Display *display = SDL_GetPointerProperty(properties,
        SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    Window window = (Window)SDL_GetNumberProperty(properties,
        SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    LinuxX11State *state = calloc(1, sizeof(*state));
    if (!state) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot allocate X11 input state");
        return false;
    }
    state->platform = platform; state->display = display; state->window = window;
    atomic_init(&state->running, true); atomic_init(&state->supported, false);
    LinuxPlatformState *native = platform->native;
    native->x11 = state;
    /* XWayland advertises a marker extension that native X servers lack.
       XWayland stops delivering pointer events for a window whose input region
       is empty, so dynamic hit testing cannot restore the region by itself. */
    int marker_opcode = 0, marker_event = 0, marker_error = 0;
    state->xwayland = native->wayland || (display && XQueryExtension(display,
        "XWAYLAND", &marker_opcode, &marker_event, &marker_error));
    if (state->xwayland) SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "XWayland detected; automatic transparent-pixel click-through is disabled");
    /* Select one input backend for the session. Device hotplug must not
       switch producers midway through a held key or mouse button. */
    if (!display || !window || native->evdev_selected) return true;
    state->thread = SDL_CreateThread(input_thread,
        BONGO_CAT_SLUG "-x11-input", state);
    if (!state->thread) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Cannot start X11 input listener");
        return false;
    }
    return true;
}

/* 停止: 先清 running 再等线程退出(线程自己关 Display), 然后释放状态并清空指针。 */
void bongo_cat_linux_x11_stop(BongoCatPlatform *platform) {
    LinuxX11State *state = x11_state(platform);
    if (!state) return;
    atomic_store(&state->running, false);
    if (state->thread) SDL_WaitThread(state->thread, NULL);
    free(state);
    ((LinuxPlatformState *)platform->native)->x11 = NULL;
}

bool bongo_cat_linux_x11_supported(const BongoCatPlatform *platform) {
    const LinuxX11State *state = x11_state(platform);
    return state && atomic_load(&state->supported);
}

bool bongo_cat_linux_x11_xwayland(const BongoCatPlatform *platform) {
    const LinuxX11State *state = x11_state(platform);
    return state && state->xwayland;
}

/* 点击穿透: 用 XFixes 把窗口的 ShapeInput 区域设成空(全部穿透)或 None(恢复默认)。
   这是整窗级别的开关, 没有逐像素版本。 */
void bongo_cat_linux_x11_click_through(BongoCatPlatform *platform, bool enabled) {
    LinuxX11State *state = x11_state(platform);
    if (!state || !state->display || !state->window) return;
    XserverRegion region = enabled ? XFixesCreateRegion(state->display, NULL, 0) : None;
    XFixesSetWindowShapeRegion(state->display, state->window, ShapeInput, 0, 0, region);
    if (region) XFixesDestroyRegion(state->display, region);
    XFlush(state->display);
}

/* 按 EWMH 约定向根窗口发 _NET_WM_STATE 客户端消息(action: 0 = 移除, 1 = 添加,
   2 = 切换)。窗口管理器不一定会照做(取决于合成器), 所以这些都只是"请求"。 */
static void state_message(LinuxX11State *state, const char *name, long action) {
    Atom state_atom = XInternAtom(state->display, "_NET_WM_STATE", False);
    Atom target = XInternAtom(state->display, name, False);
    XEvent event = {0}; event.xclient.type = ClientMessage; event.xclient.window = state->window;
    event.xclient.message_type = state_atom; event.xclient.format = 32;
    event.xclient.data.l[0] = action; event.xclient.data.l[1] = (long)target;
    XSendEvent(state->display, DefaultRootWindow(state->display), False,
        SubstructureRedirectMask | SubstructureNotifyMask, &event); XFlush(state->display);
}

void bongo_cat_linux_x11_set_above(BongoCatPlatform *platform, bool enabled) {
    LinuxX11State *state = x11_state(platform);
    if (!state || !state->display || !state->window) return;
    /* Some XWayland compositors need an explicit EWMH request after mapping. */
    state_message(state, "_NET_WM_STATE_ABOVE", enabled ? 1 : 0);
}

/* 把窗口标记为不进任务栏(_NET_WM_STATE_SKIP_TASKBAR): 桌宠不该占一个任务栏条目。
   X11 下没有"只在采集软件里显示"的等价机制, 所以这里只做这一件事。 */
void bongo_cat_linux_x11_configure_capture_window(BongoCatPlatform *platform) {
    LinuxX11State *state = x11_state(platform);
    if (state && state->display && state->window)
        state_message(state, "_NET_WM_STATE_SKIP_TASKBAR", 1);
}

/* 拖动: 按 EWMH 发 _NET_WM_MOVERESIZE, data.l[2] = 8 表示"移动", data.l[3] = 1 表示
   "来自应用"的手势。于是拖动由窗口管理器接管, 我们不必自己跟踪鼠标。 */
void bongo_cat_linux_x11_begin_drag(BongoCatPlatform *platform) {
    LinuxX11State *state = x11_state(platform);
    if (!state || !state->display || !state->window) return;
    Window root = DefaultRootWindow(state->display), child;
    int x, y, wx, wy; unsigned int mask;
    XQueryPointer(state->display, root, &root, &child, &x, &y, &wx, &wy, &mask);
    Atom move = XInternAtom(state->display, "_NET_WM_MOVERESIZE", False);
    XEvent event = {0}; event.xclient.type = ClientMessage; event.xclient.window = state->window;
    event.xclient.message_type = move; event.xclient.format = 32;
    event.xclient.data.l[0] = x; event.xclient.data.l[1] = y;
    event.xclient.data.l[2] = 8; event.xclient.data.l[3] = 1;
    XSendEvent(state->display, root, False,
        SubstructureRedirectMask | SubstructureNotifyMask, &event); XFlush(state->display);
}
#endif
