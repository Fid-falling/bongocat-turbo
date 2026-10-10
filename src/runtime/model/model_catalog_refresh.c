/* 模型目录的后台刷新：整表扫描与单个包的重建都在独立 worker 线程里跑，完成后用自定义
 * SDL 事件回到主线程应用结果。主循环每帧可以调 bongo_cat_model_refresh_update() 取结果，
 * 也能由 bongo_cat_model_refresh_event() 在事件循环里提前触发。核心契约：worker 只在堆上
 * 那份临时 BongoCatApp 与自己的目录副本上工作，app->models 永远只在主线程被改写；结果只有
 * 在 job->revision == refresh->revision 时才被采纳，期间目录被改过就丢弃并重跑。最易改错
 * 的地方是 worker 侧误碰窗口与渲染字段，以及 busy 与 completed 的生命周期。 */
#include "runtime.h"
#include "model_import.h"
#include "bongo_cat/preferences.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 一次刷新任务的全部输入与输出：输入是启动时从主线程按值拷下来的路径与设置快照（worker
 * 不允许回头读 BongoCatApp），输出是扫描后的完整目录副本。字符串全部拷贝，因为 app 的字
 * 段可能在工作期间被主线程改动或失效。 */
typedef struct BongoCatModelRefreshJob {
    struct BongoCatModelRefresh *owner;
    BongoCatModelCatalog models;
    char asset_root[BONGO_CAT_PATH_CAP];
    char models_root[BONGO_CAT_PATH_CAP];
    char cache_root[BONGO_CAT_PATH_CAP];
    char nearby_root[BONGO_CAT_PATH_CAP];
    char active_model_id[BONGO_CAT_ID_CAP];
    char package_id[BONGO_CAT_ID_CAP];
    BongoCatRemovedModel removed_models[BONGO_CAT_MODEL_CAP];
    size_t removed_model_count;
    uint64_t started_ns;
    uint64_t revision;
    BongoCatError error;
    bool success;
} BongoCatModelRefreshJob;

/* 长驻的刷新状态，挂在 app->model_refresh 上。mutex 只保护 completed 这一个指针（worker
 * 发布结果、主线程取走结果），其余字段（busy、rerun_*、pending_*、revision）只在主线程
 * 读写。 */
struct BongoCatModelRefresh {
    SDL_Mutex *mutex;
    SDL_Thread *worker;
    BongoCatModelRefreshJob *completed;
    Uint32 event_type;
    uint64_t revision;
    bool busy;
    bool active_nearby;
    bool rerun_full;
    bool rerun_nearby;
    size_t pending_package_count;
    char pending_packages[BONGO_CAT_MODEL_CAP][BONGO_CAT_ID_CAP];
};

/* 懒创建：分配或 SDL 初始化失败返回 NULL，调用方降级成没有后台刷新（同步路径照常）。
 * SDL_RegisterEvents 注册的事件类型无法注销，所以失败路径只回收 mutex。 */
static BongoCatModelRefresh *create_refresh(void) {
    BongoCatModelRefresh *refresh = calloc(1, sizeof(*refresh));
    if (!refresh) return NULL;
    refresh->mutex = SDL_CreateMutex();
    refresh->event_type = SDL_RegisterEvents(1);
    if (!refresh->mutex || refresh->event_type == (Uint32)-1) {
        if (refresh->mutex) SDL_DestroyMutex(refresh->mutex);
        free(refresh);
        return NULL;
    }
    return refresh;
}

/* worker 入口。这里刻意不直接用 app：它构造一个只填了扫描所需字段（asset/models/cache
 * 根目录、active_model_id、removed_models 快照、可选目录副本）的临时 BongoCatApp，扫描代
 * 码只能依赖这些字段，不能碰窗口与渲染。结束时先在锁内发布 completed 再推事件，主线程据
 * 此 join 线程；顺序反了主线程会读到空结果。 */
static int SDLCALL refresh_worker(void *userdata) {
    BongoCatModelRefreshJob *job = userdata;
    if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot lower model catalog thread priority: %s", SDL_GetError());
        SDL_ClearError();
    }
    BongoCatApp *scan = calloc(1, sizeof(*scan));
    if (scan) {
        snprintf(scan->asset_root, sizeof(scan->asset_root), "%s",
            job->asset_root);
        snprintf(scan->models_root, sizeof(scan->models_root), "%s",
            job->models_root);
        snprintf(scan->cache_root, sizeof(scan->cache_root), "%s",
            job->cache_root);
        snprintf(scan->session.active_model_id,
            sizeof(scan->session.active_model_id), "%s",
            job->active_model_id);
        scan->settings.removed_model_count = job->removed_model_count;
        memcpy(scan->settings.removed_models, job->removed_models,
            job->removed_model_count * sizeof(job->removed_models[0]));
        scan->models = job->models;
        BongoCatResult result = BONGO_CAT_OK;
        if (job->package_id[0])
            result = bongo_cat_import_installed_package(scan,
                job->models_root, job->package_id, &job->error);
        else result = bongo_cat_model_catalog_scan(scan, false,
            job->nearby_root);
        job->models = scan->models;
        job->success = result == BONGO_CAT_OK;
        free(scan);
    }

    BongoCatModelRefresh *refresh = job->owner;
    SDL_LockMutex(refresh->mutex);
    refresh->completed = job;
    SDL_UnlockMutex(refresh->mutex);

    SDL_Event event = {0};
    event.type = refresh->event_type;
    event.user.data1 = refresh;
    SDL_PushEvent(&event);
    return 0;
}

/* 启动 worker。busy 必须在 SDL_CreateThread 之前置位，否则线程可能在主线程读到 busy 之
 * 前就跑完，完成回调会被当成无任务而丢掉。package_id 非空时把主线程目录整表拷进任务
 * （增量包刷新要在现有条目上合并），为空时留零表让扫描自己重建。 */
static bool start_refresh(BongoCatApp *app, bool include_nearby,
    const char *package_id) {
    BongoCatModelRefresh *refresh = app->model_refresh;
    BongoCatModelRefreshJob *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->owner = refresh;
    job->started_ns = SDL_GetTicksNS();
    job->revision = refresh->revision;
    if (package_id) job->models = app->models;
    snprintf(job->asset_root, sizeof(job->asset_root), "%s", app->asset_root);
    snprintf(job->models_root, sizeof(job->models_root), "%s", app->models_root);
    snprintf(job->cache_root, sizeof(job->cache_root), "%s", app->cache_root);
    if (include_nearby)
        snprintf(job->nearby_root, sizeof(job->nearby_root), "%s",
            app->nearby_root);
    if (package_id)
        snprintf(job->package_id, sizeof(job->package_id), "%s", package_id);
    snprintf(job->active_model_id, sizeof(job->active_model_id), "%s",
        app->session.active_model_id);
    job->removed_model_count = app->settings.removed_model_count;
    if (job->removed_model_count > BONGO_CAT_MODEL_CAP)
        job->removed_model_count = BONGO_CAT_MODEL_CAP;
    memcpy(job->removed_models, app->settings.removed_models,
        job->removed_model_count * sizeof(job->removed_models[0]));
    refresh->busy = true;
    refresh->active_nearby = include_nearby;
    refresh->worker = SDL_CreateThread(refresh_worker,
        BONGO_CAT_SLUG "-model-catalog", job);
    if (refresh->worker) return true;
    refresh->busy = false;
    free(job);
    return false;
}

/* 请求合并策略：busy 时不能再起第二个 worker，于是把请求折算成重跑标记或包刷新队列。整
 * 表重扫优先于包刷新（置 rerun_full 会清空队列）；队列按 id 去重，满了也升级成整表重扫
 * —— 宁可多扫一次，也不能漏掉用户刚导入的模型。非 busy 时直接启动。 */
static void request_refresh(BongoCatApp *app, bool include_nearby,
    const char *package_id) {
    if (!app) return;
    if (!app->model_refresh) app->model_refresh = create_refresh();
    BongoCatModelRefresh *refresh = app->model_refresh;
    if (!refresh) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Background model refresh is unavailable: %s", SDL_GetError());
        return;
    }
    if (refresh->busy) {
        if (!package_id || include_nearby) {
            refresh->rerun_full = true;
            refresh->rerun_nearby = refresh->rerun_nearby || include_nearby;
            refresh->pending_package_count = 0;
            return;
        }
        if (refresh->rerun_full) return;
        for (size_t i = 0; i < refresh->pending_package_count; ++i)
            if (!strcmp(refresh->pending_packages[i], package_id)) return;
        if (refresh->pending_package_count < BONGO_CAT_MODEL_CAP)
            snprintf(refresh->pending_packages[
                refresh->pending_package_count++], BONGO_CAT_ID_CAP,
                "%s", package_id);
        else {
            refresh->rerun_full = true;
            refresh->pending_package_count = 0;
        }
        return;
    }
    if (!start_refresh(app, include_nearby, package_id))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot start background model refresh: %s", SDL_GetError());
}

void bongo_cat_app_request_model_refresh(BongoCatApp *app) {
    request_refresh(app, false, NULL);
}

/* 供主循环与 UI 判断是否有刷新在跑（例如显示进度或避免同一帧重复触发）。 */
bool bongo_cat_app_model_refresh_busy(const BongoCatApp *app) {
    return app && app->model_refresh && app->model_refresh->busy;
}

void bongo_cat_app_request_model_package_refresh(BongoCatApp *app,
    const char *package_id) {
    if (!package_id || !package_id[0]) return;
    request_refresh(app, false, package_id);
}

void bongo_cat_app_request_nearby_model_refresh(BongoCatApp *app) {
    if (SDL_getenv("BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN")) return;
    request_refresh(app, true, NULL);
}

/* 主线程已原地改动目录（同步重扫、删除、导入）时调用：revision 自增让在途 worker 的结果
 * 作废，并让它按原样重跑一次；附近扫描标记要继承在跑任务的值，不能凭本次调用来决定。 */
void bongo_cat_model_refresh_invalidate(BongoCatApp *app) {
    BongoCatModelRefresh *refresh = app ? app->model_refresh : NULL;
    if (!refresh) return;
    refresh->revision++;
    if (refresh->busy) {
        refresh->rerun_full = true;
        refresh->rerun_nearby = refresh->rerun_nearby ||
            refresh->active_nearby;
        refresh->pending_package_count = 0;
    }
}

/* 主线程收尾：取走 completed 后 join worker 并清 busy，再决定是否采纳结果。采纳条件是任
 * 务成功且 revision 未变；内容逐字节比对（数量加 memcmp）无变化时不碰目录、不通知偏好设
 * 置，避免无谓重建。采纳整表刷新要 reconcile 并补元数据，采纳包刷新只补该包。最后按
 * rerun_full 与 pending_packages 决定是否立刻再起一轮，并无条件 invalidate 偏好设置，让
 * 它下一帧重读目录与设置。 */
void bongo_cat_model_refresh_update(BongoCatApp *app) {
    BongoCatModelRefresh *refresh = app ? app->model_refresh : NULL;
    if (!refresh || !refresh->busy) return;
    SDL_LockMutex(refresh->mutex);
    BongoCatModelRefreshJob *job = refresh->completed;
    refresh->completed = NULL;
    SDL_UnlockMutex(refresh->mutex);
    if (!job) return;

    if (refresh->worker) SDL_WaitThread(refresh->worker, NULL);
    refresh->worker = NULL;
    refresh->busy = false;
    if (job->success && job->revision == refresh->revision) {
        bool changed = app->models.count != job->models.count ||
            memcmp(app->models.entries, job->models.entries,
                job->models.count * sizeof(job->models.entries[0])) != 0;
        if (changed) {
            app->models = job->models;
            if (job->package_id[0])
                bongo_cat_model_catalog_finish_package(app, job->package_id);
            else bongo_cat_model_catalog_finish(app);
            bongo_cat_preferences_models_changed(app->preferences);
        }
        SDL_Log("Background model refresh completed: models=%llu changed=%d "
            "elapsed_ms=%.1f", (unsigned long long)job->models.count,
            changed, (SDL_GetTicksNS() - job->started_ns) / 1000000.0);
    } else if (!job->success)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Background model refresh failed: %s",
            job->error.message[0] ? job->error.message : "not enough memory");
    free(job);
    bool rerun_full = refresh->rerun_full;
    bool include_nearby = refresh->rerun_nearby;
    char package_id[BONGO_CAT_ID_CAP];
    package_id[0] = '\0';
    if (rerun_full) {
        refresh->rerun_full = false;
        refresh->rerun_nearby = false;
        refresh->pending_package_count = 0;
    } else if (refresh->pending_package_count) {
        snprintf(package_id, sizeof(package_id), "%s",
            refresh->pending_packages[0]);
        memmove(refresh->pending_packages, refresh->pending_packages + 1,
            (refresh->pending_package_count - 1) *
                sizeof(refresh->pending_packages[0]));
        --refresh->pending_package_count;
    }
    if ((rerun_full || package_id[0]) && !start_refresh(app, include_nearby,
            package_id[0] ? package_id : NULL))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot restart background model refresh: %s", SDL_GetError());
    bongo_cat_preferences_invalidate(app->preferences);
}

/* 事件循环里的处理入口：只认自己注册的事件类型与 data1 == refresh 的事件；返回 true 表
 * 示已消费，事件分发不应继续。 */
bool bongo_cat_model_refresh_event(BongoCatApp *app,
    const SDL_Event *event) {
    BongoCatModelRefresh *refresh = app ? app->model_refresh : NULL;
    if (!refresh || !event || event->type != refresh->event_type ||
        event->user.data1 != refresh) return false;
    bongo_cat_model_refresh_update(app);
    return true;
}

/* 关停：先 join worker 再回收它发布的结果，然后排空事件队列里还没处理的、data1 指向本对
 * 象的刷新事件，避免之后用到已释放的 refresh。必须在主循环结束、不再派发事件之后调用。 */
void bongo_cat_model_refresh_shutdown(BongoCatApp *app) {
    BongoCatModelRefresh *refresh = app ? app->model_refresh : NULL;
    if (!refresh) return;
    if (refresh->worker) SDL_WaitThread(refresh->worker, NULL);
    SDL_LockMutex(refresh->mutex);
    BongoCatModelRefreshJob *job = refresh->completed;
    refresh->completed = NULL;
    SDL_UnlockMutex(refresh->mutex);
    free(job);
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, refresh->event_type,
        refresh->event_type) > 0) {}
    SDL_DestroyMutex(refresh->mutex);
    free(refresh);
    app->model_refresh = NULL;
}
