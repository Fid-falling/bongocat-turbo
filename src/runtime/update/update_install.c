/*
 * 自动替换 (Windows 自更新) 的调度层: 决定新文件放哪、什么时候请求重启、失败时怎么收场。
 *
 * 运行中的 exe 不能被覆盖, 但**可以被改名** —— 这是整个自替换能成立的关键 (Windows 加载
 * 映像时带着 FILE_SHARE_DELETE, 所以 MoveFile 改名成功、删除与覆盖失败)。于是流程是:
 *   1. 新版本下载成 exe 同目录下的 "<exe 名>.new.exe" (rename 同卷, 才能是原子的);
 *   2. 主程序走既有的退出路径正常退出 (先落盘配置、还回单实例锁、释放 Vulkan 等资源),
 *      退出前用同一个 exe 的隐藏模式拉起助手进程:
 *        BongoCatTurbo.exe --apply-update --wait-pid=<pid> --src=<新文件> --dst=<当前 exe>
 *      助手等本进程退出 -> 旧文件改名成 .old -> 新文件搬到当前 exe 的名字 -> 重新启动;
 *   3. 新版本第一次成功启动时删掉 .old (见 install_startup), 启动失败则 .old 留着可回滚。
 *
 * 失败一律"不破坏现有安装": 目录不可写就只下载到缓存目录并提示手动替换; 助手起不来就撤销
 * 重启意图、抢回单实例锁继续运行, 并把状态改成"已下载到 <路径>, 请手动替换"。
 * 无人值守运行 (--ci-* / 冒烟) 永远只检查、不下载、不替换, 见 install_request。
 */
#include "update_internal.h"
#include "preferences_notice.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/platform.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "windows_utf8.h"
#include <windows.h>
#include <stdlib.h>
#endif

/* 助手进程的结果回执: "<配置目录>/update-result.txt", 只有失败时才有内容。 */
#define RESULT_FILE "update-result.txt"
#define APPLY_LOG_FILE "update-apply.log"
#define RESULT_FAILED "failed"
/* 新版 exe 与旧版备份的后缀。新版带 .exe 结尾是为了用户手动替换时能直接双击验证。 */
#define NEW_SUFFIX ".new.exe"
#define OLD_SUFFIX ".old"

/* 当前进程 exe 的绝对路径 (UTF-8)。GetModuleFileNameW 是唯一权威来源: argv[0] 可能是
   相对路径, 也可能是别人传进来的任意字符串。 */
static bool current_executable(char *output, size_t capacity) {
#ifdef _WIN32
    wchar_t wide[BONGO_CAT_PATH_CAP];
    DWORD length = GetModuleFileNameW(NULL, wide,
        (DWORD)(sizeof(wide) / sizeof(wide[0])));
    if (!length || length >= sizeof(wide) / sizeof(wide[0])) return false;
    return bongo_cat_windows_utf8(wide, output, capacity);
#else
    (void)output;
    (void)capacity;
    return false;
#endif
}

/* 拆出目录与文件名 (同时接受 / 与 \)。两者都返回指向入参内部的借用指针。 */
static bool split_path(const char *path, char *directory, size_t directory_capacity,
    const char **name) {
    const char *slash = NULL;
    for (const char *at = path; *at; ++at)
        if (*at == '/' || *at == '\\') slash = at;
    if (!slash) return false;
    size_t length = (size_t)(slash - path);
    if (length + 1 > directory_capacity) return false;
    memcpy(directory, path, length);
    directory[length] = '\0';
    *name = slash + 1;
    return (*name)[0] != '\0';
}

/* "<当前 exe 的名字>.new.exe": 去掉原来的 .exe 再加后缀, 得到 BongoCatTurbo.new.exe。
   后缀特意保留 .exe: 用户需要手动替换时, 那个文件本身就能双击运行, 可以先用它验证一遍。 */
static void new_file_name(const char *executable_name, char *output, size_t capacity) {
    size_t length = strlen(executable_name);
    if (length > 4 && executable_name[length - 4] == '.' &&
        (executable_name[length - 3] == 'e' || executable_name[length - 3] == 'E') &&
        (executable_name[length - 2] == 'x' || executable_name[length - 2] == 'X') &&
        (executable_name[length - 1] == 'e' || executable_name[length - 1] == 'E'))
        length -= 4;
    if (length + sizeof(NEW_SUFFIX) > capacity) length = capacity - sizeof(NEW_SUFFIX);
    snprintf(output, capacity, "%.*s%s", (int)length, executable_name, NEW_SUFFIX);
}

/* 能不能在 path 处建文件。真正的下载随后会重新打开并截断它 —— 这里只做可行性探测,
   于是在 Program Files 这类只读目录下会立刻退到缓存目录, 而不是下到一半才失败。 */
static bool writable_target(const char *path) {
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return false;
    fclose(file);
    return true;
}

/*
 * 选定本次下载的落点。优先 exe 同目录 (替换必须是同卷 rename), 不可写时退到
 * <cache>/updates 并把 manual_only 置位 —— 那时只提示"已下载到 <路径>, 请手动替换",
 * 绝不尝试替换 (没权限就是没权限, 硬来只会把现有安装搞坏)。
 */
bool bongo_cat_update_install_target(BongoCatUpdateService *service, char *output,
    size_t capacity) {
    if (!service || !output || !capacity) return false;
    char executable[BONGO_CAT_PATH_CAP];
    char directory[BONGO_CAT_PATH_CAP];
    const char *name = NULL;
    if (!current_executable(executable, sizeof(executable)) ||
        !split_path(executable, directory, sizeof(directory), &name)) {
        SDL_LogWarn(BONGO_CAT_LOG_UPDATE, "Cannot determine the executable path");
        return false;
    }
    char file_name[BONGO_CAT_UPDATE_ASSET_CAP];
    new_file_name(name, file_name, sizeof(file_name));
    int written = snprintf(output, capacity, "%s/%s", directory, file_name);
    if (written < 0 || (size_t)written >= capacity) return false;
    service->manual_only = false;
    if (writable_target(output)) return true;
    const char *cache = service->app ? service->app->cache_root : NULL;
    if (!cache || !cache[0]) return false;
    char updates[BONGO_CAT_PATH_CAP];
    written = snprintf(updates, sizeof(updates), "%s/updates", cache);
    if (written < 0 || (size_t)written >= sizeof(updates)) return false;
    bongo_cat_path_create_directory(updates);
    written = snprintf(output, capacity, "%s/%s-%s", updates, service->release.version,
        file_name);
    if (written < 0 || (size_t)written >= capacity) return false;
    service->manual_only = true;
    SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
        "The installation directory is not writable; downloading to %s instead",
        output);
    return writable_target(output);
}

/* 接在路径后的可选参数; 各个 root 为空 (极早期失败) 时就整段省略。 */
static void append_argument(char *command, size_t capacity, const char *format,
    const char *value) {
    size_t used = strlen(command);
    if (!value || !value[0] || used + 1 >= capacity) return;
    int written = snprintf(command + used, capacity - used, format, value);
    if (written < 0 || (size_t)written >= capacity - used)
        command[used] = '\0';
}

/*
 * 创建"我还活着"的命名互斥体并持有它 (不释放、不关闭): 进程结束时系统会自动放弃它,
 * 于是助手进程的 WaitForSingleObject 会立刻返回 WAIT_ABANDONED —— 这就是"等旧进程退出"
 * 的全部实现, 且不需要任何跨进程句柄 API (OpenProcess 在本仓库属于禁用的生产 API,
 * 见 cmake/CheckPlatformRuntimeSafety.cmake)。名字里带 PID, 日志里一眼能对上。
 */
static void create_alive_mutex(BongoCatUpdateService *service) {
#ifdef _WIN32
    if (service->alive_mutex) return;
    snprintf(service->alive_name, sizeof(service->alive_name),
        BONGO_CAT_SLUG "-update-alive-%lu", (unsigned long)GetCurrentProcessId());
    wchar_t *wide = bongo_cat_windows_wide_text(service->alive_name);
    if (!wide) return;
    /* bInitialOwner = TRUE: 归属本线程 → 本进程一死就被判为 abandoned。 */
    service->alive_mutex = CreateMutexW(NULL, TRUE, wide);
    free(wide);
    if (!service->alive_mutex) {
        service->alive_name[0] = '\0';
        SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
            "Cannot create the update handshake mutex");
    }
#endif
}

/*
 * 拉起助手进程 (同一个 exe 的隐藏模式)。参数逐个加引号: exe 可能装在
 * "C:\Program Files\..." 下, 不加引号会被拆成两个参数。
 * restart-args 末尾刻意补一个空格: 参数值以反斜杠结尾时, 结尾引号会被命令行解析器当成
 * 转义引号 (如 --storage-root=C:\dir\), 补一个空格就绕开了这个坑。
 * --wait-pid 只用于日志/对号入座, 真正的等待靠 --wait-mutex (见 create_alive_mutex)。
 */
bool bongo_cat_update_install_spawn(BongoCatApp *app, const char *restart_arguments,
    BongoCatError *error) {
#ifdef _WIN32
    if (!app || !app->update) return false;
    BongoCatUpdateService *service = app->update;
    char executable[BONGO_CAT_PATH_CAP];
    if (!service->new_path[0] || !current_executable(executable, sizeof(executable))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot determine the paths for the update");
        return false;
    }
    create_alive_mutex(service);
    char command[BONGO_CAT_PATH_CAP * 3];
    int written = snprintf(command, sizeof(command),
        "--apply-update --wait-pid=%lu --src=\"%s\" --dst=\"%s\" --restart-args=\"%s \"",
        (unsigned long)GetCurrentProcessId(), service->new_path, executable,
        restart_arguments ? restart_arguments : "");
    if (written < 0 || (size_t)written >= sizeof(command)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "The update command line does not fit");
        return false;
    }
    if (service->alive_name[0])
        append_argument(command, sizeof(command), " --wait-mutex=\"%s\"",
            service->alive_name);
    char path[BONGO_CAT_PATH_CAP];
    if (app->config_root[0] && snprintf(path, sizeof(path), "%s/%s",
            app->config_root, RESULT_FILE) > 0)
        append_argument(command, sizeof(command), " --result-file=\"%s\"", path);
    if (app->log_root[0] && snprintf(path, sizeof(path), "%s/%s",
            app->log_root, APPLY_LOG_FILE) > 0)
        append_argument(command, sizeof(command), " --log-file=\"%s\"", path);
    BongoCatResult result = bongo_cat_platform_restart_application(command, error);
    if (result != BONGO_CAT_OK) {
        SDL_LogError(BONGO_CAT_LOG_UPDATE,
            "Cannot start the update helper: %s", error ? error->message : "");
        return false;
    }
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
        "Update helper started; the application will exit and be replaced: %s",
        service->new_path);
    return true;
#else
    (void)app;
    (void)restart_arguments;
    if (error) bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "Automatic updates are not supported on this platform");
    return false;
#endif
}

/* 新文件还在磁盘上时, "已下载到 <路径>, 请手动替换"这句提示才有意义。 */
static bool pending_file(BongoCatUpdateService *service) {
    return service->new_path[0] && bongo_cat_path_is_file(service->new_path);
}

/* 替换环节失败后的统一收尾: 状态改成 ERROR 并带上"手动替换"的信息, 然后提示用户。 */
static void finish_with_failure(BongoCatUpdateService *service,
    const char *message) {
    char path[BONGO_CAT_PATH_CAP] = {0};
    bool keep = pending_file(service);
    if (keep) snprintf(path, sizeof(path), "%s", service->new_path);
    SDL_LockMutex(service->mutex);
    service->status = BONGO_CAT_UPDATE_ERROR;
    service->manual_replace = keep;
    snprintf(service->manual_path, sizeof(service->manual_path), "%s", path);
    snprintf(service->error, sizeof(service->error), "%s", message);
    SDL_UnlockMutex(service->mutex);
    SDL_LogError(BONGO_CAT_LOG_UPDATE, "Automatic update failed: %s (%s)",
        message, keep ? path : "no downloaded file left");
    bongo_cat_update_show_completion(service);
}

/*
 * 主线程收到"校验通过"后的唯一入口。三条出路:
 *   1. 无人值守运行: 只记日志 —— CI 只能检查, 绝不动二进制;
 *   2. 只能手动替换 (安装目录不可写): 不尝试替换, 直接提示文件位置;
 *   3. 正常情况: 请求"关掉自己换新版本"的重启 (由 application_restart.c 执行)。
 */
void bongo_cat_update_install_request(BongoCatUpdateService *service) {
    if (!service || !service->app) return;
    BongoCatApp *app = service->app;
    if (app->smoke || app->automated) {
        SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
            "Unattended run (%s): the update was verified but will not be installed",
            app->smoke ? "smoke" : "automated");
        finish_with_failure(service, BONGO_CAT_UPDATE_ERROR_NOT_WRITABLE);
        return;
    }
    if (service->manual_only) {
        finish_with_failure(service, BONGO_CAT_UPDATE_ERROR_NOT_WRITABLE);
        return;
    }
    if (!bongo_cat_app_request_update_restart(app)) {
        finish_with_failure(service, BONGO_CAT_UPDATE_ERROR_REPLACE_FAILED);
        return;
    }
    /* 状态保持 INSTALLING, 界面显示"准备重启…"; 进程会在主循环末尾退出并交给助手进程。
       这里**不能**再补一次阶段事件: 事件处理函数会再次调用本函数。 */
}

/* 助手进程没能拉起 (被安全软件拦、命令行太长): 撤销替换意图, 让用户手动替换。 */
void bongo_cat_update_install_abort(BongoCatUpdateService *service,
    const BongoCatError *error) {
    if (!service) return;
    SDL_LogError(BONGO_CAT_LOG_UPDATE,
        "The update helper could not be started (%s); keeping the current version",
        error && error->message[0] ? error->message : "unknown error");
    finish_with_failure(service, BONGO_CAT_UPDATE_ERROR_REPLACE_FAILED);
}

/*
 * 启动时的一次性收尾 (由 bongo_cat_update_create 调用, 位置刻意放在应用初始化之后:
 * 能走到这里就说明新版本真的起来了, 备份可以删了)。
 *   - ".old": 上一版可执行文件的备份。删掉它就是"新版本成功启动过一次"的落地动作;
 *     删不掉 (被杀软占用等) 只记日志, 下次启动再试。
 *   - ".new.exe": **刻意不删**。用户看到"请手动替换"的提示之后可能先关掉程序, 那时这个
 *     文件就是他要手动替换的东西; 下一次下载会自行截断覆盖它。
 *   - 结果回执: 助手进程写入的失败原因, 读走并删除, 然后按需提示用户。
 * 无人值守运行只记日志: 这里可能会弹模态框, 而 CI 里没人能点。
 */
void bongo_cat_update_install_startup(BongoCatUpdateService *service) {
    if (!service || !service->app) return;
    char executable[BONGO_CAT_PATH_CAP];
    if (current_executable(executable, sizeof(executable))) {
        char backup[BONGO_CAT_PATH_CAP];
        int written = snprintf(backup, sizeof(backup), "%s%s", executable,
            OLD_SUFFIX);
        if (written > 0 && (size_t)written < sizeof(backup) &&
            bongo_cat_path_is_file(backup)) {
            if (bongo_cat_path_remove(backup))
                SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
                    "Removed the previous version backup after a successful start: %s",
                    backup);
            else SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
                "Cannot remove the previous version backup: %s", backup);
        }
    }
    BongoCatApp *app = service->app;
    char result_path[BONGO_CAT_PATH_CAP];
    if (!app->config_root[0] || snprintf(result_path, sizeof(result_path),
            "%s/%s", app->config_root, RESULT_FILE) < 0) return;
    if (!bongo_cat_path_is_file(result_path)) return;
    char content[128] = {0};
    FILE *file = bongo_cat_file_open(result_path, "rb");
    if (file) {
        size_t length = fread(content, 1, sizeof(content) - 1, file);
        content[length] = '\0';
        fclose(file);
    }
    bongo_cat_path_remove(result_path);
    if (strncmp(content, RESULT_FAILED, strlen(RESULT_FAILED)) != 0) return;
    const char *message = bongo_cat_i18n_get(app->i18n,
        "native.support.updateDownloadFailed",
        "The update could not be installed automatically. Please try again later.");
    SDL_LogError(BONGO_CAT_LOG_UPDATE,
        "The previous automatic update did not finish; the current version is intact");
    if (app->smoke || app->automated) return;
    bongo_cat_preferences_notice_show(app, message, true);
    if (!app->window || (app->preferences &&
            bongo_cat_preferences_visible(app->preferences))) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, BONGO_CAT_NAME, message,
        app->window);
}
