#include "bongo_cat/platform.h"
#include "windows_startup.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "bongo_cat/log.h"

/* 开机自启(快捷方式 + 最高权限计划任务)搬到了 windows_autostart.c, 这里只留单实例、
 * 唤醒与更新交棒 —— 本文件于是不再需要 COM / Shell 的头文件。 */

/* 命名对象的句柄与其名字(默认值见下面, 可被测试环境变量改写)。这是进程级单例状态:
   只有 initialize_identity 会改名字, 其它函数一律通过 *._name() 取值, 于是"必须先
   初始化再使用"这条顺序要求集中在一个地方。 */
static HANDLE instance_mutex;
static HANDLE instance_wake_event;
static HANDLE instance_update_shutdown_event;
static HANDLE instance_stopped_event;
static HANDLE instance_info_mapping;
static BongoCatWindowsInstanceInfo *instance_info;
static wchar_t instance_title[96] = BONGO_CAT_PET_WINDOW_TITLE_W;
static wchar_t instance_mutex_name[128] = L"Local\\BongoCat.SingleInstance";
static wchar_t instance_wake_name[128] = L"Local\\BongoCat.WakeInstance";
static wchar_t instance_update_shutdown_name[128] =
    L"Local\\BongoCat.UpdateShutdown";
static wchar_t instance_stopped_name[128] = L"Local\\BongoCat.InstanceStopped";
static wchar_t instance_info_name[128] = L"Local\\BongoCat.InstanceInfo";
static bool identity_ready;

/* 测试用的实例标识只允许字母数字与 -_ 且不超过 32 字符: 它会被拼进内核对象名字,
   带反斜杠的名字会跑进别的命名空间(或者直接失败), 所以必须白名单过滤。 */
static bool safe_identity(const char *value) {
    if (!value || !value[0] || strlen(value) > 32) return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; ++cursor)
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_') return false;
    return true;
}

static void initialize_identity(void) {
    if (identity_ready) return;
    identity_ready = true;
    const char *value = SDL_getenv_unsafe("BONGO_CAT_TEST_INSTANCE_ID");
    if (!safe_identity(value)) return;
    swprintf(instance_title, sizeof(instance_title) / sizeof(instance_title[0]),
        L"%ls [%hs]", BONGO_CAT_PET_WINDOW_TITLE_W, value);
    swprintf(instance_mutex_name,
        sizeof(instance_mutex_name) / sizeof(instance_mutex_name[0]),
        L"Local\\BongoCat.SingleInstance.%hs", value);
    swprintf(instance_wake_name,
        sizeof(instance_wake_name) / sizeof(instance_wake_name[0]),
        L"Local\\BongoCat.WakeInstance.%hs", value);
    swprintf(instance_update_shutdown_name,
        sizeof(instance_update_shutdown_name) /
            sizeof(instance_update_shutdown_name[0]),
        L"Local\\BongoCat.UpdateShutdown.%hs", value);
    swprintf(instance_stopped_name,
        sizeof(instance_stopped_name) / sizeof(instance_stopped_name[0]),
        L"Local\\BongoCat.InstanceStopped.%hs", value);
    swprintf(instance_info_name,
        sizeof(instance_info_name) / sizeof(instance_info_name[0]),
        L"Local\\BongoCat.InstanceInfo.%hs", value);
}

const wchar_t *bongo_cat_windows_instance_title(void) {
    initialize_identity(); return instance_title;
}

/*
 * 身份窗口: 一个 0x0、永不显示、也从不抢焦点的顶层窗口, 标题就是 instance_title。
 *
 * 为什么需要它: "找到本实例"的代码 (更新交接的 FindWindowW(标题), 以及将来任何按标题找
 * 窗口的地方) 需要一个稳定标识; 而主窗口的标题现在**跟着模型走** —— 多桌宠时每个窗口都
 * 要能在 OBS 的窗口采集列表里区分开, 标题就是唯一的区分手段。把标识从主窗口挪到这个
 * 永不显示的窗口上, 两边都不再互相牵制。
 *
 * 类名按实例 id 区分: 同一台机器上跑多个实例 (测试/多开) 时, 各自的窗口标题与类名都不撞。
 */
static HWND identity_window;
static wchar_t identity_class[128] = L"BongoCatIdentity";

static LRESULT CALLBACK identity_window_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    /* 更新交接会对这个窗口 PostMessage(WM_CLOSE) 当"催一下", 真正让旧实例退出的还是事件
       对象; 这里交给默认处理即可 (销毁走 DestroyWindow)。 */
    return DefWindowProcW(window, message, wparam, lparam);
}

bool bongo_cat_windows_identity_window_create(void) {
    if (identity_window) return true;
    initialize_identity();
    wchar_t class_name[128];
    swprintf(class_name, sizeof(class_name) / sizeof(class_name[0]),
        L"BongoCatIdentity.%ls", instance_title);
    WNDCLASSEXW type = {0};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = identity_window_proc;
    type.hInstance = GetModuleHandleW(NULL);
    type.lpszClassName = class_name;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    identity_window = CreateWindowExW(0, class_name, instance_title, WS_OVERLAPPED,
        0, 0, 0, 0, NULL, NULL, type.hInstance, NULL);
    if (!identity_window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] 身份窗口创建失败, 更新交接将只能靠进程名匹配");
        return false;
    }
    return true;
}

void bongo_cat_windows_identity_window_destroy(void) {
    if (!identity_window) return;
    DestroyWindow(identity_window);
    identity_window = NULL;
}

const wchar_t *bongo_cat_windows_update_shutdown_name(void) {
    initialize_identity(); return instance_update_shutdown_name;
}

const wchar_t *bongo_cat_windows_instance_stopped_name(void) {
    initialize_identity(); return instance_stopped_name;
}

const wchar_t *bongo_cat_windows_instance_info_name(void) {
    initialize_identity(); return instance_info_name;
}

/*
 * 创建(或打开)全部命名对象。三个事件的语义不同, 不能互换:
 *   - wake: 自动重置, 只表示"有人敲了一下门";
 *   - update-shutdown: 自动重置, 只表示"请退出";
 *   - stopped: 手动重置, 是"我已经退出"的持久状态 —— 新实例必须在它置位后才敢接管,
 *     所以它绝不能是自动重置。
 * 共享内存里写 magic(用于识别结构已被初始化)、可执行文件路径与版本号。
 */
static void create_instance_events(void) {
    if (!instance_wake_event)
        instance_wake_event = CreateEventW(NULL, FALSE, FALSE, instance_wake_name);
    if (!instance_update_shutdown_event)
        instance_update_shutdown_event = CreateEventW(NULL, FALSE, FALSE,
            instance_update_shutdown_name);
    if (!instance_stopped_event)
        instance_stopped_event = CreateEventW(NULL, TRUE, FALSE,
            instance_stopped_name);
    if (!instance_info_mapping) {
        instance_info_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
            PAGE_READWRITE, 0, sizeof(BongoCatWindowsInstanceInfo),
            instance_info_name);
        if (instance_info_mapping)
            instance_info = MapViewOfFile(instance_info_mapping,
                FILE_MAP_WRITE, 0, 0, sizeof(BongoCatWindowsInstanceInfo));
        if (instance_info) {
            memset(instance_info, 0, sizeof(*instance_info));
            instance_info->magic = BONGO_CAT_WINDOWS_INSTANCE_MAGIC;
            GetModuleFileNameW(NULL, instance_info->executable_path,
                BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP);
            snprintf(instance_info->version, sizeof(instance_info->version),
                "%s", BONGO_CAT_VERSION);
        } else {
            /* 段被旧版本实例建得更小 (或权限不足) 时映射会失败: 单实例照样工作, 只是
                "唤醒原因"这条通道退回降级行为 (只把窗口唤到前台)。留一条日志, 否则
               "第二次启动没打开设置"在日志里完全看不出来。 */
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[startup] 共享内存映射失败, 唤醒原因不可用 (只支持唤到前台): %s",
                SDL_GetError());
        }
    }
}

/*
 * 把"这一次的唤醒原因"写进共享内存, 读侧是 bongo_cat_platform_single_instance_take_wake。
 *
 * 为什么需要它: 唤醒事件是自动重置的, 只能表达"有人敲了一下门", 带不了载荷 —— 而
 * "第二次启动 = 打开设置窗口"这个新语义必须让主实例知道。写载荷和敲门用的是同一对既有
 * 对象 (共享内存 + 事件), 不是第二套 IPC。
 *
 * 顺序必须是"先写原因, 再 SetEvent": 反过来的话主实例可能已经看到事件、读到的却还是旧值。
 * 打不开段 (旧版本实例、提权实例的安全描述符挡住了普通权限进程) 时不报错退出 —— 唤醒照发,
 * 只是退化成"把窗口唤到前台"。
 */
static void publish_wake_request(uint32_t request) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, instance_info_name);
    if (!mapping) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[startup] 打不开实例共享内存, 只能把已有实例唤到前台 (request=%u)",
            (unsigned)request);
        return;
    }
    BongoCatWindowsInstanceInfo *info = MapViewOfFile(mapping, FILE_MAP_WRITE,
        0, 0, sizeof(*info));
    if (info) {
        if (info->magic == BONGO_CAT_WINDOWS_INSTANCE_MAGIC)
            info->wake_request = request;
        UnmapViewOfFile(info);
    }
    CloseHandle(mapping);
}

/*
 * 通知已经运行的实例"又有人启动了一次": 先试事件(最快), 事件打不开就退化成按标题
 * 找到窗口并直接激活它 —— 走退化的那条路时唤醒原因送不过去, 主实例只会把窗口唤到前台。
 * 重试 30 次 × 100ms: 旧实例可能还在启动过程中, 窗口和事件都没建好; 这时立刻放弃会
 * 让用户点了图标却什么都没发生。
 */
static void wake_existing_instance(void) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        HANDLE wake = OpenEventW(EVENT_MODIFY_STATE, FALSE, instance_wake_name);
        if (wake) {
            publish_wake_request((uint32_t)BONGO_CAT_WAKE_OPEN_PREFERENCES);
            bool signaled = SetEvent(wake) != FALSE;
            CloseHandle(wake);
            if (signaled) {
                SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                    "[startup] 已通知运行中的实例打开设置窗口, 本进程退出");
                return;
            }
        }
        HWND existing = FindWindowW(NULL, instance_title);
        if (existing) {
            ShowWindowAsync(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing);
            return;
        }
        Sleep(100);
    }
}

/*
 * 单实例裁决。返回 true = 本进程继续运行(自己拿到了互斥体, 或者被测试环境变量放行);
 * 返回 false = 已经有实例在跑, 调用方应当退出。
 * 流程: 建互斥体 -> 已存在就先试"更新交棒"(让旧实例为更新让位) -> 等它真正退出后
 * 重新拿互斥体 -> 还是拿不到就只把旧实例唤到前台, 然后返回 false。
 * 这里每一步都要关掉再重新 CreateMutexW: 互斥体是"持有"语义, 旧实例退出后必须重新
 * 创建才能取得所有权。
 */
bool bongo_cat_platform_single_instance_begin(void) {
    initialize_identity();
    if (SDL_getenv_unsafe("BONGO_CAT_ALLOW_TEST_INSTANCES")) return true;
    instance_mutex = CreateMutexW(NULL, FALSE, instance_mutex_name);
    if (!instance_mutex) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        create_instance_events(); return true;
    }
    CloseHandle(instance_mutex); instance_mutex = NULL;
    if (bongo_cat_windows_update_handoff()) {
        for (int attempt = 0; attempt < 30; ++attempt) {
            instance_mutex = CreateMutexW(NULL, FALSE, instance_mutex_name);
            if (instance_mutex && GetLastError() != ERROR_ALREADY_EXISTS) {
                create_instance_events(); return true;
            }
            if (instance_mutex) CloseHandle(instance_mutex);
            instance_mutex = NULL;
            Sleep(100);
        }
    }
    wake_existing_instance();
    instance_mutex = CreateMutexW(NULL, FALSE, instance_mutex_name);
    if (instance_mutex && GetLastError() != ERROR_ALREADY_EXISTS) {
        create_instance_events(); return true;
    }
    if (instance_mutex) CloseHandle(instance_mutex);
    instance_mutex = NULL;
    return false;
}

/* 非阻塞地取一次唤醒请求: 主循环每轮调用。原因写在共享内存里, 这里读一次并立刻清零
   —— 不清的话下一次"只唤醒"的敲门会被当成又一次"打开设置"。读不到原因 (映射失败或
   magic 不对) 就按旧的"唤到前台"处理。 */
BongoCatWakeRequest bongo_cat_platform_single_instance_take_wake(void) {
    if (!instance_wake_event ||
        WaitForSingleObject(instance_wake_event, 0) != WAIT_OBJECT_0)
        return BONGO_CAT_WAKE_NONE;
    uint32_t request = (uint32_t)BONGO_CAT_WAKE_REVEAL;
    if (instance_info && instance_info->magic == BONGO_CAT_WINDOWS_INSTANCE_MAGIC) {
        request = instance_info->wake_request;
        instance_info->wake_request = (uint32_t)BONGO_CAT_WAKE_NONE;
    }
    if (request != (uint32_t)BONGO_CAT_WAKE_OPEN_PREFERENCES &&
        request != (uint32_t)BONGO_CAT_WAKE_REVEAL)
        request = (uint32_t)BONGO_CAT_WAKE_REVEAL;
    return (BongoCatWakeRequest)request;
}

/*
 * 更新流程里"新进程"的入口: 命令行带 --shutdown-for-update 时, 负责把旧实例请出去并
 * 等它关完。
 * 三条通知同时发: 事件(给正常运行的实例)、WM_CLOSE(给卡在消息循环里的窗口), 最后等
 * stopped 事件最多 15 秒 —— 即使超时也照样返回 true, 因为更新已经进入下一步, 卡在
 * 这里没有意义。
 * 返回 true 表示"本进程只负责关掉旧实例, 关完就该退出"。
 */
bool bongo_cat_platform_update_shutdown_argument(int argc, char **argv) {
    bool requested = false;
    for (int i = 1; i < argc; ++i)
        if (argv && argv[i] && strcmp(argv[i], "--shutdown-for-update") == 0)
            requested = true;
    if (!requested) return false;
    initialize_identity();
    HANDLE stopped = OpenEventW(SYNCHRONIZE, FALSE, instance_stopped_name);
    HANDLE shutdown = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        instance_update_shutdown_name);
    if (shutdown) {
        SetEvent(shutdown);
        CloseHandle(shutdown);
    }
    HWND existing = FindWindowW(NULL, instance_title);
    if (existing) PostMessageW(existing, WM_CLOSE, 0, 0);
    if (stopped) {
        WaitForSingleObject(stopped, 15000);
        CloseHandle(stopped);
    }
    return true;
}

/* 非阻塞地检查"是否有更新流程要求我退出": 主循环每轮调用, 返回 true 就应当优雅退出。 */
bool bongo_cat_platform_single_instance_take_update_shutdown(void) {
    return instance_update_shutdown_event &&
        WaitForSingleObject(instance_update_shutdown_event, 0) == WAIT_OBJECT_0;
}

/* 退出清理。顺序很重要: 先置 stopped(让正在等待的新实例知道可以接管了), 再逐个关闭
   句柄。即使某个句柄本来就是 NULL 也要走到最后 —— 单实例资源泄漏会让下次启动被误判
   成"已有实例在运行"。 */
void bongo_cat_platform_single_instance_end(void) {
    if (instance_stopped_event) SetEvent(instance_stopped_event);
    if (instance_update_shutdown_event)
        CloseHandle(instance_update_shutdown_event);
    instance_update_shutdown_event = NULL;
    if (instance_stopped_event) CloseHandle(instance_stopped_event);
    instance_stopped_event = NULL;
    if (instance_info) UnmapViewOfFile(instance_info);
    instance_info = NULL;
    if (instance_info_mapping) CloseHandle(instance_info_mapping);
    instance_info_mapping = NULL;
    if (instance_wake_event) CloseHandle(instance_wake_event);
    instance_wake_event = NULL;
    if (instance_mutex) CloseHandle(instance_mutex);
    instance_mutex = NULL;
}
#endif
