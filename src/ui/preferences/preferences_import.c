/*
 * 模型导入的编排: 原生文件夹对话框 -> 工作线程 -> 主线程收尾。
 *
 * 主线程侧负责三件事: 分发导入相关事件 (import_event)、启动任务 (start_job /
 * import_path), 以及在完成时把结果落回设置与目录 (complete_job)。
 * 真正的解包安装全在 preferences_import_worker.c 里跑, 主线程不能碰。
 *
 * 所有权速查 (谁创建谁负责, 拒绝方自己回收):
 * import_callback (对话框线程) 造 job  -> 主线程 import_event -> start_job 交给
 * 工作线程 -> 工作线程发完成事件 -> 主线程 complete_job 释放。
 * 任何一环失败, 失败的那一环负责 free, 见 import_callback / import_event /
 * start_job 里的三处 free 分支。
 */
#include "preferences_import_internal.h"
#include "preferences_state.h"
#include "model_import.h"
#ifdef _WIN32
#include "windows_dialog.h"
#endif
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void bongo_cat_preferences_import_job_free(BongoCatImportJob *job) {
    if (!job) return;
    for (size_t i = 0; i < job->count; ++i) SDL_free(job->paths[i]);
    SDL_free(job->paths);
    SDL_free(job);
}

/*
 * 按事件种类回收它的载荷。进度事件的 data1 是一块独立堆内存, 完成/选中事件才是 job
 * 本体 —— 两种载荷的释放方式不同, 混了就会写坏堆。
 * release_worker 是给销毁路径用的: 完成事件被丢弃意味着工作线程没能把自己的那次
 * 引用还回来, 得由排空事件的人补上 (见 import_destroy)。
 */
static void free_import_event(const SDL_Event *event, bool *release_worker) {
    if (!event) return;
    if (event->user.code == BONGO_CAT_IMPORT_PROGRESS_CODE) {
        SDL_free(event->user.data1);
        return;
    }
    BongoCatImportJob *job = (BongoCatImportJob *)event->user.data1;
    if (release_worker && event->user.code == BONGO_CAT_IMPORT_COMPLETE_CODE)
        *release_worker = true;
    bongo_cat_preferences_import_job_free(job);
}
/*
 * 还一次引用, 归零才真正销毁。
 * 递减在锁内做、销毁在锁外做: SDL_DestroyMutex 之后不能再用这把锁, 顺序反了就是
 * 解锁已销毁的互斥量。调用方可能来自对话框回调线程, 所以这里不能假设自己在主线程。
 */
void bongo_cat_preferences_import_dialog_release(
    BongoCatImportDialog *dialog) {
    bool destroy = false;
    SDL_LockMutex(dialog->mutex);
    destroy = --dialog->references == 0;
    SDL_UnlockMutex(dialog->mutex);
    if (destroy) {
        SDL_DestroyMutex(dialog->mutex);
        SDL_free(dialog);
    }
}
/*
 * 关闭设置窗口时销毁导入器。
 * 顺序不能换: 先让 active=false 再唤醒/等工作线程 —— active 是工作线程决定"还要不
 * 要发完成事件"的依据, 先等的活, 线程可能正卡在等待某个还没影的事件上。
 * 线程结束后把队列里剩下的本对话框事件排空: 那些事件里带着 job 和进度载荷, 不排掉
 * 就既泄漏又在下次开窗时被当成新事件处理。data2 != dialog 的跳过 (可能是别的实例的)。
 */
void bongo_cat_preferences_import_destroy(BongoCatImportDialog *dialog) {
    if (!dialog) return;
    SDL_LockMutex(dialog->mutex);
    dialog->active = false;
    dialog->open = false;
    SDL_Thread *worker = dialog->worker;
    SDL_UnlockMutex(dialog->mutex);
    if (worker) SDL_WaitThread(worker, NULL);
    bool release_worker = false;
    SDL_LockMutex(dialog->mutex);
    dialog->worker = NULL;
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, dialog->event_type,
        dialog->event_type) > 0) {
        if (event.user.data2 != dialog) continue;
        free_import_event(&event, &release_worker);
    }
    if (dialog->completion_pending) {
        bongo_cat_preferences_import_job_free(dialog->worker_job);
        dialog->completion_pending = false;
        release_worker = true;
    }
    dialog->worker_job = NULL;
    while (dialog->pending_head) {
        BongoCatImportJob *job = dialog->pending_head;
        dialog->pending_head = job->next;
        bongo_cat_preferences_import_job_free(job);
    }
    dialog->pending_tail = NULL;
    dialog->busy = false;
    dialog->started_ns = 0;
    dialog->completed = dialog->total = 0;
    /* 补上 worker 没能还回的那一次引用 (队列持有者此刻仍占着 dialog) */
    if (release_worker) --dialog->references;
    SDL_UnlockMutex(dialog->mutex);
    bongo_cat_preferences_import_dialog_release(dialog);
}
/*
 * 把 SDL 给的以 NULL 结尾的文件名数组深拷一份: 那个数组只在回调期间有效, 而 job
 * 要跨线程活很久, 所以连字符串带数组全部自己复制, 逐级失败逐级回滚。
 */
static BongoCatImportJob *copy_job(const char *const *files) {
    if (!files || !files[0]) return NULL;
    size_t count = 0;
    while (files[count]) ++count;
    BongoCatImportJob *job = SDL_calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->paths = SDL_calloc(count, sizeof(*job->paths));
    if (!job->paths) { SDL_free(job); return NULL; }
    job->count = count;
    for (size_t i = 0; i < count; ++i) {
        job->paths[i] = SDL_strdup(files[i]);
        if (!job->paths[i]) {
            bongo_cat_preferences_import_job_free(job);
            return NULL;
        }
    }
    return job;
}
static BongoCatImportJob *copy_path(const char *path) {
    const char *files[] = {path, NULL};
    return path && path[0] ? copy_job(files) : NULL;
}

/*
 * 把 job 交给工作线程。models_root 在这里才拷进 job, 因为它是"启动那一刻"的设置,
 * 之后用户改缓存目录不该影响已经在跑的任务。
 * 引用计数先加后建线程: 线程一跑起来就可能立刻发完成事件, 那时它得能安全地引用
 * dialog; 建线程失败要把这一次引用和所有状态一起回滚, 否则对话框永远销毁不掉。
 * busy 是并发闸: 同一时间只允许一个导入任务。
 */
static bool start_job(BongoCatImportDialog *dialog, BongoCatApp *app,
    SDL_WindowID window_id, BongoCatImportJob *job) {
    if (!dialog || !app || !job || !app->models_root[0]) return false;
    snprintf(job->models_root, sizeof(job->models_root), "%s", app->models_root);
    job->dialog = dialog;
    job->window_id = window_id;
    SDL_LockMutex(dialog->mutex);
    if (!dialog->active) {
        SDL_UnlockMutex(dialog->mutex);
        return false;
    }
    if (dialog->busy) {
        /* 有任务在跑, 不丢弃新拖进来的文件, 先进 pending 队列, 完成后再串行拉起来 */
        if (dialog->pending_tail) dialog->pending_tail->next = job;
        else dialog->pending_head = job;
        dialog->pending_tail = job;
        SDL_UnlockMutex(dialog->mutex);
        return true;
    }
    dialog->busy = true;
    dialog->worker_job = job;
    dialog->started_ns = SDL_GetTicksNS();
    dialog->completed = 0;
    dialog->total = job->count;
    ++dialog->references;
    dialog->worker = SDL_CreateThread(bongo_cat_preferences_import_worker,
        BONGO_CAT_SLUG "-model-import", job);
    if (!dialog->worker) {
        dialog->worker_job = NULL;
        dialog->completion_pending = false;
        dialog->busy = false;
        dialog->started_ns = 0;
        dialog->completed = dialog->total = 0;
        --dialog->references;
    }
    SDL_UnlockMutex(dialog->mutex);
    return dialog->worker != NULL;
}
/*
 * 从 pending 队列里把下一个任务拉起来跑。线程创建失败不放弃整批: 把该任务记成
 * 失败并合并进 summary, 继续试下一个, 直到队列空或有一个真正跑起来。
 */
static void start_pending_jobs(BongoCatImportDialog *dialog, BongoCatApp *app) {
    for (;;) {
        SDL_LockMutex(dialog->mutex);
        BongoCatImportJob *job = dialog->active && !dialog->busy
            ? dialog->pending_head : NULL;
        if (job) {
            dialog->pending_head = job->next;
            if (!dialog->pending_head) dialog->pending_tail = NULL;
            job->next = NULL;
        }
        SDL_UnlockMutex(dialog->mutex);
        if (!job) return;
        if (start_job(dialog, app, job->window_id, job)) return;
        BongoCatError error = {0};
        bongo_cat_error_set(&error, BONGO_CAT_ERROR_IO,
            "Cannot start model import thread: %s", SDL_GetError());
        for (size_t i = 0; i < job->count; ++i)
            bongo_cat_preferences_import_record_failure(job, job->paths[i]);
        job->failed_count = job->count;
        job->error = error;
        bongo_cat_preferences_import_merge(&dialog->summary, job);
        bongo_cat_preferences_import_job_free(job);
    }
}
/*
 * 原生文件夹对话框的回调 (跑在对话框自己的线程上)。
 * 从进入函数起 dialog 上的每一次访问都在锁内 —— 用户可能正好在这一刻关窗口。
 * 三种失败都要把这次回调占的引用还掉, 并且只要事件没成功排进队列, 就得自己
 * free(job): 没人会再来接手它。open=false 表示"对话框已经有了结论", 用户看到
 * 的等待状态要在这里结束。
 */
static void SDLCALL import_callback(void *userdata, const char *const *files,
    int filter) {
    (void)filter;
    BongoCatImportDialog *dialog = userdata;
    BongoCatImportJob *job = copy_job(files);
    SDL_Event event = {0};
    bool pushed = false;
    SDL_LockMutex(dialog->mutex);
    event.type = dialog->event_type;
    event.user.windowID = dialog->window_id;
    event.user.code = BONGO_CAT_IMPORT_EVENT_CODE;
    event.user.data1 = job;
    event.user.data2 = dialog;
    if (dialog->active && !dialog->busy && job) pushed = SDL_PushEvent(&event);
    if (!dialog->active)
        SDL_Log("[runtime] Folder dialog result ignored during shutdown");
    if (!pushed) dialog->open = false;
    SDL_UnlockMutex(dialog->mutex);
    if (!pushed) bongo_cat_preferences_import_job_free(job);
    bongo_cat_preferences_import_dialog_release(dialog);
}

/*
 * 打开"选择文件夹"对话框。先占一次引用 (回调结束时还), 这样即使窗口立刻被关,
 * dialog 也不会在回调飞在半路时被销毁。open 兼作去重: 已经开着就别再开一个,
 * 否则两个回调会互相把 open 置回 false。
 */
bool bongo_cat_preferences_import_open(BongoCatImportDialog *dialog,
    SDL_Window *window) {
    if (!dialog || !window) return false;
    SDL_LockMutex(dialog->mutex);
    if (!dialog->active || dialog->open || dialog->busy) {
        SDL_UnlockMutex(dialog->mutex);
        return false;
    }
    dialog->open = true;
    dialog->window_id = SDL_GetWindowID(window);
    ++dialog->references;
    SDL_UnlockMutex(dialog->mutex);
#ifdef _WIN32
    bongo_cat_windows_show_open_folder_dialog(import_callback, dialog, window,
        NULL, true);
#else
    SDL_ShowOpenFolderDialog(import_callback, dialog, window, NULL, true);
#endif
    return true;
}

/*
 * 导入完成的主线程收尾, 顺序有实际意义:
 * 1. 先把 worker/worker_job/busy 清掉并 join 线程, 之后 dialog 才重新可用;
 * 2. 再恢复那些"原来被禁用、这次导入又装回来了"的模型包 —— 不恢复的话用户会
 * 发现导进来的模型在列表里是灰的, 以为导入失败;
 * 3. 只对没人请求过刷新的包发刷新请求, 避免同一包被要求刷两次 (合并成一次扫描);
 * 4. 最后才回调 import_complete 提示用户, 并释放 job 和这一次的引用。
 * restored_count 要并进 installed_count 一起报给用户, 否则"恢复"的那部分不计入结果。
 */
static void complete_job(BongoCatImportDialog *dialog, BongoCatApp *app,
    BongoCatImportJob *job) {
    SDL_Thread *worker = NULL;
    SDL_LockMutex(dialog->mutex);
    if (dialog->worker_job == job) {
        worker = dialog->worker;
        dialog->worker = NULL;
        dialog->worker_job = NULL;
        dialog->completion_pending = false;
        dialog->busy = false;
        dialog->started_ns = 0;
        dialog->completed = dialog->total = 0;
    }
    SDL_UnlockMutex(dialog->mutex);
    if (worker) SDL_WaitThread(worker, NULL);
    size_t restored_count = job->restored_count;
    if (job->result == BONGO_CAT_OK)
        for (size_t i = 0; i < job->package_id_count; ++i)
            if (bongo_cat_settings_restore_model_package(&app->settings,
                    job->package_ids[i])) {
                restored_count++;
                job->package_imported[i] = true;
            }
    bool catalog_changed = job->installed_count > 0 || restored_count > 0;
    if (job->result == BONGO_CAT_OK && catalog_changed)
        for (size_t i = 0; i < job->package_id_count; ++i)
            if (!job->package_refresh_requested[i])
                bongo_cat_app_request_model_package_refresh(app,
                    job->package_ids[i]);
    bongo_cat_preferences_import_merge(&dialog->summary, job);
    /* 队列里还有就继续拉下一个任务; 全跑完了才合并弹一条完成提示 */
    start_pending_jobs(dialog, app);
    if (!bongo_cat_preferences_import_status(dialog, NULL, NULL, NULL)) {
        bongo_cat_preferences_import_complete(app, &dialog->summary);
        memset(&dialog->summary, 0, sizeof(dialog->summary));
    }
    bongo_cat_preferences_import_job_free(job);
    bongo_cat_preferences_import_dialog_release(dialog);
}
bool bongo_cat_preferences_imports_busy(const BongoCatPreferences *value) {
    return value && bongo_cat_preferences_import_status(value->import_dialog,
        NULL, NULL, NULL);
}
/* 交给控制层, 停止接受新的导入任务 (关窗/退出时调用) */
void bongo_cat_preferences_stop_imports(BongoCatPreferences *value) {
    BongoCatImportDialog *dialog = value ? value->import_dialog : NULL;
    if (!dialog) return;
    SDL_LockMutex(dialog->mutex);
    dialog->active = false;
    dialog->open = false;
    SDL_UnlockMutex(dialog->mutex);
}
/*
 * 每帧兜底: 完成事件没能推给主线程 (active 变 false / PushEvent 失败) 时,
 * worker 不再发事件, 这里的 completion_pending 是回收 worker 的最后机会。
 */
void bongo_cat_preferences_poll_imports(BongoCatPreferences *value) {
    BongoCatImportDialog *dialog = value ? value->import_dialog : NULL;
    if (!dialog) return;
    SDL_LockMutex(dialog->mutex);
    BongoCatImportJob *job = dialog->active && dialog->completion_pending ?
        dialog->worker_job : NULL;
    SDL_UnlockMutex(dialog->mutex);
    if (job) complete_job(dialog, value->app, job);
}

/*
 * 分发导入相关事件。先校验 data2 是不是本对话框 —— 进程里可能有多个 app 实例
 * (冒烟测试), 事件类型是全局的, 不校验就会串台。
 * 选中文件夹那条要额外确认发起时的窗口还在: 窗口没了就当这次选择从没发生, 直接
 * 丢掉 job (用户已经看不到任何结果了)。
 */
bool bongo_cat_preferences_import_event(BongoCatImportDialog *dialog,
    BongoCatApp *app, const SDL_Event *event) {
    if (!dialog || !event || event->type != dialog->event_type ||
        event->user.data2 != dialog) return false;
    BongoCatImportJob *job = (BongoCatImportJob *)event->user.data1;
    if (event->user.code == BONGO_CAT_IMPORT_EVENT_CODE) {
        SDL_Window *owner = SDL_GetWindowFromID(event->user.windowID);
        SDL_LockMutex(dialog->mutex);
        dialog->open = false;
        bool accept = dialog->active && owner != NULL;
        SDL_UnlockMutex(dialog->mutex);
        if (accept && app && job && !start_job(dialog, app,
            event->user.windowID, job))
            bongo_cat_preferences_import_job_free(job);
        else if (!accept) bongo_cat_preferences_import_job_free(job);
        return true;
    }
    if (bongo_cat_preferences_import_progress_event(dialog, app, event))
        return true;
    if (event->user.code == BONGO_CAT_IMPORT_COMPLETE_CODE && app && job) {
        complete_job(dialog, app, job);
        return true;
    }
    return false;
}

/* 拖放进入的入口: 不经过原生对话框, 直接把落下的路径当成一次任务启动 */
void bongo_cat_preferences_import_path(BongoCatApp *app,
    SDL_Window *window, const char *path) {
    if (!app || !app->preferences || !window || !path || !path[0]) return;
    BongoCatImportJob *job = copy_path(path);
    if (!job || !start_job(app->preferences->import_dialog, app,
        SDL_GetWindowID(window), job))
        bongo_cat_preferences_import_job_free(job);
    else app->preferences->render_dirty = true;
}