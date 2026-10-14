/*
 * 开机自启 (Windows): 启动文件夹里的快捷方式 + 最高权限计划任务。
 *
 * 为什么单独一个文件: windows_startup.c 已经贴着 500 行的策略上限 (见
 * cmake/CheckLines.cmake), 而这一块从"写快捷方式"到"schtasks 提权"自成一体, 与
 * 单实例/唤醒那部分没有共享状态 —— 正好搬出来。
 *
 * 两条容易踩空的地方, 改之前先看:
 *   1. 目标状态是 [快捷方式 IFF autostart] + [计划任务 IFF autostart && run_as_admin],
 *      所以开关的两个方向都要**同时**处理两种对象: 只删一个会留下"上次提权注册的任务"
 *      在下次开机时把普通权限的进程再拉起来。
 *   2. 提权会话里执行 schtasks 必须走静默路径 (prompt=false), 否则用户在设置里点一下
 *      就会再看到一个 UAC —— "设置时弹一次、之后开机免弹"这条约定就断了 (见
 *      bongo_cat_platform_autostart_enforce_elevated 的调用方)。
 */
#include "bongo_cat/platform.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "bongo_cat/log.h"

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
