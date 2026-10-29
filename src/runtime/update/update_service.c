/*
 * 更新检查与一键更新的状态机与生命周期: 工作线程负责网络抓取、JSON 解析、版本比较,
 * 以及"下载 -> 校验"; 结论写回 service 后用自定义 SDL 事件唤醒主线程做界面提示、会话落盘,
 * 以及替换/重启的最后一步 (替换只能在主线程: 它要拉起助手进程并让本进程退出)。
 * 锁的约定: mutex 保护 status/release/error/进度等对外可见字段, http_mutex 只保护
 * 取消标志与在飞的网络句柄 —— 两把锁不要嵌套获取。
 * 节流: 自动检查一天一次且同一 BONGO_CAT_VERSION 只查一遍, 手动检查不受限制。
 * 回收线程的时机 (reap_worker) 最容易改错, 见其注释。
 * 自动下载只由"用户点击检查更新"触发: 自动检查永远停在 AVAILABLE, 只放角标;
 * --ci-* / 冒烟运行连下载都不进 (CI 只能检查, 不能动二进制)。
 */
#include "update_internal.h"
#include "update_install.h"
#include "preferences_notice.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELEASES_URL \
    "https://github.com/Fid-falling/bongocat-turbo/releases/latest"
#define STORE_URI "ms-windows-store://pdp/?ProductId=9P41MLSX72XW"
#define STORE_WEB_URL "https://apps.microsoft.com/detail/9P41MLSX72XW"

/* 回收已经结束的工作线程。判据是 worker_done 而不是"status 不再是 CHECKING":
 * 下载阶段的状态同样不是 CHECKING, 但线程还在跑, 主线程一旦据此 join 就会卡住整个
 * 事件循环 (下载可能持续几十秒)。所以: 只有线程自己置了 worker_done 才 join。 */
static void reap_worker(BongoCatUpdateService *service) {
    SDL_LockMutex(service->mutex);
    SDL_Thread *worker = service->worker_done ? service->worker : NULL;
    if (worker) service->worker = NULL;
    SDL_UnlockMutex(service->mutex);
    if (worker) SDL_WaitThread(worker, NULL);
}

/* 工作线程的交卷动作 (每个返回路径都必须调用): 置位之后主线程才允许 join。 */
static void finish_worker(BongoCatUpdateService *service) {
    SDL_LockMutex(service->mutex);
    service->worker_done = true;
    SDL_UnlockMutex(service->mutex);
}

/* 本地日期压成 yyyymmdd; 时间 API 失败返回 0, 调用方视为"未知" (不比较、不写入)。 */
static int local_day(void) {
    SDL_Time timestamp = 0;
    SDL_DateTime local = {0};
    if (!SDL_GetCurrentTime(&timestamp) ||
        !SDL_TimeToDateTime(timestamp, &local, true)) return 0;
    return local.year * 10000 + local.month * 100 + local.day;
}

/* 工作线程唯一的"交卷"入口: 锁内更新状态, 锁外写日志并发事件; shutting_down 之后
 * 状态与事件都不再发出 (那时 destroy 正在等线程结束, 事件已无人处理)。
 * release 为 NULL 时保留上一次的版本号 —— 检查到过更新、但下载阶段失败时, 界面仍要显示
 * "可更新到 vX" 的角标, 而不是退回"检查更新"。 */
static void complete(BongoCatUpdateService *service,
    BongoCatUpdateStatus status, const BongoCatUpdateRelease *release,
    const char *error) {
    SDL_LockMutex(service->mutex);
    bool notify = !service->shutting_down;
    if (notify) {
        service->status = status;
        if (release) service->release = *release;
        snprintf(service->error, sizeof(service->error), "%s",
            error ? error : "");
    }
    SDL_UnlockMutex(service->mutex);
    if (!notify) return;
    switch (status) {
    case BONGO_CAT_UPDATE_ERROR:
        SDL_LogError(BONGO_CAT_LOG_UPDATE, "Update failed: %s",
            error && error[0] ? error : "unknown error");
        break;
    case BONGO_CAT_UPDATE_DOWNLOADING:
    case BONGO_CAT_UPDATE_VERIFYING:
    case BONGO_CAT_UPDATE_INSTALLING:
        SDL_LogInfo(BONGO_CAT_LOG_UPDATE, "Update stage: %u", (unsigned)status);
        break;
    default:
        SDL_LogInfo(BONGO_CAT_LOG_UPDATE, "Update check: %s version=%s",
            status == BONGO_CAT_UPDATE_AVAILABLE ? "available" : "current",
            release ? release->version : BONGO_CAT_VERSION);
        break;
    }
    SDL_Event event = {0};
    event.type = service->event_type;
    event.user.data1 = service;
    SDL_PushEvent(&event);
}

/* 阶段切换: 只改状态 + 发事件, 不清空版本号 (complete 的语义是"终态交卷")。 */
void bongo_cat_update_report_stage(BongoCatUpdateService *service,
    BongoCatUpdateStatus status) {
    if (!service) return;
    complete(service, status, NULL, NULL);
}

/* 下载进度: 由下载循环按块调用 (几十 KB 一次, 不会成为锁热点); 界面每帧取一次快照。 */
void bongo_cat_update_report_progress(BongoCatUpdateService *service,
    uint64_t downloaded, uint64_t total) {
    if (!service) return;
    SDL_LockMutex(service->mutex);
    service->downloaded = downloaded;
    service->total = total;
    SDL_UnlockMutex(service->mutex);
}

/* 工作线程侧读取消位 (粘滞标志, 只有下一次 check 才清零)。 */
bool bongo_cat_update_cancelled(BongoCatUpdateService *service) {
    if (!service || !service->http_mutex) return true;
    SDL_LockMutex(service->http_mutex);
    bool value = service->http_cancelled;
    SDL_UnlockMutex(service->http_mutex);
    return value;
}

/*
 * 工作线程主体: 抓取 -> 解析 -> 版本比较 -> (仅手动检查) 下载 -> 校验 -> 请求替换。
 * 全程不碰界面与配置; 被取消时连事件都不发 (静默退出)。
 * 只有远端版本严格大于本地 BONGO_CAT_VERSION 才算有更新, 相等、更旧或无法解析都按已是最新。
 */
static int SDLCALL update_worker(void *userdata) {
    BongoCatUpdateService *service = userdata;
    char message[256] = {0};
    char *response = NULL;
    BongoCatUpdateRelease release;
    BongoCatError error = {0};
    BongoCatUpdateFetchResult fetched = bongo_cat_update_http_fetch(service,
        &response, message, sizeof(message));
    bool parsed = fetched == BONGO_CAT_UPDATE_FETCH_OK &&
        bongo_cat_update_parse_release(response,
            bongo_cat_update_platform_asset(), &release, &error);
    free(response);
    if (fetched != BONGO_CAT_UPDATE_FETCH_OK) {
        if (fetched != BONGO_CAT_UPDATE_FETCH_CANCELLED)
            complete(service, BONGO_CAT_UPDATE_ERROR, NULL, message);
        finish_worker(service);
        return 0;
    }
    if (!parsed) {
        complete(service, BONGO_CAT_UPDATE_ERROR, NULL, error.message);
        finish_worker(service);
        return 0;
    }
    if (bongo_cat_update_compare_versions(release.version, BONGO_CAT_VERSION) <= 0) {
        complete(service, BONGO_CAT_UPDATE_CURRENT, &release, NULL);
        finish_worker(service);
        return 0;
    }
    /*
     * 有更新。继续下载需要同时满足三条, 缺一不可:
     *   - 这次是"用户点了检查更新"(manual): 自动检查只放角标, 绝不静默下载/替换;
     *   - 平台支持原地替换自己 (目前只有 Windows);
     *   - 不是无人值守运行 (--ci-* / 冒烟): CI 只能检查, 不能动二进制。
     * 任何一条不满足就停在 AVAILABLE, 界面照旧提供"打开下载页"的出路。
     */
    bool unattended = service->app &&
        (service->app->smoke || service->app->automated);
    bool install = service->manual && !unattended &&
        bongo_cat_update_platform_self_update() && release.portable.url[0];
    if (!install) {
        complete(service, BONGO_CAT_UPDATE_AVAILABLE, &release, NULL);
        finish_worker(service);
        return 0;
    }
    SDL_LockMutex(service->mutex);
    service->release = release;
    SDL_UnlockMutex(service->mutex);
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
        "One-click update started for v%s (asset %s)", release.version,
        release.portable.name);
    if (!bongo_cat_update_download_asset(service, message, sizeof(message))) {
        if (!bongo_cat_update_cancelled(service))
            complete(service, BONGO_CAT_UPDATE_ERROR, NULL, message);
        finish_worker(service);
        return 0;
    }
    /* 文件已通过校验: 请主线程执行替换与重启 (它要拉起助手进程并让本进程退出)。 */
    SDL_LockMutex(service->mutex);
    service->install_requested = true;
    SDL_UnlockMutex(service->mutex);
    complete(service, BONGO_CAT_UPDATE_INSTALLING, NULL, NULL);
    finish_worker(service);
    return 0;
}

/* 一次性初始化: 保护状态的 mutex、保护取消与句柄的 http_mutex、一个自定义事件类型;
 * 任一失败就整体返回 NULL, 调用方降级为"没有更新检查" (只记一条警告)。
 * 初始状态由平台能力决定: 不支持 → UNSUPPORTED, 商店包 → STORE, 其余进入可检查的 IDLE;
 * 走 GitHub 通道时恢复上次会话存的 available_update_version (仍比本地新才算, 否则清掉),
 * 让冷启动第一帧就能在关于页显示更新角标。 */
BongoCatUpdateService *bongo_cat_update_create(BongoCatApp *app) {
    if (!app) return NULL;
    BongoCatUpdateService *service = calloc(1, sizeof(*service));
    if (!service) return NULL;
    service->app = app;
    service->mutex = SDL_CreateMutex();
    service->http_mutex = SDL_CreateMutex();
    service->event_type = SDL_RegisterEvents(1);
    if (!service->mutex || !service->http_mutex ||
        service->event_type == (Uint32)-1) {
        if (service->mutex) SDL_DestroyMutex(service->mutex);
        if (service->http_mutex) SDL_DestroyMutex(service->http_mutex);
        free(service);
        return NULL;
    }
    if (!bongo_cat_update_platform_supported())
        service->status = BONGO_CAT_UPDATE_UNSUPPORTED;
    else if (bongo_cat_update_platform_store())
        service->status = BONGO_CAT_UPDATE_STORE;
    else {
        service->status = BONGO_CAT_UPDATE_IDLE;
        service->installed = bongo_cat_update_platform_installed();
        if (app->session.available_update_version[0] &&
            bongo_cat_update_compare_versions(
                app->session.available_update_version,
                BONGO_CAT_VERSION) > 0) {
            snprintf(service->release.version,
                sizeof(service->release.version), "%s",
                app->session.available_update_version);
            service->status = BONGO_CAT_UPDATE_AVAILABLE;
        } else {
            app->session.available_update_version[0] = '\0';
        }
        /* 源列表在这里先载入一次 (界面第一帧就能显示"共几个源"), 下载前还会重读一次。
           启动收尾放在应用初始化之后: 能走到这里就说明新版本真的起来了, .old 可以删了。 */
        bongo_cat_update_sources_load(service);
        bongo_cat_update_install_startup(service);
    }
    return service;
}

/* 发起一次检查; manual 决定两件事: 结束后是否弹提示, 以及发现新版本后是否继续自动下载
 * (只有用户点的那一次会下载, 见 update_worker)。
 * 返回 false 表示本次没发起 (重入、正在关闭、STORE/UNSUPPORTED, 或线程创建失败) ——
 * 调用方据此提示用户, check 自身不碰界面; 每次开始都清掉取消位并先回收上一轮线程。 */
bool bongo_cat_update_check(BongoCatUpdateService *service, bool manual) {
    if (!service) return false;
    reap_worker(service);
    SDL_LockMutex(service->mutex);
    if (service->shutting_down || service->worker ||
        service->status == BONGO_CAT_UPDATE_STORE ||
        service->status == BONGO_CAT_UPDATE_UNSUPPORTED) {
        SDL_UnlockMutex(service->mutex);
        return false;
    }
    service->status = BONGO_CAT_UPDATE_CHECKING;
    service->manual = manual;
    service->error[0] = '\0';
    service->worker_done = false;
    service->install_requested = false;
    service->manual_replace = false;
    service->manual_only = false;
    service->manual_path[0] = '\0';
    service->downloaded = service->total = 0;
    service->source_index = service->source_count = 0;
    service->source_label[0] = '\0';
    SDL_LockMutex(service->http_mutex);
    service->http_cancelled = false;
    SDL_UnlockMutex(service->http_mutex);
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE, "Update check started (%s)",
        manual ? "manual" : "automatic");
    service->worker = SDL_CreateThread(update_worker,
        BONGO_CAT_SLUG "-update-check", service);
    bool started = service->worker != NULL;
    if (!started) {
        service->status = BONGO_CAT_UPDATE_ERROR;
        service->worker_done = true;
        SDL_LogError(BONGO_CAT_LOG_UPDATE, "Cannot start the update checker");
        snprintf(service->error, sizeof(service->error),
            "Cannot start the update checker");
    }
    SDL_UnlockMutex(service->mutex);
    return started;
}

/* 界面按钮语义: 用户点"检查更新"/"更新至 vX"时的唯一入口。
 * 支持原地替换的平台 (Windows) 上, 这次检查发现新版本后会继续自动下载并替换重启, 所以
 * 不再打开浏览器; 其它平台保持老行为 —— 检查完若有更新就打开下载页。
 * 检查没发起成功就撤销这个意图。 */
bool bongo_cat_update_start(BongoCatUpdateService *service) {
    if (!service) return false;
    SDL_LockMutex(service->mutex);
    service->open_after_check = !bongo_cat_update_platform_self_update();
    SDL_UnlockMutex(service->mutex);
    if (bongo_cat_update_check(service, true)) return true;
    SDL_LockMutex(service->mutex);
    service->open_after_check = false;
    SDL_UnlockMutex(service->mutex);
    return false;
}

/* 启动时调用一次 (见 runtime/lifecycle/app.c): smoke 与次要宠物进程直接跳过。
 * 节流: 同一天加同一 BONGO_CAT_VERSION 已经查过就不再查 —— 版本升级后允许当天再查一次。
 * 日期未知 (local_day 返回 0) 时不节流, 宁可多查一次也不要永远不查。 */
void bongo_cat_update_start_automatic(BongoCatUpdateService *service) {
    if (!service || !service->app || service->app->smoke ||
        service->app->secondary_pet) return;
    int today = local_day();
    BongoCatSessionState *session = &service->app->session;
    if (today && session->last_update_check_day == today &&
        strcmp(session->last_update_check_version, BONGO_CAT_VERSION) == 0)
        return;
    if (!bongo_cat_update_check(service, false)) return;
}

/* 给界面的快照: 复制到调用方栈上再使用, 免得渲染期间一直持锁; service 为 NULL 视为不支持。 */
void bongo_cat_update_snapshot(BongoCatUpdateService *service,
    BongoCatUpdateSnapshot *snapshot) {
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!service) {
        snapshot->status = BONGO_CAT_UPDATE_UNSUPPORTED;
        return;
    }
    SDL_LockMutex(service->mutex);
    snapshot->status = service->status;
    snapshot->release = service->release;
    snapshot->installed = service->installed;
    snapshot->downloaded = service->downloaded;
    snapshot->total = service->total;
    snapshot->source_index = service->source_index;
    snapshot->source_count = service->source_count;
    snapshot->manual_replace = service->manual_replace;
    snapshot->manual = service->manual;
    snprintf(snapshot->source, sizeof(snapshot->source), "%s",
        service->source_label);
    snprintf(snapshot->manual_path, sizeof(snapshot->manual_path), "%s",
        service->manual_path);
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", service->error);
    SDL_UnlockMutex(service->mutex);
}

static const char *tr(BongoCatUpdateService *service, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(service->app->i18n, key, fallback);
}

/* 主线程事件处理: 事件类型与 data1 都要匹配 (别人也可能注册 SDL 事件), 不匹配返回 false。
 * 先在锁内取走结论, 再到锁外做界面与落盘动作, 避免长时间持锁。
 * 三种事件: 阶段 (下载中/校验中) 只要重画; INSTALLING 且带 install_requested 才去执行
 * 替换与重启; 其余是终态, 走会话落盘 + 提示。
 * 只有自动检查成功 (CURRENT/AVAILABLE) 才记节流用的日期与版本, 手动检查不写;
 * available_update_version 写进 session 供下次冷启动直接显示, 随后让关于页重画。 */
bool bongo_cat_update_event(BongoCatUpdateService *service,
    const SDL_Event *event) {
    if (!service || !event || event->type != service->event_type ||
        event->user.data1 != service) return false;
    SDL_LockMutex(service->mutex);
    bool manual = service->manual;
    BongoCatUpdateStatus status = service->status;
    bool open_after_check = service->open_after_check;
    service->open_after_check = false;
    bool install_requested = service->install_requested;
    service->install_requested = false;
    BongoCatUpdateRelease release = service->release;
    SDL_UnlockMutex(service->mutex);
    reap_worker(service);
    /* 阶段事件: 落盘与提示都不做, 那些是终态的事; 版本号也保留 (角标要一直亮着)。 */
    if (status == BONGO_CAT_UPDATE_DOWNLOADING ||
        status == BONGO_CAT_UPDATE_VERIFYING) {
        bongo_cat_preferences_invalidate(service->app->preferences);
        return true;
    }
    if (status == BONGO_CAT_UPDATE_INSTALLING) {
        /* 校验通过的交接点: 主线程来执行替换 (拉起助手进程 + 安排本进程退出)。 */
        if (install_requested) bongo_cat_update_install_request(service);
        else bongo_cat_preferences_invalidate(service->app->preferences);
        return true;
    }
    if ((status == BONGO_CAT_UPDATE_AVAILABLE ||
            status == BONGO_CAT_UPDATE_ERROR) && release.version[0])
        snprintf(service->app->session.available_update_version,
            sizeof(service->app->session.available_update_version), "%s",
            release.version);
    else if (status == BONGO_CAT_UPDATE_CURRENT)
        service->app->session.available_update_version[0] = '\0';
    if (!manual && (status == BONGO_CAT_UPDATE_CURRENT ||
            status == BONGO_CAT_UPDATE_AVAILABLE)) {
        int today = local_day();
        if (today) {
            BongoCatSessionState *session = &service->app->session;
            session->last_update_check_day = today;
            snprintf(session->last_update_check_version,
                sizeof(session->last_update_check_version), "%s",
                BONGO_CAT_VERSION);
        }
    }
    bongo_cat_preferences_invalidate(service->app->preferences);
    if (open_after_check && status == BONGO_CAT_UPDATE_AVAILABLE) {
        if (!bongo_cat_update_open(service)) {
            char detail[384];
            const char *reason = SDL_GetError();
            snprintf(detail, sizeof(detail), "%s: %s", tr(service,
                "native.support.openUpdateFailed",
                "Unable to open the update page"),
                reason && reason[0] ? reason : "system rejected the URL");
            bongo_cat_preferences_notice_show(service->app, detail, true);
        }
    } else if (manual) bongo_cat_update_show_completion(service);
    return true;
}

/* 打开更新页面: STORE 状态先试 ms-windows-store: 协议, 失败再退到网页商店;
 * 其余状态按 installed 选安装包或便携包, 都没有就退到 releases 页, 返回 SDL_OpenURL 的结果。 */
bool bongo_cat_update_open(BongoCatUpdateService *service) {
    BongoCatUpdateSnapshot snapshot;
    bongo_cat_update_snapshot(service, &snapshot);
    if (snapshot.status == BONGO_CAT_UPDATE_STORE)
        return SDL_OpenURL(STORE_URI) || SDL_OpenURL(STORE_WEB_URL);
    const char *url = RELEASES_URL;
    if (snapshot.status == BONGO_CAT_UPDATE_AVAILABLE) {
        if (snapshot.installed && snapshot.release.installer.url[0])
            url = snapshot.release.installer.url;
        else if (!snapshot.installed && snapshot.release.portable.url[0])
            url = snapshot.release.portable.url;
        else if (snapshot.release.release_url[0])
            url = snapshot.release.release_url;
    }
    return SDL_OpenURL(url);
}

/* 关停顺序不能换: 先置 shutting_down 让工作线程不再发事件, 再取消网络请求把阻塞的收包
 * 打断, 然后 join, 最后把队列里残留的事件吃掉 —— 那些事件的 user.data1 指向即将被
 * free 的 service。若不先取消就 join, 退出时最长要等一整个网络超时。 */
void bongo_cat_update_destroy(BongoCatUpdateService *service) {
    if (!service) return;
    SDL_LockMutex(service->mutex);
    service->shutting_down = true;
    SDL_UnlockMutex(service->mutex);
    bongo_cat_update_http_cancel(service);
    if (service->worker) SDL_WaitThread(service->worker, NULL);
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, service->event_type,
        service->event_type) > 0) {}
    SDL_DestroyMutex(service->http_mutex);
    SDL_DestroyMutex(service->mutex);
    free(service);
}
