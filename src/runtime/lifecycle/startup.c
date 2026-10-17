/*
 * 启动的可观测性: 日志输出、启动阶段标记、启动失败记录。
 *
 * 阶段标记 (state_root/startup-stage.txt) 是崩溃诊断的核心: 每个阶段进入时写一次
 * (先写 .tmp 再原子替换), 跑到 running 就删掉。下次启动如果发现文件还在, 说明上次
 * 是在那个阶段挂的 —— begin_log() 会把它读出来并 WARN。因此**新增启动步骤时一定要
 * 补一个 bongo_cat_startup_stage()**, 否则崩溃现场就断在这后面了。
 *
 * runtime-state.txt 记的是"最后到达的阶段", 正常退出写 clean-shutdown, 用来区分
 * 崩溃 / 被强杀 / 挂起。
 *
 * 日志是进程内单例 (log_mutex + runtime_log_path 都是文件级静态变量): 主进程负责清空
 * 本次 session 日志, 副宠只追加 —— 否则副宠启动会把主进程已经写下的诊断擦掉。
 */
#include "runtime.h"
#include "runtime_state.h"
#include "bongo_cat/file.h"
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/renderer_module.h"
#include "portable_mode.h"
#include "storage_paths.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows_package.h"
#endif

static char runtime_log_path[BONGO_CAT_PATH_CAP];
static char log_source[BONGO_CAT_ID_CAP + 8] = "unknown";
static bool startup_is_ready;
static SDL_Mutex *log_mutex;

static const char *priority_name(SDL_LogPriority priority) {
    switch (priority) {
    case SDL_LOG_PRIORITY_TRACE: return "TRACE";
    case SDL_LOG_PRIORITY_VERBOSE: return "VERBOSE";
    case SDL_LOG_PRIORITY_DEBUG: return "DEBUG";
    case SDL_LOG_PRIORITY_INFO: return "INFO";
    case SDL_LOG_PRIORITY_WARN: return "WARN";
    case SDL_LOG_PRIORITY_ERROR: return "ERROR";
    case SDL_LOG_PRIORITY_CRITICAL: return "CRITICAL";
    default: return "UNKNOWN";
    }
}

/* 自己拼一行日志: 时间戳/来源/分类/级别 + 去掉换行的正文, 一次 append 写完。前缀长度
   先用 snprintf(NULL, 0) 预算, 因此正文里带换行也不会把日志撕裂成多行。 */
static void append_log(const char *path, const char *timestamp,
    int category, SDL_LogPriority priority, const char *message) {
    if (!path || !path[0]) return;
    const char *text = message ? message : "";
    int prefix = snprintf(NULL, 0, "%s [%s] [%s:%d] ",
        timestamp ? timestamp : "unknown-time", log_source,
        priority_name(priority), category);
    size_t text_length = strlen(text);
    char *line = prefix >= 0 && text_length < SIZE_MAX - (size_t)prefix - 2
        ? malloc((size_t)prefix + text_length + 2) : NULL;
    if (!line) return;
    snprintf(line, (size_t)prefix + 1, "%s [%s] [%s:%d] ",
        timestamp ? timestamp : "unknown-time", log_source,
        priority_name(priority), category);
    for (size_t i = 0; i < text_length; ++i)
        line[(size_t)prefix + i] = text[i] == '\r' || text[i] == '\n'
            ? ' ' : text[i];
    size_t length = (size_t)prefix + text_length;
    line[length++] = '\n';
    bongo_cat_file_append(path, line, length);
    free(line);
}

/* 挂到 SDL 的日志钩子上: 先过 bongo_cat_log_enabled 的过滤 (避免重复落盘), 再上锁 ——
   日志可能来自音频/网络线程。 */
static void SDLCALL log_output(void *userdata, int category,
    SDL_LogPriority priority, const char *message) {
    (void)userdata;
    if (!bongo_cat_log_enabled(category, priority)) return;
    if (log_mutex) SDL_LockMutex(log_mutex);
    char timestamp[64];
    bongo_cat_runtime_timestamp(timestamp, sizeof(timestamp));
    append_log(runtime_log_path, timestamp, category, priority, message);
    fprintf(stderr, "%s [%s] [%s:%d] %s\n", timestamp, log_source,
        priority_name(priority), category, message ? message : "");
    if (log_mutex) SDL_UnlockMutex(log_mutex);
}

/* 正常退出的收尾: 补一条"关闭完成"日志 (之后 log_mutex 会被销毁), 再把 runtime-state
   清掉。所以它必须发生在 bongo_cat_runtime_log_stop() 之前。 */
void bongo_cat_runtime_clean_shutdown(BongoCatApp *app, int exit_code) {
    char timestamp[64], message[80];
    bongo_cat_runtime_timestamp(timestamp, sizeof(timestamp));
    snprintf(message, sizeof(message),
        "[runtime] Shutdown complete: exit_code=%d", exit_code);
    append_log(runtime_log_path, timestamp, BONGO_CAT_LOG_LIFECYCLE,
        SDL_LOG_PRIORITY_INFO, message);
    fprintf(stderr, "%s [%s] [%s:%d] %s\n", timestamp, log_source,
        priority_name(SDL_LOG_PRIORITY_INFO),
        BONGO_CAT_LOG_LIFECYCLE, message);

    bongo_cat_runtime_state_clean(app, timestamp);
}

/* 先摘指针再销毁: 免得别的线程在销毁过程中继续往钩子里写。 */
void bongo_cat_runtime_log_stop(void) {
    if (!log_mutex) return;
    SDL_Mutex *mutex = log_mutex;
    log_mutex = NULL;
    SDL_DestroyMutex(mutex);
}

/*
 * 把当前会话日志改名成 BongoCat.previous.log (自动重启之前调用, 见
 * application_restart.c)。
 *
 * 为什么需要: 主进程每次启动都会把日志清空 (begin_log 里的 "wb"), 于是"上一次为什么
 * 重启"的证据会被新进程当场抹掉 —— 用户回传日志时只剩重启之后的半截。沿用已有的
 * previous 命名, 一次重启只保留最近一份, 不会无限堆积。
 * 日志是逐行 append(开-写-关) 的, 改名不会撞上被别人占用的句柄。
 */
void bongo_cat_runtime_log_preserve(void) {
    if (!runtime_log_path[0]) return;
    static const char suffix[] = ".log";
    size_t length = strlen(runtime_log_path);
    if (length <= sizeof(suffix) - 1 ||
        strcmp(runtime_log_path + length - (sizeof(suffix) - 1), suffix) != 0)
        return;
    size_t base = length - (sizeof(suffix) - 1);
    static const char previous_suffix[] = ".previous.log";
    char previous[BONGO_CAT_PATH_CAP];
    if (base + sizeof(previous_suffix) > sizeof(previous)) return;
    memcpy(previous, runtime_log_path, base);
    memcpy(previous + base, previous_suffix, sizeof(previous_suffix));
    bongo_cat_path_rename(runtime_log_path, previous);
}

static void set_log_source(const BongoCatApp *app) {
    if (app && app->secondary_pet)
        snprintf(log_source, sizeof(log_source), "pet:%s",
            app->secondary_model_id);
    else snprintf(log_source, sizeof(log_source), "primary");
    for (size_t i = 0; log_source[i]; ++i)
        if (log_source[i] == '\r' || log_source[i] == '\n')
            log_source[i] = ' ';
}

/* 日志机制自己必须能容错, 顺序也不能乱: 先读上一次的 runtime-state 与 startup-stage
   (要在覆盖它们之前读), 再 (仅主进程) 清空日志文件, 然后装钩子、压默认日志级别
   (只给 lifecycle/update/input 开 INFO), 最后把上次没走完的阶段/异常退出 WARN 出来。 */
static void begin_log(BongoCatApp *app) {
    char stage_path[BONGO_CAT_PATH_CAP];
    char stage_temporary[BONGO_CAT_PATH_CAP];
    char interrupted[128] = {0};
    char unexpected[256] = {0};
    bongo_cat_runtime_state_previous(app, unexpected, sizeof(unexpected));
    runtime_log_path[0] = '\0';
    bongo_cat_path_join(runtime_log_path, sizeof(runtime_log_path),
        app->log_root, "BongoCat.log");
    /* The primary process owns the session log. Secondary pets append to it
       instead of erasing diagnostics already written by the primary. */
    if (runtime_log_path[0] && !app->secondary_pet) {
        FILE *fresh_log = bongo_cat_file_open(runtime_log_path, "wb");
        if (fresh_log) fclose(fresh_log);
    }
    set_log_source(app);
    bool stage_paths_ready = bongo_cat_path_join(stage_path,
        sizeof(stage_path), app->state_root, "startup-stage.txt") &&
        bongo_cat_path_join(stage_temporary, sizeof(stage_temporary),
            app->state_root, "startup-stage.tmp");
    if (stage_paths_ready && !bongo_cat_path_is_file(stage_path) &&
        bongo_cat_path_is_file(stage_temporary))
        bongo_cat_file_replace(stage_temporary, stage_path, true);
    else if (stage_paths_ready) bongo_cat_file_remove(stage_temporary);
    FILE *stage = stage_paths_ready ? bongo_cat_file_open(stage_path, "rb")
        : NULL;
    if (stage) {
        size_t length = fread(interrupted, 1, sizeof(interrupted) - 1, stage);
        interrupted[ferror(stage) ? 0 : length] = '\0';
        fclose(stage);
    }
    if (!log_mutex) log_mutex = SDL_CreateMutex();
    SDL_SetLogOutputFunction(log_output, NULL);
#ifdef NDEBUG
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
    SDL_SetLogPriority(BONGO_CAT_LOG_LIFECYCLE, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(BONGO_CAT_LOG_UPDATE, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(BONGO_CAT_LOG_INPUT, SDL_LOG_PRIORITY_INFO);
#else
    /* Debug 构建把日志级别压到 DEBUG, 与 log.h 的过滤策略保持一致。 */
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#endif
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] Process started: version=%s platform=%s",
        BONGO_CAT_VERSION, SDL_GetPlatform());
#ifdef _WIN32
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[runtime] Package identity: %s",
        bongo_cat_windows_is_packaged() ? "MSIX" : "unpackaged Win32");
#endif
    if (interrupted[0]) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Previous startup ended before readiness at stage: %s", interrupted);
    if (unexpected[0] && !strstr(unexpected, "stage=clean-shutdown") &&
        /* 自动重启: 上一次是主动退出去换新实例, 关闭阶段被标成 shutdown:restart,
           这不是崩溃 (见 application_restart.c 与 app_entry.c)。 */
        !strstr(unexpected, "stage=shutdown:restart"))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Previous run ended unexpectedly (crash, forced close, or hang): %s",
            unexpected);
}

/*
 * "这一次到底把数据放在哪"必须在这里说, 不能在 storage_paths.c 里说: 那个函数跑在
 * begin_log 之前, 日志输出函数还没安装, 在那里调 SDL_Log 一个字都留不下 —— 而排查
 * "数据落到哪去了"时最需要的恰恰是这一行 (--storage-root 与便携模式两条路都覆盖)。
 */
static void log_storage_root(const BongoCatApp *app) {
    if (app->storage_root[0]) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 存储根来自 --storage-root=%s", app->storage_root);
        return;
    }
    char portable[BONGO_CAT_PATH_CAP];
    if (bongo_cat_portable_mode_active() &&
        bongo_cat_portable_mode_root(portable, sizeof(portable)))
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[portable] 使用便携存储根: %s", portable);
}

/* 把内嵌在 exe 里的渲染组件按需释放到 <存储根>/cores/。
 *
 * 位置很关键: 必须在 bongo_cat_renderer_module_scan() **之前** —— 释放的目的就是让现有的
 * 目录解析能看见组件, 所以这里不重写任何发现逻辑, 只是把文件摆到它要看的那个位置上。
 * 也必须在 begin_log() 之后, 否则"释放失败"这类只发生一次的事在日志里一个字都留不下。
 *
 * 失败一律只降级: 目录只读、目标被别的实例加载着、包里没有组件(无 SDK 的诊断构建) 都不是
 * 启动失败的理由 —— Live2D 走诊断兜底, 其余功能不受影响。 */
static void release_embedded_component(void) {
    char root[BONGO_CAT_PATH_CAP], directory[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_storage_paths_root(root, sizeof(root)) ||
        !bongo_cat_path_join(directory, sizeof(directory), root, "cores")) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 存储根未解析, 跳过内嵌渲染组件的释放");
        return;
    }
    BongoCatError component = {0};
    BongoCatResult result =
        bongo_cat_platform_embedded_assets_release_component(directory, &component);
    if (result == BONGO_CAT_OK || result == BONGO_CAT_ERROR_PLATFORM) return;
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 释放内嵌渲染组件失败, Live2D 将以诊断兜底继续: %s",
        component.message[0] ? component.message : "unknown reason");
}

/* 参数 -> 存储路径 -> 日志 -> Live2D 组件与 Core, 顺序不能反: 日志路径来自 log_root,
   而 log_root 可能被 --storage-root= 改掉; 组件/Core 的目录又来自存储根, 且它们的
   发现结果必须写进日志, 所以只能排在 begin_log 之后。
   两步都**失败也继续启动**: 缺 Core 或缺组件时程序照常跑, Live2D 走 live2d_stub.c 的
   兜底实现 —— 这是降级路径的核心, 不能让"没导入 Core"变成启动失败。 */
bool bongo_cat_startup_prepare(BongoCatApp *app, int argc, char **argv,
    BongoCatError *error) {
    if (!bongo_cat_startup_arguments(app, argc, argv, error) ||
        !bongo_cat_storage_paths_prepare(app, error)) return false;
    begin_log(app); log_storage_root(app);
    release_embedded_component();
    BongoCatRendererModuleInfo modules[8];
    bongo_cat_renderer_module_scan(modules,
        sizeof(modules) / sizeof(modules[0]));
    /* 目录服务要知道六棵树在哪, 而那只有 app 知道 —— 启动期绑定一次(退出时解绑)。
       组件装载发生在首次建实例时(那时才知道资源根), 所以绑定必须早于它。 */
    bongo_cat_renderer_module_bind_app(app);
    bongo_cat_live2d_core_initialize();
    bongo_cat_startup_stage(app, "paths-ready"); return true;
}

/* 写阶段标记。写失败只降级不影响启动 —— 它纯粹是诊断设施。 */
void bongo_cat_startup_stage(BongoCatApp *app, const char *stage) {
    if (!app || !app->state_root[0] || !stage) return;
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(path, sizeof(path), app->state_root,
        "startup-stage.txt") && bongo_cat_path_join(temporary,
        sizeof(temporary), app->state_root, "startup-stage.tmp")) {
        FILE *file = bongo_cat_file_open(temporary, "wb");
        bool written = file && fputs(stage, file) >= 0;
        if (file && fclose(file) != 0) written = false;
        if (!written || !bongo_cat_file_replace(temporary, path, true))
            bongo_cat_file_remove(temporary);
    }
    char runtime_stage[160];
    snprintf(runtime_stage, sizeof(runtime_stage), "startup:%s", stage);
    bongo_cat_runtime_stage(app, runtime_stage);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[runtime] Startup stage: %s", stage);
}

/* 幂等。主循环在第一次成功呈现后调用它: 也就是说"启动完成"的定义是"第一帧真的端出去
   了", 而不是"初始化返回了 true"。 */
void bongo_cat_startup_ready(BongoCatApp *app) {
    if (!app || startup_is_ready) return;
    startup_is_ready = true;
    char path[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(path, sizeof(path), app->state_root,
        "startup-stage.txt")) bongo_cat_file_remove(path);
    if (bongo_cat_path_join(path, sizeof(path), app->state_root,
        "startup-stage.tmp")) bongo_cat_file_remove(path);
    bongo_cat_runtime_stage(app, "running");
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[runtime] Startup ready");
}

/* 启动失败: 留下 failed 阶段 (下次启动能看到上次是失败退出的), 然后打日志。 */
void bongo_cat_startup_failure(BongoCatApp *app, const BongoCatError *error) {
    const char *message = error && error->message[0] ? error->message : "Initialization failed";
    if (app) bongo_cat_startup_stage(app, "failed");
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Startup failed: %s", message);
}

/* CI 失败会置 exit_code=1, 但**不**中断启动: 让进程照常跑到 smoke deadline, 好把剩下
   的诊断全部写完。 */
void bongo_cat_startup_ci_failure(BongoCatApp *app,
    const BongoCatError *error) {
    if (!app) return;
    app->exit_code = 1;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[runtime] CI failure: %s",
        error && error->message[0] ? error->message : "CI operation failed");
}
