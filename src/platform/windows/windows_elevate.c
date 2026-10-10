#include "bongo_cat/platform.h"
#include "bongo_cat/log.h"

#ifdef _WIN32
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
    info.lpParameters = GetCommandLineW();
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

#else /* 非 Windows: 没有 UAC 这一套, 直接当作"无需提权" */

bool bongo_cat_platform_is_elevated(void) { return false; }
bool bongo_cat_platform_relaunch_elevated(void) { return false; }
bool bongo_cat_platform_relaunch_elevated_if_needed(void) { return false; }

#endif
