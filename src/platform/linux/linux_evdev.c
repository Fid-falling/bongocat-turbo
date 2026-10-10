#include "linux_evdev_internal.h"
#include "bongo_cat/log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/*
 * evdev 输入后端: 直接读 /dev/input/event* 的内核事件流。
 *
 * 为什么需要: Wayland 合成器不会把别的应用的按键转发给 X11 客户端, 所以 XInput2 在
 * Wayland 下看不到全局按键(见 linux_internal.h)。
 * 代价: 需要能读 /dev/input/*(通常要加入 input 组), 因此属于实验性路径 —— 默认只在
 * Wayland 下、且显式设置 BONGOCAT_ENABLE_EVDEV=1 时才启用。默认关闭是因为没权限时
 * 会刷一堆失败日志, 而用户往往不知道该怎么授权。
 */
bool bongo_cat_linux_evdev_requested(const char *option, bool wayland) {
    return wayland && option && strcmp(option, "1") == 0;
}

/* 从平台状态里取 evdev 子状态; 没选这个后端时返回 NULL, 于是所有对外函数都可以安全
   调用而不必各自判空。 */
static LinuxEvdevState *evdev_state(const BongoCatPlatform *platform) {
    const LinuxPlatformState *native = platform ? platform->native : NULL;
    return native ? native->evdev : NULL;
}

/*
 * 接收线程主体: epoll 同时等所有设备 fd, 超时用来做两件事 —— 定期重扫设备
 * (EVDEV_SCAN_INTERVAL_NS, 处理热插拔, 不依赖 inotify)和检查退出标志。
 * EPOLLHUP / EPOLLERR 或读失败都当作设备消失, 从表里摘掉; 设备表是紧凑数组, 摘除会
 * 移动后面的元素, 所以每轮都要按 fd 重新查一遍下标。
 * 退出时先清空设备再复位 running, 保证 stop 的等待者看到的是完全停下来的状态。
 */
static int SDLCALL evdev_thread(void *userdata) {
    LinuxEvdevState *state = userdata;
    bongo_cat_evdev_scan(state);
    struct epoll_event events[EVDEV_MAX_DEVICES];
    uint64_t next_scan = SDL_GetTicksNS() + EVDEV_SCAN_INTERVAL_NS;
    while (atomic_load(&state->running)) {
        int ready = epoll_wait(state->epoll_fd, events, EVDEV_MAX_DEVICES,
            EVDEV_POLL_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "evdev input wait failed");
            break;
        }
        for (int i = 0; i < ready; ++i) {
            size_t index = 0;
            for (; index < state->device_count; ++index)
                if (state->devices[index].fd == events[i].data.fd) break;
            if (index == state->device_count) continue;
            if ((events[i].events & (EPOLLHUP | EPOLLERR)) ||
                !bongo_cat_evdev_read(state, &state->devices[index]))
                bongo_cat_evdev_remove(state, index);
        }
        uint64_t now = SDL_GetTicksNS();
        if (now >= next_scan) {
            next_scan = now + EVDEV_SCAN_INTERVAL_NS;
            bongo_cat_evdev_scan(state);
        }
    }
    while (state->device_count) bongo_cat_evdev_remove(state, 0);
    atomic_store(&state->running, false);
    return 0;
}

/* 启动: 三个前提(选中了 evdev、平台状态存在、输入队列存在)。已经启动过就直接返回 true
   (幂等)。epoll 用 CLOEXEC —— 我们后面会用 fork/exec 打开目录, 不能让这些 fd 漏给
   子进程。reported_count 初始化为 SIZE_MAX, 让第一次扫描一定会打印"发现了几个设备"。 */
bool bongo_cat_linux_evdev_start(BongoCatPlatform *platform, BongoCatError *error) {
    LinuxPlatformState *native = platform ? platform->native : NULL;
    if (!native || !native->evdev_selected || !platform->input) return false;
    if (native->evdev) return true;
    LinuxEvdevState *state = calloc(1, sizeof(*state));
    if (!state) goto failed;
    state->platform = platform;
    state->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    state->motion_lock = SDL_CreateMutex();
    atomic_init(&state->running, true);
    atomic_init(&state->pointer_active, false);
    state->reported_count = SIZE_MAX;
    if (state->epoll_fd < 0 || !state->motion_lock) goto failed;
    state->thread = SDL_CreateThread(evdev_thread, BONGO_CAT_SLUG "-evdev-input", state);
    if (!state->thread) goto failed;
    native->evdev = state;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[runtime] experimental evdev input enabled");
    return true;
failed:
    if (state) {
        if (state->epoll_fd >= 0) close(state->epoll_fd);
        SDL_DestroyMutex(state->motion_lock);
        free(state);
    }
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Cannot start evdev input listener");
    return false;
}

/* 停止: 先清 running 标志再等线程退出(线程自己会清理设备表), 然后才释放 epoll / mutex /
   内存, 最后清空平台的 evdev 指针。顺序反了会让线程访问已释放的状态。 */
void bongo_cat_linux_evdev_stop(BongoCatPlatform *platform) {
    LinuxEvdevState *state = evdev_state(platform);
    if (!state) return;
    atomic_store(&state->running, false);
    SDL_WaitThread(state->thread, NULL);
    close(state->epoll_fd);
    SDL_DestroyMutex(state->motion_lock);
    free(state);
    ((LinuxPlatformState *)platform->native)->evdev = NULL;
}

bool bongo_cat_linux_evdev_pointer_active(const BongoCatPlatform *platform) {
    LinuxEvdevState *state = evdev_state(platform);
    return state && atomic_load(&state->pointer_active);
}

/* 取走累积的相对位移(取走即清零)。返回值表示"这一次是否真的有位移" —— 返回 false 时
   dx/dy 已经被写成 0, 调用方可以无条件使用它们。 */
bool bongo_cat_linux_evdev_relative_pointer(BongoCatPlatform *platform,
    double *dx, double *dy) {
    LinuxEvdevState *state = evdev_state(platform);
    if (!state || !dx || !dy) return false;
    SDL_LockMutex(state->motion_lock);
    *dx = state->relative_x;
    *dy = state->relative_y;
    state->relative_x = state->relative_y = 0;
    SDL_UnlockMutex(state->motion_lock);
    return *dx != 0 || *dy != 0;
}

void bongo_cat_linux_evdev_relative_pointer_reset(BongoCatPlatform *platform) {
    LinuxEvdevState *state = evdev_state(platform);
    if (state) bongo_cat_evdev_motion_reset(state);
}
