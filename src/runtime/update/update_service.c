/*
 * 更新检查服务的状态机与生命周期: 工作线程只负责网络抓取、JSON 解析和版本比较,
 * 结论写回 service 后用自定义 SDL 事件唤醒主线程做界面提示与会话落盘。
 * 锁的约定: mutex 保护 status/release/worker/error 等对外可见字段, http_mutex 只保护
 * 取消标志与在飞的网络句柄 —— 两把锁不要嵌套获取。
 * 节流: 自动检查一天一次且同一 BONGO_CAT_VERSION 只查一遍, 手动检查不受限制。
 * 回收线程的时机 (reap_worker) 最容易改错, 见其注释。
 */
#include "update_internal.h"
#include "preferences_notice.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELEASES_URL \
    "https://github.com/vladelaina/BongoCat/releases/latest"
#define STORE_URI "ms-windows-store://pdp/?ProductId=9P41MLSX72XW"
#define STORE_WEB_URL "https://apps.microsoft.com/detail/9P41MLSX72XW"

/* 回收已经结束的工作线程。只有 status 不再是 CHECKING 时才认为它可以 join, 否则保持
 * 不动 —— 绝不能在主线程上等待一个还在收包的线程, 那会卡住整个事件循环, 所以空转正常。 */
static void reap_worker(BongoCatUpdateService *service) {
    SDL_LockMutex(service->mutex);
    SDL_Thread *worker = service->status != BONGO_CAT_UPDATE_CHECKING
        ? service->worker : NULL;
    if (worker) service->worker = NULL;
    SDL_UnlockMutex(service->mutex);
    if (worker) SDL_WaitThread(worker, NULL);
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
 * release 为 NULL 且不是 ERROR 时清空上次的版本; ERROR 刻意保留它, 这样查到过更新、
 * 但这次检查失败时界面仍显示"更新到 vX", 而不是退回"检查更新"。 */
static void complete(BongoCatUpdateService *service,
    BongoCatUpdateStatus status, const BongoCatUpdateRelease *release,
    const char *error) {
    SDL_LockMutex(service->mutex);
    bool notify = !service->shutting_down;
    if (notify) {
        service->status = status;
        if (release) service->release = *release;
        else if (status != BONGO_CAT_UPDATE_ERROR)
            memset(&service->release, 0, sizeof(service->release));
        snprintf(service->error, sizeof(service->error), "%s",
            error ? error : "");
    }
    SDL_UnlockMutex(service->mutex);
    if (!notify) return;
    if (status == BONGO_CAT_UPDATE_ERROR)
        SDL_LogError(BONGO_CAT_LOG_UPDATE, "Update check failed: %s",
            error && error[0] ? error : "unknown error");
    else SDL_LogInfo(BONGO_CAT_LOG_UPDATE, "Update check: %s version=%s",
        status == BONGO_CAT_UPDATE_AVAILABLE ? "available" : "current",
        release ? release->version : BONGO_CAT_VERSION);
    SDL_Event event = {0};
    event.type = service->event_type;
    event.user.data1 = service;
    SDL_PushEvent(&event);
}

/* 工作线程主体: 抓取 → 解析 → 版本比较, 全程不碰界面与配置。
 * 被取消时连事件都不发 (静默退出); 其余失败一律压成 ERROR 加一句 error 文案。
 * 只有远端版本严格大于本地 BONGO_CAT_VERSION 才算有更新, 相等、更旧或无法解析都按已是最新。 */
static int SDLCALL update_worker(void *userdata) {
    BongoCatUpdateService *service = userdata;
    char *response = NULL;
    char message[256] = {0};
    BongoCatUpdateFetchResult fetched = bongo_cat_update_http_fetch(service,
        &response, message, sizeof(message));
    if (fetched != BONGO_CAT_UPDATE_FETCH_OK) {
        if (fetched != BONGO_CAT_UPDATE_FETCH_CANCELLED)
            complete(service, BONGO_CAT_UPDATE_ERROR, NULL, message);
        return 0;
    }
    BongoCatUpdateRelease release;
    BongoCatError error = {0};
    bool parsed = bongo_cat_update_parse_release(response,
        bongo_cat_update_platform_asset(), &release, &error);
    free(response);
    if (!parsed) {
        complete(service, BONGO_CAT_UPDATE_ERROR, NULL, error.message);
        return 0;
    }
    BongoCatUpdateStatus status = bongo_cat_update_compare_versions(
        release.version, BONGO_CAT_VERSION) > 0
        ? BONGO_CAT_UPDATE_AVAILABLE : BONGO_CAT_UPDATE_CURRENT;
    complete(service, status, &release, NULL);
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
    }
    return service;
}

/* 发起一次检查; manual 决定结束后是否弹提示 (自动检查静默)。
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
    SDL_LockMutex(service->http_mutex);
    service->http_cancelled = false;
    SDL_UnlockMutex(service->http_mutex);
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE, "Update check started");
    service->worker = SDL_CreateThread(update_worker,
        BONGO_CAT_SLUG "-update-check", service);
    bool started = service->worker != NULL;
    if (!started) {
        service->status = BONGO_CAT_UPDATE_ERROR;
        SDL_LogError(BONGO_CAT_LOG_UPDATE, "Cannot start the update checker");
        snprintf(service->error, sizeof(service->error),
            "Cannot start the update checker");
    }
    SDL_UnlockMutex(service->mutex);
    return started;
}

/* 界面按钮语义: 记住"这次检查完若有更新就直接打开下载页"; 检查没发起成功就撤销这个意图。 */
bool bongo_cat_update_refresh_and_open(BongoCatUpdateService *service) {
    if (!service) return false;
    SDL_LockMutex(service->mutex);
    service->open_after_check = true;
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
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", service->error);
    SDL_UnlockMutex(service->mutex);
}

static const char *tr(BongoCatUpdateService *service, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(service->app->i18n, key, fallback);
}

/* 主线程事件处理: 事件类型与 data1 都要匹配 (别人也可能注册 SDL 事件), 不匹配返回 false。
 * 先在锁内取走结论, 再到锁外做界面与落盘动作, 避免长时间持锁。
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
    BongoCatUpdateRelease release = service->release;
    SDL_UnlockMutex(service->mutex);
    reap_worker(service);
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
        if (snapshot.installed && snapshot.release.installer_url[0])
            url = snapshot.release.installer_url;
        else if (!snapshot.installed && snapshot.release.portable_url[0])
            url = snapshot.release.portable_url;
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
