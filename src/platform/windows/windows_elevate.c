#include "bongo_cat/platform.h"
#include "bongo_cat/log.h"
#include "bongo_cat/common.h"
#include "windows_utf8.h"

#ifdef _WIN32
#include <stdio.h>
#include <SDL3/SDL.h>
#include <windows.h>
#include <shellapi.h>

/*
 * 以管理员身份运行。
 *
 * 清单里声明的是 asInvoker (见 cmake/windows.manifest.in), 所以提权要在运行时
 * 做: 启动时发现当前不是管理员, 就用 ShellExecuteExW 的 "runas" 动词把自己重新
 * 拉起来, 旧进程立刻退出 (交棒)。
 *
 * 为什么不用清单的 requireAdministrator: 那样"想关都关不掉" —— 每次启动必弹 UAC,
 * 而且开机自启会直接被系统拒绝。运行时提权至少还能靠一个开关绕开。
 *
 * 用环境变量 BONGO_CAT_ELEVATION_TRIED 做幂等标记: 提权后的子进程会继承它, 于是
 * 不会再次尝试 (用户拒绝 UAC 时也不会在同一台机器上无限重试)。
 */
#define BONGO_CAT_ELEVATION_TRIED "BONGO_CAT_ELEVATION_TRIED"
#define BONGO_CAT_ELEVATED_COMMAND_CAP 2048

bool bongo_cat_platform_is_elevated(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {0};
    DWORD size = sizeof(elevation);
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
        size, &size) && elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

/*
 * 取原始命令行里"程序名之后"的参数部分, 供提权重启用。不能直接把 GetCommandLineW()
 * 的整个结果塞进 lpParameters: ShellExecute 会把子进程命令行拼成 "<lpFile> <参数>",
 * 而 GetCommandLineW() 已经带了程序名, 结果 argv[1] 会多出一个旧的 exe 路径 —— 目前
 * 命令行解析会静默忽略未知参数(见 startup_arguments.c), 所以暂时没炸, 但这是一条脆弱
 * 的暗线: 将来一旦接受位置参数就会把参数整体错位。这里按引号规则跳过第一个 token。
 * 返回的指针指向进程常驻命令行缓冲, 无需释放。
 */
static const wchar_t *command_arguments(void) {
    const wchar_t *cursor = GetCommandLineW();
    bool quoted = false;
    while (*cursor == L' ' || *cursor == L'\t') ++cursor;
    for (;;) {
        wchar_t ch = *cursor++;
        if (ch == L'"') quoted = !quoted;
        else if (ch == L'\0') return L"";
        else if ((ch == L' ' || ch == L'\t') && !quoted) {
            while (*cursor == L' ' || *cursor == L'\t') ++cursor;
            return cursor;
        }
    }
}

bool bongo_cat_platform_relaunch_elevated(void) {
    wchar_t path[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(NULL, path,
        (DWORD)(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= sizeof(path) / sizeof(path[0])) return false;
    SHELLEXECUTEINFOW info = {0};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.lpParameters = command_arguments();
    info.nShow = SW_SHOWNORMAL;
    SetEnvironmentVariableA(BONGO_CAT_ELEVATION_TRIED, "1");
    if (!ShellExecuteExW(&info)) {
        DWORD error = GetLastError();
        if (error == ERROR_CANCELLED)
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] 用户取消了管理员提权, 本次以普通权限继续运行");
        else
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] 管理员提权失败 (error=%lu), 本次以普通权限继续运行",
                (unsigned long)error);
        return false;
    }
    if (info.hProcess) CloseHandle(info.hProcess);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] 已以管理员身份重新启动, 旧进程退出");
    return true;
}

bool bongo_cat_platform_relaunch_elevated_if_needed(void) {
    if (bongo_cat_platform_is_elevated()) return false;
    if (SDL_getenv(BONGO_CAT_ELEVATION_TRIED)) return false;
    return bongo_cat_platform_relaunch_elevated();
}

/*
 * 提权后放行拖放消息 (UIPI)。
 *
 * 为什么必须做: 资源管理器 (explorer.exe) 跑在中等完整性级别, 而提权后的本进程是高
 * 完整性级别。UIPI 默认禁止低完整性进程向高完整性窗口投递消息, 于是从资源管理器拖一个
 * zip 到窗口上**完全没反应** —— 消息根本到不了 SDL 的消息循环, 也就没有 SDL_EVENT_DROP_*。
 * 表现非常有迷惑性: 同一份程序不提权就一切正常, 一提权就"拖不动"。
 *
 * 放行三条:
 *   - WM_DROPFILES: SDL 在没初始化 OLE 时的拖放回退通路, 消息里带一个 HDROP;
 *   - WM_COPYDATA: 经典的自定义跨进程数据搬运, 拖放源可能用它;
 *   - WM_COPYGLOBALDATA(0x0049): **未文档化**的内部消息, OLE 在跨进程传递 IDataObject
 *     时要靠它把 HGLOBAL 搬进目标进程。SDL3 在 Windows 上默认 OleInitialize 成功, 走的
 *     正是 OLE 的 IDropTarget 通路 —— 只放行 WM_DROPFILES 是不够的, 这一条才是关键。
 *     旧的 Windows SDK 头里没有这个宏, 所以自己定义。
 *
 * 只在已提权时动过滤表: 没提权时 UIPI 本来就不拦, 改了反而白白放宽本进程的窗口过滤。
 *
 * 覆盖范围: 传进来的窗口 + 本线程所有顶层窗口。后者是为了顺带覆盖 OLE 自己建的中转窗口
 * (OleInitialize 在 SDL 初始化视频时就已经建好, 属于同一个线程), 以及以后新增的窗口创建
 * 路径 —— 漏掉任何一个, 那个窗口上的拖放就仍然是"没反应"。
 */
#ifndef WM_COPYGLOBALDATA
#define WM_COPYGLOBALDATA 0x0049
#endif

static const UINT drop_messages[] = {
    WM_DROPFILES, WM_COPYGLOBALDATA, WM_COPYDATA
};

static void allow_drop_messages(HWND hwnd) {
    if (!hwnd) return;
    for (size_t i = 0; i < sizeof(drop_messages) / sizeof(drop_messages[0]); ++i)
        ChangeWindowMessageFilterEx(hwnd, drop_messages[i], MSGFLT_ALLOW, NULL);
}

static BOOL CALLBACK allow_drop_messages_for_window(HWND hwnd, LPARAM unused) {
    (void)unused;
    allow_drop_messages(hwnd);
    return TRUE;
}

void bongo_cat_platform_allow_file_drop(SDL_Window *window) {
    if (!bongo_cat_platform_is_elevated()) return;
    HWND hwnd = window ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
    allow_drop_messages(hwnd);
    EnumThreadWindows(GetCurrentThreadId(), allow_drop_messages_for_window, 0);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] 已放行拖放消息 (UIPI): hwnd=%p", (void *)hwnd);
}

BongoCatResult bongo_cat_platform_restart_application_elevated(
    const char *arguments, BongoCatError *error) {
    wchar_t executable[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(NULL, executable,
        (DWORD)(sizeof(executable) / sizeof(executable[0])));
    if (length == 0 || length >= sizeof(executable) / sizeof(executable[0])) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot determine the executable path for an elevated restart");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    const char *extra = arguments ? arguments : "";
    char command[BONGO_CAT_ELEVATED_COMMAND_CAP];
    /* 路径必须带引号: Program Files 里的路径不带引号会被拆成两个参数。 */
    int written = snprintf(command, sizeof(command), "\"%ls\"%s%s",
        executable, extra[0] ? " " : "", extra);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Elevated restart arguments do not fit (%d bytes)", written);
        return BONGO_CAT_ERROR_PLATFORM;
    }
    wchar_t *wide = bongo_cat_windows_wide(command);
    if (!wide) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Elevated restart command line is not valid UTF-8");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    /* 标记带上: 新进程不再弹 UAC (ELEVATION_TRIED 幂等)。已不再设置 RESTARTED 防循环
       标记 (方案 B), 见 runtime/lifecycle/application_restart.c 文件头。 */
    SetEnvironmentVariableA(BONGO_CAT_ELEVATION_TRIED, "1");
    SHELLEXECUTEINFOW info = {0};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = executable;
    info.lpParameters = wide;
    info.nShow = SW_SHOWNORMAL;
    bool launched = ShellExecuteExW(&info) != FALSE;
    DWORD failure = launched ? 0 : GetLastError();
    if (info.hProcess) CloseHandle(info.hProcess);
    free(wide);
    if (!launched) {
        /* 没起来就原样返回: ELEVATION_TRIED 留在本进程不再重复弹窗, 由调用方继续
           运行并提示。 */
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot relaunch BongoCat as administrator (Windows error %lu)",
            (unsigned long)failure);
        return BONGO_CAT_ERROR_PLATFORM;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] 已以管理员身份重新启动 (参数: %s)",
        extra[0] ? extra : "无");
    return BONGO_CAT_OK;
}

#else /* 非 Windows: 没有 UAC 这一套, 直接当作"无需提权" */

bool bongo_cat_platform_is_elevated(void) { return false; }
bool bongo_cat_platform_relaunch_elevated(void) { return false; }
bool bongo_cat_platform_relaunch_elevated_if_needed(void) { return false; }
void bongo_cat_platform_allow_file_drop(SDL_Window *window) { (void)window; }
BongoCatResult bongo_cat_platform_restart_application_elevated(
    const char *arguments, BongoCatError *error) {
    (void)arguments;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "BongoCat cannot relaunch elevated on this platform");
    return BONGO_CAT_ERROR_PLATFORM;
}

#endif