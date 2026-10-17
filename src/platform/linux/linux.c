#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "bongo_cat/platform.h"
#include "bongo_cat/common.h"
#include "bongo_cat/path.h"
#include "linux_internal.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <X11/Xlib.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Linux 平台层的装配点(X11 与 Wayland 共用同一份代码)。
 *
 * 两条输入路径并存: X11(XInput2)负责窗口内的键鼠与悬停, evdev 负责 Wayland 下拿不到的
 * 全局输入(见 linux_internal.h)。单实例用一个带 flock 的锁文件, 锁文件里顺手记下主窗口
 * 的 X11 id, 好让第二个实例把第一个唤到前台。
 * active_platform 是给"按窗口找平台"的回调(raise_window / dynamic_hit_supported)用的:
 * 平台层只有一份实例, 但那些接口签名只给出了 SDL_Window。
 */
static int instance_lock = -1;
static BongoCatPlatform *active_platform;

void bongo_cat_platform_configure_preferences_window(SDL_Window *window) {
    (void)window;
}

/* 把主窗口的 X11 id 写进锁文件(并 fsync): 第二个实例靠它找到第一个的窗口。
   写之前必须 ftruncate, 否则新的短内容会被旧内容的尾巴污染。 */
static void publish_instance_window(SDL_Window *window) {
    if (instance_lock < 0 || !window) return;
    Window id = (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!id) return;
    char text[32]; int length = snprintf(text, sizeof(text), "%lu\n", (unsigned long)id);
    if (length <= 0 || (size_t)length >= sizeof(text)) return;
    if (ftruncate(instance_lock, 0) == 0 && lseek(instance_lock, 0, SEEK_SET) == 0) {
        if (write(instance_lock, text, (size_t)length) == (ssize_t)length) fsync(instance_lock);
    }
}

/* 把第一个实例的窗口抬到前台: 先 XMapRaised, 再按 EWMH 约定向根窗口发一条
   _NET_ACTIVE_WINDOW(很多窗口管理器会忽略不带这条消息的激活请求; data.l[0] = 2 表示
   "来自用户或 pager 的操作")。这里临时开一个自己的 Display 连接, 用完即关。 */
static void restore_instance_window(void) {
    if (instance_lock < 0) return;
    char text[32] = {0}; lseek(instance_lock, 0, SEEK_SET);
    ssize_t length = read(instance_lock, text, sizeof(text) - 1);
    Window window = length > 0 ? (Window)strtoul(text, NULL, 10) : 0;
    Display *display = window ? XOpenDisplay(NULL) : NULL;
    if (!display) return;
    XMapRaised(display, window);
    Atom active = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    XEvent event = {0}; event.xclient.type = ClientMessage;
    event.xclient.window = window; event.xclient.message_type = active;
    event.xclient.format = 32; event.xclient.data.l[0] = 2;
    XSendEvent(display, DefaultRootWindow(display), False,
        SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display); XCloseDisplay(display);
}

/* 取自身可执行文件路径。AppImage 必须先看 $APPIMAGE: 用户执行的其实是挂载出来的临时
   目录里的可执行文件, 直接读 /proc/self/exe 会把自动启动项指向那个临时路径。 */
static bool executable_path(char output[BONGO_CAT_PATH_CAP]) {
    const char *appimage = getenv("APPIMAGE");
    if (appimage && appimage[0]) {
        int length = snprintf(output, BONGO_CAT_PATH_CAP, "%s", appimage);
        return length >= 0 && length < BONGO_CAT_PATH_CAP;
    }
    ssize_t length = readlink("/proc/self/exe", output, BONGO_CAT_PATH_CAP - 1);
    if (length <= 0 || length >= BONGO_CAT_PATH_CAP) return false;
    output[length] = '\0'; return true;
}

/*
 * 平台初始化。要点:
 *   - hover_hide_unavailable: 悬停隐藏依赖 X11 的指针事件, Wayland 下拿不到, 直接标记
 *     为不可用, 让上层隐藏这个选项;
 *   - wayland 的判定同时看 WAYLAND_DISPLAY、当前视频驱动和 XDG_SESSION_TYPE(单个都
 *     不完全可靠, 取并集);
 *   - X11 与 evdev 两个后端都允许失败, 失败只记警告: 没有全局输入时程序仍然能跑。
 */
BongoCatResult bongo_cat_platform_init(BongoCatPlatform *platform, SDL_Window *window,
    BongoCatInputState *input, BongoCatError *error) {
    memset(platform, 0, sizeof(*platform));
    platform->window = window;
    platform->input = input;
    platform->window_opacity = 1.0f;
    const char *driver = SDL_GetCurrentVideoDriver();
    platform->hover_hide_unavailable = !driver || strcmp(driver, "x11") != 0;
    if (platform->hover_hide_unavailable)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hover hiding requires the X11 backend on Linux");
    platform->wake_event_type = SDL_RegisterEvents(1);
    if (platform->wake_event_type == (Uint32)-1) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot reserve the Linux input wake event");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    LinuxPlatformState *native = calloc(1, sizeof(*native));
    if (!native) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot allocate Linux platform state");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    platform->native = native;
    active_platform = platform;
    publish_instance_window(window);
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *session = getenv("XDG_SESSION_TYPE");
    native->wayland = (wayland && wayland[0]) ||
        (driver && strcmp(driver, "wayland") == 0) ||
        (session && strcmp(session, "wayland") == 0);
    native->evdev_selected = bongo_cat_linux_evdev_requested(
        getenv("BONGOCAT_ENABLE_EVDEV"), native->wayland);
    BongoCatError input_error = {0};
    if (!bongo_cat_linux_x11_start(platform, &input_error) && input_error.message[0])
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", input_error.message);
    if (native->evdev_selected) {
        input_error = (BongoCatError){0};
        if (!bongo_cat_linux_evdev_start(platform, &input_error) && input_error.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", input_error.message);
    }
    return BONGO_CAT_OK;
}
/* 关闭顺序与 init 相反; 两个后端都无条件 stop(它们自己判断有没有启动过), 最后释放
   native 并清掉 active_platform, 免得留下悬空指针。 */
void bongo_cat_platform_shutdown(BongoCatPlatform *platform) {
    if (!platform) return;
    bongo_cat_linux_x11_stop(platform);
    bongo_cat_linux_evdev_stop(platform);
    free(platform->native);
    platform->native = NULL;
    if (active_platform == platform) active_platform = NULL;
}
/* Linux 只有"整窗穿透"一种强度(靠 XShape 的空输入区域), 无法逐像素判断, 所以 forced
   与 pointer_transparent 在这里合并处理。 */
void bongo_cat_platform_set_click_through(BongoCatPlatform *platform,
    bool forced, bool pointer_transparent) {
    bongo_cat_linux_x11_click_through(platform, forced || pointer_transparent);
}
/* Linux 没有 Windows 那套代理分层窗口: GL 的逐像素透明由合成器负责, 所以"透明背景"
   这一项在这里是 no-op(设置界面也只在 Windows 上显示它)。保留空实现是为了让上层的
   下发入口可以无条件调用, 不必到处条件编译。 */
void bongo_cat_platform_apply_transparency(BongoCatPlatform *platform) {
    (void)platform;
}
bool bongo_cat_platform_set_opacity(BongoCatPlatform *platform, float opacity) {
    if (!platform || !platform->window) return false;
    opacity = SDL_clamp(opacity, 0.0f, 1.0f);
    if (opacity == platform->window_opacity) return true;
    if (!SDL_SetWindowOpacity(platform->window, opacity)) return false;
    platform->window_opacity = opacity;
    return true;
}
float bongo_cat_platform_get_opacity(const BongoCatPlatform *platform) {
    return platform ? platform->window_opacity : 1.0f;
}
bool bongo_cat_platform_present(BongoCatPlatform *platform, int width, int height) {
    (void)width; (void)height;
    return platform && platform->window && SDL_GL_SwapWindow(platform->window);
}
bool bongo_cat_platform_frame_alpha(const BongoCatPlatform *platform,
    int width, int height, int x, int y, uint8_t *alpha) {
    (void)platform; (void)width; (void)height; (void)x; (void)y; (void)alpha;
    return false;
}
bool bongo_cat_platform_layered_active(BongoCatPlatform *platform) {
    (void)platform; return false;
}
bool bongo_cat_platform_submit_layered_rgba(BongoCatPlatform *platform,
    int width, int height, const unsigned char *rgba) {
    (void)platform; (void)width; (void)height; (void)rgba;
    return false;
}
void bongo_cat_platform_set_visible(BongoCatPlatform *platform, bool visible) {
    if (!platform || !platform->window) return;
    visible ? SDL_ShowWindow(platform->window) : SDL_HideWindow(platform->window);
    if (visible) {
        bongo_cat_linux_x11_configure_capture_window(platform);
        const LinuxPlatformState *native = platform->native;
        if (native) bongo_cat_linux_x11_set_above(platform, native->always_on_top);
    }
}
/* 只在采集软件里显示: X11 下没有等价机制 (需要合成器/虚拟显示器支持),
   设置界面会因此隐藏这一项。 */
bool bongo_cat_platform_capture_only_supported(void) { return false; }
bool bongo_cat_platform_set_capture_only(BongoCatPlatform *platform,
    bool enabled) {
    (void)platform; (void)enabled;
    return false;
}
bool bongo_cat_platform_pointer_local(BongoCatPlatform *platform, double screen_x,
    double screen_y, float *local_x, float *local_y) {
    int x, y, width, height;
    if (!platform || !local_x || !local_y ||
        !SDL_GetWindowPosition(platform->window, &x, &y) ||
        !SDL_GetWindowSize(platform->window, &width, &height)) return false;
    *local_x = (float)(screen_x - x); *local_y = (float)(screen_y - y);
    return *local_x >= 0 && *local_x < width && *local_y >= 0 && *local_y < height;
}
bool bongo_cat_platform_pointer_locked(BongoCatPlatform *platform) {
    return bongo_cat_linux_evdev_pointer_active(platform);
}
bool bongo_cat_platform_relative_pointer(BongoCatPlatform *platform,
    double *x, double *y) {
    return bongo_cat_linux_evdev_relative_pointer(platform, x, y);
}
void bongo_cat_platform_relative_pointer_reset(BongoCatPlatform *platform) {
    bongo_cat_linux_evdev_relative_pointer_reset(platform);
}
void bongo_cat_platform_relative_pointer_release(BongoCatPlatform *platform) {
    bongo_cat_linux_evdev_relative_pointer_reset(platform);
}
void bongo_cat_platform_set_always_on_top(BongoCatPlatform *platform, bool enabled) {
    if (!platform || !platform->window) return;
    LinuxPlatformState *native = platform->native;
    if (native) native->always_on_top = enabled;
    SDL_SetWindowAlwaysOnTop(platform->window, enabled);
    bongo_cat_linux_x11_set_above(platform, enabled);
    bongo_cat_linux_x11_configure_capture_window(platform);
}
void bongo_cat_platform_raise_window(SDL_Window *window) {
    if (!window) return;
    if (active_platform && active_platform->window == window)
        bongo_cat_platform_set_visible(active_platform, true);
    else SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
}

bool bongo_cat_platform_set_geometry(BongoCatPlatform *platform,
    int x, int y, int width, int height) {
    if (!platform || !platform->window) return false;
    int current_width, current_height;
    if ((!SDL_GetWindowSize(platform->window, &current_width, &current_height) ||
        current_width != width || current_height != height) &&
        !SDL_SetWindowSize(platform->window, width, height)) return false;
    int current_x, current_y;
    if (!SDL_GetWindowPosition(platform->window, &current_x, &current_y) ||
        current_x != x || current_y != y)
        SDL_SetWindowPosition(platform->window, x, y);
    return true;
}
void bongo_cat_platform_begin_drag(BongoCatPlatform *platform,
    BongoCatModalTick modal_tick, void *userdata) {
    (void)modal_tick; (void)userdata;
    bongo_cat_linux_x11_begin_drag(platform);
}
/* 原因见下面原有的英文注释: XWayland 只在某个 surface 有指针焦点时才转发指针事件, 把
   输入区域清空会让指针彻底消失、猫再也点不回来, 所以 XWayland 下不支持逐像素命中。 */
bool bongo_cat_platform_dynamic_hit_supported(void) {
    /* XWayland only forwards pointer events while one of its surfaces has
       pointer focus, so an empty input region would hide the pointer
       permanently and the pet could never become clickable again. */
    return bongo_cat_linux_x11_supported(active_platform) &&
        !bongo_cat_linux_x11_xwayland(active_platform);
}

/* 用 xdg-open 打开目录。double fork + setsid 是为了让 xdg-open 彻底脱离我们的进程组:
   否则它会一直挂在我们名下, 我们退出时可能把它一起带走, 或者被它继承的 fd 拖住。 */
bool bongo_cat_platform_open_directory(const char *path) {
    if (!path || !path[0]) return false;
    pid_t launcher = fork();
    if (launcher < 0) return false;
    if (launcher == 0) {
        pid_t opener = fork();
        if (opener < 0) _exit(127);
        if (opener > 0) _exit(0);
        setsid();
        execlp("xdg-open", "xdg-open", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    return waitpid(launcher, &status, 0) == launcher &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void bongo_cat_platform_set_tray_callbacks(void *tray,
    BongoCatTrayClick left_click, BongoCatModalTick modal_tick,
    BongoCatTrayRestore restore, void *userdata) {
    (void)tray; (void)left_click; (void)modal_tick;
    (void)restore; (void)userdata;
}
/* 单实例靠 /tmp 下的锁文件 + flock(LOCK_NB): 内核在进程退出时自动释放, 不会留下需要
   清理的陈旧锁 —— 这也正是用 flock 而不是写 pid 文件的原因。拿不到锁就说明已有实例,
   把它的窗口抬到前台后返回 false。 */
bool bongo_cat_platform_single_instance_begin(void) {
    char path[96]; snprintf(path, sizeof(path), "/tmp/%s-%lu.lock",
        BONGO_CAT_SLUG,
        (unsigned long)getuid());
    instance_lock = open(path, O_CREAT | O_RDWR, 0600);
    if (instance_lock < 0 || flock(instance_lock, LOCK_EX | LOCK_NB) == 0) return true;
    restore_instance_window(); close(instance_lock); instance_lock = -1; return false;
}
/* 唤醒原因只跟着"打开设置窗口"这条新路径走, Windows 才有; 这里保持无请求 (Linux 的
   第二次启动仍然只把窗口抬到前台, 见 restore_instance_window)。 */
BongoCatWakeRequest bongo_cat_platform_single_instance_take_wake(void) {
    return BONGO_CAT_WAKE_NONE;
}
void bongo_cat_platform_single_instance_end(void) {
    if (instance_lock >= 0) close(instance_lock);
    instance_lock = -1;
}
/* 开机自启 = 在 $XDG_CONFIG_HOME/autostart(默认 ~/.config/autostart)下写一个 .desktop
   文件, 这是 XDG 桌面环境的标准做法。Exec 里的路径用引号包住, 所以路径含双引号时必须
   拒绝 —— 否则能拼出一条非法甚至有害的 Exec 行。写失败要删掉半成品, 免得留下坏条目。 */
BongoCatResult bongo_cat_platform_set_autostart(bool enabled, bool elevated, BongoCatError *error) {
    (void)elevated;
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char config[BONGO_CAT_PATH_CAP], directory[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
    if (base && base[0]) snprintf(config, sizeof(config), "%s", base);
    else if (home && bongo_cat_path_join(config, sizeof(config), home, ".config")) {}
    else return BONGO_CAT_ERROR_PLATFORM;
    if (!bongo_cat_path_join(directory, sizeof(directory), config, "autostart") ||
        !bongo_cat_path_join(path, sizeof(path), directory, BONGO_CAT_SLUG ".desktop"))
        return BONGO_CAT_ERROR_IO;
    if (!enabled) {
        if (remove(path) == 0 || errno == ENOENT) return BONGO_CAT_OK;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot remove Linux autostart entry");
        return BONGO_CAT_ERROR_IO;
    }
    char executable[BONGO_CAT_PATH_CAP];
    if (!executable_path(executable) || strchr(executable, '"') ||
        !bongo_cat_path_create_directory(directory)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot prepare Linux autostart entry");
        return BONGO_CAT_ERROR_IO;
    }
    FILE *file = fopen(path, "wb");
    if (!file) return BONGO_CAT_ERROR_IO;
    bool written = fprintf(file, "[Desktop Entry]\nType=Application\nName=BongoCat\n"
        "Exec=\"%s\" --autostart\nTerminal=false\nX-GNOME-Autostart-enabled=true\n", executable) > 0;
    if (fclose(file) != 0) written = false;
    if (written) return BONGO_CAT_OK;
    remove(path); bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot write Linux autostart entry");
    return BONGO_CAT_ERROR_IO;
}
BongoCatResult bongo_cat_platform_autostart_enforce_elevated(void) {
    /* Linux 以套件提权(不会像 Windows 那样换个用户令牌), 正常会话即可写自启。 */
    return BONGO_CAT_OK;
}
/* Linux 侧没有内嵌资源(资源随包放在磁盘上), 所以恒定失败, 由调用方走普通目录逻辑。 */
BongoCatResult bongo_cat_platform_embedded_assets(const char *target, BongoCatError *error) {
    (void)target; (void)error; return BONGO_CAT_ERROR_PLATFORM;
}
/* 同理: 没有内嵌资源就没有内嵌组件。Linux 发行包把 cores/libbongo_live2d.so 直接放在
   磁盘上, 由既有的目录解析发现, 不需要"释放"这一步。 */
BongoCatResult bongo_cat_platform_embedded_assets_release_component(
    const char *directory, BongoCatError *error) {
    (void)directory; (void)error; return BONGO_CAT_ERROR_PLATFORM;
}
#endif
