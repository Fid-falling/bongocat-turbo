/*
 * "用同一份 exe 重新启动自己" 的平台实现 (声明见 bongo_cat/platform.h)。
 *
 * 为什么这件事必须落在平台层: 新进程能不能起来, 取决于两件只有平台层知道的事 ——
 * 旧的单实例互斥体有没有还回去 (调用方负责, 见 runtime/lifecycle/application_restart.c),
 * 以及当前是不是管理员。后者决定了用哪条 API 拉起新进程。
 *
 * 两条不变量, 改动前先看明白:
 * 1. 已不存在防循环标记 (方案 B): 早期会用 BONGO_CAT_RESTARTED=1 标记"我是被自动
 *    重启出来的", 拦住"启动即被改设置"的路径。现已随方案 B 彻底移除 —— 系统里没有
 *    自动改设置的来源, 重启只由用户主动操作触发 (见 runtime/lifecycle/application_restart.c
 *    文件头的"无防循环标记"一段), 因此新进程不需要任何"我是重启出来的"标记。
 * 2. BONGO_CAT_ELEVATION_TRIED=1 必须跟着走。普通权限下重启时如果不带它, 新进程
 *    会先弹一次 UAC 再把自己提权重启一遍 —— 用户看到的是"重启后又要授权", 而且进程
 *    号会连换两次, 正好是"提权 -> 重启 -> 再提权"的循环。
 * 3. 已经是管理员就用 runas 拉起 (同一个已提权 token, 不会再弹 UAC); 不是管理员就用
 *    CreateProcessW 直接拉。提权状态因此被原样保持, 而不是"重启之后变了权限"。
 *
 * 失败语义: 返回非 OK 时**不改变任何进程状态**, 由调用方把单实例锁抢回来继续运行。
 * 唯一需要自己收尾的是刚设上去的提权标记 —— 留着它会让"下次再试一次重启"永远带着
 * 一个已经在弹 UAC 的痕迹。
 */
#include "bongo_cat/platform.h"
#include "bongo_cat/log.h"

#ifdef _WIN32
#include "windows_utf8.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>

#define BONGO_CAT_ELEVATION_TRIED_ENV "BONGO_CAT_ELEVATION_TRIED"
/* 命令行只可能由我们自己拼 (--storage-root=/--cache-root=), 上限仍然要有:
   被截断的命令行会让新进程用错目录, 那比不重启更糟。 */
#define BONGO_CAT_RESTART_COMMAND_CAP 2048

static BongoCatResult launch_failure(BongoCatError *error, DWORD code) {
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "Cannot restart BongoCat (Windows error %lu)", (unsigned long)code);
    return BONGO_CAT_ERROR_PLATFORM;
}

BongoCatResult bongo_cat_platform_restart_application(const char *arguments,
    BongoCatError *error) {
    wchar_t executable[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(NULL, executable,
        (DWORD)(sizeof(executable) / sizeof(executable[0])));
    if (length == 0 || length >= sizeof(executable) / sizeof(executable[0])) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot determine the executable path for a restart");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    const char *extra = arguments ? arguments : "";
    char command[BONGO_CAT_RESTART_COMMAND_CAP];
    /* 路径必须带引号: 程序装在 "C:\Program Files\..." 时不带引号会被拆成两个参数,
       新进程会去启动一个不存在的 exe。 */
    int written = snprintf(command, sizeof(command), "\"%ls\"%s%s",
        executable, extra[0] ? " " : "", extra);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Restart arguments do not fit (%d bytes)", written);
        return BONGO_CAT_ERROR_PLATFORM;
    }
    SetEnvironmentVariableA(BONGO_CAT_ELEVATION_TRIED_ENV, "1");
    /*
     * 已经是管理员: 走 runas。同样的已提权 token, 系统不会再弹 UAC, 新进程仍然是
     * 管理员。不用 CreateProcessW 是因为提权进程用普通方式创建子进程会丢掉提权。
     *
     * 不是管理员: 绝不能用 runas —— 那就变成"普通权限重启 -> 弹 UAC -> 提权"，
     * 正是任务里点名的那个循环。直接 CreateProcessW, lpCurrentDirectory 传 NULL
     * 让它继承当前工作目录 (nearby 模型扫描是按工作目录找的, 不能换)。
     */
    if (bongo_cat_platform_is_elevated()) {
        SHELLEXECUTEINFOW info = {0};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = executable;
        /* lpParameters 只要参数部分 (lpFile 已经是 exe 了), 而且要宽字符。 */
        wchar_t *wide_extra = extra[0] ? bongo_cat_windows_wide(extra) : NULL;
        info.lpParameters = wide_extra;
        info.nShow = SW_SHOWNORMAL;
        bool launched = ShellExecuteExW(&info) != FALSE;
        DWORD failure = launched ? 0 : GetLastError();
        if (info.hProcess) CloseHandle(info.hProcess);
        free(wide_extra);
        if (!launched) return launch_failure(error, failure);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 已用管理员身份重新启动 (参数: %s)",
            extra[0] ? extra : "无");
        return BONGO_CAT_OK;
    }
    wchar_t *wide_command = bongo_cat_windows_wide(command);
    if (!wide_command) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Restart command line is not valid UTF-8");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    bool launched = CreateProcessW(executable, wide_command, NULL, NULL, FALSE,
        0, NULL, NULL, &startup, &process) != FALSE;
    DWORD failure = launched ? 0 : GetLastError();
    free(wide_command);
    if (!launched) return launch_failure(error, failure);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] 已以当前权限重新启动 (参数: %s)", extra[0] ? extra : "无");
    return BONGO_CAT_OK;
}

#else /* 非 Windows: 没有"同一份 exe 重启自己"的通用做法, 明确不支持 */

BongoCatResult bongo_cat_platform_restart_application(const char *arguments,
    BongoCatError *error) {
    (void)arguments;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "BongoCat cannot restart itself automatically on this platform");
    return BONGO_CAT_ERROR_PLATFORM;
}

#endif