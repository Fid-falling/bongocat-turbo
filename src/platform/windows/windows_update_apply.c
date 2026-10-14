#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/sha256.h"
#include "bongo_cat/update.h"
#include "windows_utf8.h"

#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/*
 * "--apply-update": 同一个 exe 的隐藏命令行模式, 负责在旧进程退出之后完成替换并重启。
 *
 * 为什么必须由"同一个 exe 的另一个进程"来做: 运行中的 exe 文件不能被覆盖, 但**可以被
 * 改名** (加载器带着 FILE_SHARE_DELETE 打开映像, 所以 MoveFile 成功、DeleteFile 失败)。
 * 替换顺序因此是: 旧文件改名成 <exe>.old -> 新文件搬到 <exe> 的名字 -> 重新启动。
 * 进程内做不了的原因是"自己还在跑的时候不能改名自己再落到自己的名字上"—— 那要在
 * 映像还活着的时候删掉它的目录项, 系统不允许。
 *
 * 三条不变量:
 * 1. 这个模式在 SDL/窗口/单实例锁之前就返回 (见 app_entry.c), 不建立任何界面; 因此它
 *    既不会和用户抢单实例, 也不会在无人值守运行里弹东西;
 * 2. 任何一步失败都必须让磁盘上仍然是**一个能跑的程序**: 改名失败就原样重启旧文件,
 *    搬新文件失败就把 .old 改回去再重启旧文件;
 * 3. 替换前再校验一次新文件的 SHA-256 (主进程已经校验过): 从下载完成到此刻之间存在
 *    被掉包/写到一半的时间窗, 这里补最后一道闸。
 *
 * 日志与结果回执: 助手进程没有控制台, 所以失败原因写进 --log-file 指定的日志, 并在
 * --result-file 里留一个 "failed", 由下一次启动的主程序读走并提示用户。
 */
#define APPLY_ARGUMENT "--apply-update"
#define WAIT_PREFIX "--wait-pid="
#define WAIT_MUTEX_PREFIX "--wait-mutex="
#define SRC_PREFIX "--src="
#define DST_PREFIX "--dst="
#define SHA_PREFIX "--sha256="
#define RESTART_PREFIX "--restart-args="
#define RESULT_PREFIX "--result-file="
#define LOG_PREFIX "--log-file="
/* 等旧进程退出: 正常退出是毫秒级的, 60 秒只是"用户机器卡住/有调试器挂着"时的兜底。 */
#define APPLY_WAIT_TIMEOUT_MS 60000u
/* 改名重试: 旧进程刚退出时, 杀软/索引器还可能短暂占着文件, 250ms x 40 = 10 秒。 */
#define APPLY_REPLACE_ATTEMPTS 40
#define APPLY_RETRY_MS 250u
#define APPLY_FAILED "failed"

typedef struct UpdateApplyRequest {
    unsigned long pid;
    const char *wait_mutex;
    const char *source;
    const char *target;
    const char *sha256;
    const char *restart_arguments;
    const char *result_file;
    const char *log_file;
} UpdateApplyRequest;

static void write_log(const UpdateApplyRequest *request, const char *line) {
    if (!request->log_file || !request->log_file[0]) return;
    bongo_cat_file_append(request->log_file, line, strlen(line));
    bongo_cat_file_append(request->log_file, "\r\n", 2);
}

static void write_result(const UpdateApplyRequest *request, const char *text) {
    if (!request->result_file || !request->result_file[0]) return;
    FILE *file = bongo_cat_file_open(request->result_file, "wb");
    if (!file) return;
    fprintf(file, "%s\n", text);
    fclose(file);
}

static bool prefixed(const char *argument, const char *prefix,
    const char **value) {
    size_t length = strlen(prefix);
    if (strncmp(argument, prefix, length) != 0) return false;
    *value = argument + length;
    return true;
}

/* 只认 "--apply-update" 加上那几个前缀参数; 解析不出 src/dst 就整体失败并当作不是本模式,
   让正常的启动流程接手 (参数是系统注入的、或者用户手滑, 都不该变成一次替换)。 */
static bool parse_request(int argc, char **argv, UpdateApplyRequest *request) {
    memset(request, 0, sizeof(*request));
    bool apply = false;
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (!argument) continue;
        const char *value = NULL;
        if (strcmp(argument, APPLY_ARGUMENT) == 0) apply = true;
        else if (prefixed(argument, WAIT_PREFIX, &value))
            request->pid = strtoul(value, NULL, 10);
        else if (prefixed(argument, WAIT_MUTEX_PREFIX, &value))
            request->wait_mutex = value;
        else if (prefixed(argument, SRC_PREFIX, &value)) request->source = value;
        else if (prefixed(argument, DST_PREFIX, &value)) request->target = value;
        else if (prefixed(argument, SHA_PREFIX, &value)) request->sha256 = value;
        else if (prefixed(argument, RESTART_PREFIX, &value))
            request->restart_arguments = value;
        else if (prefixed(argument, RESULT_PREFIX, &value))
            request->result_file = value;
        else if (prefixed(argument, LOG_PREFIX, &value)) request->log_file = value;
    }
    return apply && request->source && request->source[0] && request->target &&
        request->target[0];
}

/*
 * 等旧进程退出。判据是主程序持有的命名互斥体 (见 runtime/update/update_install.c 的
 * create_alive_mutex): 它随进程死亡被系统放弃, 于是这里会拿到 WAIT_ABANDONED。
 *
 * 为什么不用 PID + OpenProcess: 本仓库把 OpenProcess 列为禁用的生产 API
 * (cmake/CheckPlatformRuntimeSafety.cmake), 而且"进程是否还活着"本来就有一个不需要
 * 任何句柄的等价信号。--wait-pid 只用于日志与互斥体名字的对号入座。
 * 互斥体不存在 = 旧进程已经退出 (或名字不对), 直接往下走。
 */
static bool wait_for_previous(unsigned long pid, const char *name) {
    if (!name || !name[0]) {
        if (pid) return true;
        return true;
    }
    wchar_t *wide = bongo_cat_windows_wide_text(name);
    if (!wide) return true;
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, wide);
    free(wide);
    if (!mutex) return true;
    DWORD result = WaitForSingleObject(mutex, APPLY_WAIT_TIMEOUT_MS);
    CloseHandle(mutex);
    return result == WAIT_ABANDONED || result == WAIT_OBJECT_0;
}

static bool replace_target(const UpdateApplyRequest *request) {
    char backup[BONGO_CAT_PATH_CAP];
    int written = snprintf(backup, sizeof(backup), "%s.old", request->target);
    if (written < 0 || (size_t)written >= sizeof(backup)) return false;
    bool moved_away = !bongo_cat_path_is_file(request->target);
    for (int attempt = 0; !moved_away && attempt < APPLY_REPLACE_ATTEMPTS;
            ++attempt) {
        if (bongo_cat_path_rename(request->target, backup)) {
            write_log(request, "previous executable renamed to the .old backup");
            moved_away = true;
            break;
        }
        Sleep(APPLY_RETRY_MS);
    }
    if (!moved_away) {
        write_log(request, "cannot rename the running executable");
        return false;
    }
    for (int attempt = 0; attempt < APPLY_REPLACE_ATTEMPTS; ++attempt) {
        if (bongo_cat_path_rename(request->source, request->target)) {
            write_log(request, "new executable moved into place");
            return true;
        }
        Sleep(APPLY_RETRY_MS);
    }
    /* 新文件搬不过去 (磁盘满/被杀软拦): 把旧文件改回原名, 用户至少还有一个能跑的程序。 */
    write_log(request, "cannot move the new executable into place; rolling back");
    if (bongo_cat_path_rename(backup, request->target))
        write_log(request, "the previous executable was restored");
    else write_log(request, "rollback failed; <exe>.old holds the previous version");
    return false;
}

/* 替换前的最后一道校验。失败时删掉新文件 (它已被判定不可信), 旧文件不动。 */
static bool verify_source(const UpdateApplyRequest *request) {
    if (!request->sha256 || !request->sha256[0]) return true;
    char actual[BONGO_CAT_UPDATE_DIGEST_CAP];
    BongoCatError error = {0};
    if (bongo_cat_sha256_file(request->source, actual, &error) == BONGO_CAT_OK &&
        strcmp(actual, request->sha256) == 0) return true;
    write_log(request, "the new executable failed its SHA-256 check; it was deleted");
    bongo_cat_path_remove(request->source);
    return false;
}

/* 把用户拉回来: 用与主程序相同的启动方式 (保持提权状态、带 --storage-root 等) 重启。 */
static bool relaunch(const UpdateApplyRequest *request) {
    BongoCatError error = {0};
    BongoCatResult result = bongo_cat_platform_restart_application(
        request->restart_arguments ? request->restart_arguments : "", &error);
    if (result != BONGO_CAT_OK) {
        write_log(request, "cannot restart the application");
        return false;
    }
    return true;
}

bool bongo_cat_platform_update_apply_argument(int argc, char **argv,
    int *exit_code) {
    UpdateApplyRequest request;
    if (!parse_request(argc, argv, &request)) return false;
    if (exit_code) *exit_code = 1;
    write_log(&request, "update helper started");
    /* 助手应当就是从 --dst 启动的 (主程序用"重启自己"的方式拉起它): 只有那样, 替换之后
       的重启才会拉起新文件。路径不一致时记一条警告方便排查 (测试里就是这么跑的)。 */
    wchar_t wide_self[BONGO_CAT_PATH_CAP];
    char self[BONGO_CAT_PATH_CAP] = {0};
    DWORD self_length = GetModuleFileNameW(NULL, wide_self,
        (DWORD)(sizeof(wide_self) / sizeof(wide_self[0])));
    if (self_length && self_length < sizeof(wide_self) / sizeof(wide_self[0]) &&
        bongo_cat_windows_utf8(wide_self, self, sizeof(self)) &&
        strcmp(self, request.target) != 0)
        write_log(&request, "warning: the helper is not running from --dst; "
            "the restart will use the helper's own path");
    if (!wait_for_previous(request.pid, request.wait_mutex))
        write_log(&request, "timed out waiting for the previous process");
    if (!verify_source(&request)) {
        write_result(&request, APPLY_FAILED);
        relaunch(&request);
        return true;
    }
    if (!replace_target(&request)) {
        write_result(&request, APPLY_FAILED);
        relaunch(&request);
        return true;
    }
    if (!relaunch(&request)) {
        write_result(&request, APPLY_FAILED);
        return true;
    }
    write_log(&request, "update applied");
    if (exit_code) *exit_code = 0;
    return true;
}
#endif
