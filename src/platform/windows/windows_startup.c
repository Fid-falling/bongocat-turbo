#include "bongo_cat/platform.h"
#include "windows_startup.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <ctype.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "bongo_cat/log.h"

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
            instance_info->magic = 0x42434D49u;
            GetModuleFileNameW(NULL, instance_info->executable_path,
                BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP);
            snprintf(instance_info->version, sizeof(instance_info->version),
                "%s", BONGO_CAT_VERSION);
        }
    }
}

/*
 * 通知已经运行的实例"又有人启动了一次": 先试事件(最快), 事件打不开就退化成按标题
 * 找到窗口并直接激活它。
 * 重试 30 次 × 100ms: 旧实例可能还在启动过程中, 窗口和事件都没建好; 这时立刻放弃会
 * 让用户点了图标却什么都没发生。
 */
static void wake_existing_instance(void) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        HANDLE wake = OpenEventW(EVENT_MODIFY_STATE, FALSE, instance_wake_name);
        if (wake) {
            bool signaled = SetEvent(wake) != FALSE;
            CloseHandle(wake);
            if (signaled) return;
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

/* 非阻塞地取一次唤醒信号: 主循环每轮调用, 返回 true 就把窗口显示出来。 */
bool bongo_cat_platform_single_instance_take_wake(void) {
    return instance_wake_event &&
        WaitForSingleObject(instance_wake_event, 0) == WAIT_OBJECT_0;
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

/*
 * 开机自启: 在用户的"启动"文件夹里放 / 删一个快捷方式(而不是写 Run 注册表项)。
 * 为什么用快捷方式: 更新后程序路径可能变化, 而且用户能在"启动"文件夹里看得见、自己
 * 管理; 快捷方式带 --autostart 参数, 程序据此知道这次是开机自启而不是用户双击。
 * 关闭时 ERROR_FILE_NOT_FOUND 也算成功 —— 目标状态(没有快捷方式)已经达成。
 */
static BongoCatResult create_startup_shortcut(BongoCatError *error) {
    PWSTR startup = NULL;
    HRESULT result = SHGetKnownFolderPath(&FOLDERID_Startup, KF_FLAG_DEFAULT,
        NULL, &startup);
    wchar_t shortcut[BONGO_CAT_PATH_CAP];
    int written = SUCCEEDED(result) && startup
        ? swprintf(shortcut, BONGO_CAT_PATH_CAP, L"%ls\\%ls.lnk", startup,
            BONGO_CAT_STARTUP_SHORTCUT_W) : -1;
    CoTaskMemFree(startup);
    if (FAILED(result) || written < 0 || written >= BONGO_CAT_PATH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot locate the Windows Startup folder");
        return BONGO_CAT_ERROR_PLATFORM;
    }

    wchar_t executable[BONGO_CAT_PATH_CAP];
    DWORD length = GetModuleFileNameW(NULL, executable, BONGO_CAT_PATH_CAP);
    if (!length || length >= BONGO_CAT_PATH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot determine executable path");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    wchar_t working_directory[BONGO_CAT_PATH_CAP];
    memcpy(working_directory, executable, (length + 1) * sizeof(wchar_t));
    wchar_t *separator = wcsrchr(working_directory, L'\\');
    if (separator) *separator = L'\0';

    /* IShellLink 需要 COM。RPC_E_CHANGED_MODE 表示本线程已经用别的套间模式初始化过,
       这时仍然可以创建对象, 但不该由我们 CoUninitialize。 */
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) result = initialized;
    else {
        IShellLinkW *link = NULL;
        IPersistFile *persist = NULL;
        result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
            &IID_IShellLinkW, (void **)&link);
        if (SUCCEEDED(result)) result = link->lpVtbl->SetPath(link, executable);
        if (SUCCEEDED(result)) result = link->lpVtbl->SetArguments(link,
            L"--autostart");
        if (SUCCEEDED(result) && separator)
            result = link->lpVtbl->SetWorkingDirectory(link, working_directory);
        if (SUCCEEDED(result)) result = link->lpVtbl->SetDescription(link,
            L"BongoCat");
        if (SUCCEEDED(result)) result = link->lpVtbl->QueryInterface(link,
            &IID_IPersistFile, (void **)&persist);
        if (SUCCEEDED(result)) result = persist->lpVtbl->Save(persist,
            shortcut, TRUE);
        if (persist) persist->lpVtbl->Release(persist);
        if (link) link->lpVtbl->Release(link);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (SUCCEEDED(result)) return BONGO_CAT_OK;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "Cannot create the Windows Startup shortcut (0x%08lx)",
        (unsigned long)result);
    return BONGO_CAT_ERROR_PLATFORM;
}

static BongoCatResult remove_startup_shortcut(BongoCatError *error) {
    PWSTR startup = NULL;
    HRESULT result = SHGetKnownFolderPath(&FOLDERID_Startup, KF_FLAG_DEFAULT,
        NULL, &startup);
    wchar_t shortcut[BONGO_CAT_PATH_CAP];
    int written = SUCCEEDED(result) && startup
        ? swprintf(shortcut, BONGO_CAT_PATH_CAP, L"%ls\\%ls.lnk", startup,
            BONGO_CAT_STARTUP_SHORTCUT_W) : -1;
    CoTaskMemFree(startup);
    if (FAILED(result) || written < 0 || written >= BONGO_CAT_PATH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot locate the Windows Startup folder");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    if (DeleteFileW(shortcut) || GetLastError() == ERROR_FILE_NOT_FOUND)
        return BONGO_CAT_OK;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "Cannot remove the Windows Startup shortcut");
    return BONGO_CAT_ERROR_PLATFORM;
}

/* 计划任务的自启对象名, 与快捷方式名共用同一来源, 不会分叉(见 common.h)。 */
#define BONGO_CAT_STARTUP_TASK_W BONGO_CAT_STARTUP_NAME_W

/* 用 schtasks 注册/删除"最高权限登录自启"任务。
   prompt=true: 当前不是管理员时就经 ShellExecuteExW(runas) 弹一次 UAC;
   prompt=false: 一律用 CreateProcessW 直接跑(沿用当前令牌)。调用方已进提权会话时传
   false 走静默路径 —— 这是"设置时弹一次、之后开机免弹"不会重复弹的关键。 */
static BongoCatResult set_startup_task(bool enabled, bool prompt,
    BongoCatError *error) {
    /* 已提权会话直接静默执行, 不再弹窗。 */
    if (bongo_cat_platform_is_elevated()) prompt = false;

    wchar_t executable[BONGO_CAT_PATH_CAP];
    DWORD length = GetModuleFileNameW(NULL, executable, BONGO_CAT_PATH_CAP);
    if (!length || length >= BONGO_CAT_PATH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot determine executable path for the startup task");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    /* schtasks 用系统目录里的完整路径, 不依赖当前进程的工作目录/PATH。 */
    wchar_t schtasks_path[MAX_PATH];
    wchar_t sysdir[MAX_PATH];
    if (!GetSystemDirectoryW(sysdir, MAX_PATH) ||
        swprintf(schtasks_path, MAX_PATH, L"%ls\\schtasks.exe", sysdir) <= 0)
        wcscpy_s(schtasks_path, MAX_PATH, L"schtasks.exe");

    /* 只放 schtasks 的参数(不以 "schtasks " 开头): ShellExecuteExW 会把 lpFile 和
       lpParameters 拼成 "<lpFile> <参数>", 若参数里再带一个 "schtasks " 就会多出一个
       裸位置参数, 被 schtasks 当无效选项拒绝 —— 造成"任务明明点了允许却没建成"。 */
    wchar_t arguments[2048];
    int written;
    if (enabled) {
        /* /tr 的可执行路径再包一层引号, schtasks 才把带空格的路径当单个参数。 */
        written = swprintf(arguments, 2048,
            L"/create /f /tn \"%ls\" /sc onlogon /rl highest /tr \"\\\"%ls\\\" --autostart\"",
            BONGO_CAT_STARTUP_TASK_W, executable);
    } else {
        written = swprintf(arguments, 2048,
            L"/delete /f /tn \"%ls\"", BONGO_CAT_STARTUP_TASK_W);
    }
    if (written < 0 || written >= 2048) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Startup task command line does not fit");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[startup] schtasks: %ls %ls", schtasks_path, arguments);

    SHELLEXECUTEINFOW elevated = {0};
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    if (prompt) {
        elevated.cbSize = sizeof(elevated);
        elevated.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        elevated.lpVerb = L"runas";
        elevated.lpFile = schtasks_path;
        elevated.lpParameters = arguments;
        elevated.nShow = SW_HIDE;
        if (!ShellExecuteExW(&elevated)) {
            DWORD code = GetLastError();
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                code == ERROR_CANCELLED
                    ? "Administrator approval was cancelled for the startup task"
                    : "Cannot run schtasks.exe as administrator (error=%lu)",
                (unsigned long)code);
            return BONGO_CAT_ERROR_PLATFORM;
        }
        pi.hProcess = elevated.hProcess;
        pi.hThread = NULL;
    } else {
        /* CreateProcessW 需要可写且以程序名开头的命令行; lpApplicationName 给全路径,
           命令行首 token 的 "schtasks" 会被忽略, 仅作为 argv[0]。 */
        wchar_t command[2064];
        swprintf(command, 2064, L"schtasks.exe %ls", arguments);
        si.cb = sizeof(si);
        if (!CreateProcessW(schtasks_path, command, NULL, NULL, FALSE,
                CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Cannot start schtasks.exe (error=%lu)",
                (unsigned long)GetLastError());
            return BONGO_CAT_ERROR_PLATFORM;
        }
    }
    /*
     * 等 schtasks 退出。提权路径受 UAC 确认耗时影响, 30 秒太紧: 手慢一步就会在进程
     * 仍在跑时拿到 STILL_ACTIVE, 被下面误报成 "exit=259"。这里先等进程真正结束,
     * 拿到的 exit_code 才有意义; 真超时则单独报"超时", 不再抛一个误导的失败码。
     */
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "schtasks timed out waiting for the startup task operation");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[startup] schtasks 退出码: %lu", (unsigned long)exit_code);
    if (!enabled || exit_code == 0) return BONGO_CAT_OK;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "schtasks failed (exit=%lu)", (unsigned long)exit_code);
    return BONGO_CAT_ERROR_PLATFORM;
}

/* 已提权会话的"开机自启收口": 当进程已经是管理员且开好了 run_as_admin + autostart 时,
   把自启升格成最高权限计划任务, 下次由 Task Scheduler 直接拉起, 开机不再走 UAC。 */
BongoCatResult bongo_cat_platform_autostart_enforce_elevated(void) {
    BongoCatError error = {0};
    BongoCatResult result = set_startup_task(true, false, &error);
    if (result == BONGO_CAT_OK)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 开机自启已升级为最高权限计划任务, 下次开机不再弹 UAC");
    else
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 开机自启升级计划任务失败: %s", error.message);
    if (result == BONGO_CAT_OK) {
        BongoCatError short_error = {0};
        if (remove_startup_shortcut(&short_error) != BONGO_CAT_OK)
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] 清理普通开机快捷方式失败: %s",
                short_error.message);
    }
    return result;
}

/* 开机自启。enabled=true + elevated=true 时用计划任务以最高权限注册(开机免 UAC);
   其余情况沿用启动文件夹快捷方式。target 状态 = [快捷方式 IFF(autostart),
   计划任务 IFF(autostart && run_as_admin)]。 */
BongoCatResult bongo_cat_platform_set_autostart(bool enabled, bool elevated,
    BongoCatError *error) {
    if (!enabled) {
        BongoCatError task_error = {0};
        BongoCatResult task_result = set_startup_task(false, true, &task_error);
        BongoCatResult short_result = remove_startup_shortcut(error);
        if (task_result != BONGO_CAT_OK) {
            if (short_result == BONGO_CAT_OK) *error = task_error;
            return task_result;
        }
        return short_result;
    }
    if (elevated) {
        BongoCatResult task_result = set_startup_task(true, true, error);
        if (task_result != BONGO_CAT_OK) return task_result;
        BongoCatError short_error = {0};
        if (remove_startup_shortcut(&short_error) != BONGO_CAT_OK)
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] 清理普通开机快捷方式失败: %s",
                short_error.message);
        return BONGO_CAT_OK;
    }
    BongoCatResult short_result = create_startup_shortcut(error);
    if (short_result != BONGO_CAT_OK) return short_result;
    BongoCatError task_error = {0};
    if (set_startup_task(false, false, &task_error) != BONGO_CAT_OK)
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 收尾清理开机计划任务失败(忽略): %s",
            task_error.message);
    return BONGO_CAT_OK;
}
#endif