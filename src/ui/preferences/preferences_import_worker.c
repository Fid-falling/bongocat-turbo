/*
 * 导入工作线程: 解包 + 安装 + 报进度, 全程不碰 UI。
 *
 * 与主线程只通过 SDL 用户事件通信 (进度事件 / 完成事件), 共享数据一律在
 * dialog->mutex 下访问。返回值固定 0, 结果放在 job->result 里。
 *
 * 收尾那段是"最后一步的所有权交接": 能成功排出完成事件就把 job 交给主线程,
 * 排不出去 (窗口已关 / 队列满) 就自己把 job 和这次引用一起还掉。两条路只能走一条。
 */
#include "preferences_import_internal.h"

#include <SDL3/SDL.h>

int SDLCALL bongo_cat_preferences_import_worker(void *userdata) {
    BongoCatImportJob *job = userdata;
    /* 降优先级: 导入是纯后台劳动, 抢 CPU 会让设置窗口的滚动和宠物一起卡 */
    if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot lower model import thread priority: %s", SDL_GetError());
        SDL_ClearError();
    }
    job->result = BONGO_CAT_ERROR_FORMAT;
    /* 先假定最坏结果, 后面每成功一步再往上覆盖: 中途异常退出时不会误报成功 */
    SDL_Log("[runtime] Model import started: sources=%llu",
        (unsigned long long)job->count);
    BongoCatImportSession *session = bongo_cat_import_session_create(
        job->models_root, &job->error);
    if (!session) {
        job->result = job->error.code ? job->error.code : BONGO_CAT_ERROR_IO;
        if (job->error.code == BONGO_CAT_OK) job->error.code = job->result;
        job->failed_count = job->count;
        for (size_t i = 0; i < job->count; ++i)
            bongo_cat_preferences_import_record_failure(job, job->paths[i]);
    }
    BongoCatImportProgressContext progress = {job, 0};
    for (size_t i = 0; session && i < job->count; ++i) {
        size_t before = progress.completed;
        BongoCatImportBatchStats stats = {0};
        BongoCatError error = {0};
        SDL_Log("[runtime] Model import source started: index=%llu/%llu path=%s",
            (unsigned long long)(i + 1), (unsigned long long)job->count,
            job->paths[i]);
        BongoCatResult result = bongo_cat_import_session_install_progressive(
            session, job->paths[i], bongo_cat_preferences_import_receive,
            &progress, &stats, &error);
        SDL_Log("[runtime] Model import source completed: index=%llu/%llu "
            "result=%d succeeded=%llu failed=%llu",
            (unsigned long long)(i + 1), (unsigned long long)job->count,
            (int)result, (unsigned long long)stats.succeeded_count,
            (unsigned long long)stats.failed_count);
        job->succeeded_count += stats.succeeded_count;
        job->failed_count += stats.failed_count;
        bongo_cat_preferences_import_merge_failures(job, &stats);
        if (result != BONGO_CAT_OK && job->error.code == BONGO_CAT_OK) {
            job->result = result;
            job->error = error;
            if (job->error.code == BONGO_CAT_OK) job->error.code = result;
        }
        if (stats.failed_count) {
            /* 失败条目也算"处理过", 否则进度会停在原地不动 */
            progress.completed += stats.failed_count;
            bongo_cat_preferences_import_report_progress(job, NULL,
                BONGO_CAT_MODEL_CAP, progress.completed);
        } else if (progress.completed == before) {
            /* 这一批一个条目都没报 (比如整包被跳过): 手动推一格, 保证进度只增不减 */
            bongo_cat_preferences_import_report_progress(job, NULL,
                BONGO_CAT_MODEL_CAP, ++progress.completed);
        }
    }
    bongo_cat_import_session_destroy(session);
    /* 部分成功也算成功: 用户导 10 个成了 8 个, 不该只看到一条失败 */
    if (job->succeeded_count) job->result = BONGO_CAT_OK;
    if (!job->succeeded_count && job->error.code == BONGO_CAT_OK)
        bongo_cat_error_set(&job->error, BONGO_CAT_ERROR_FORMAT,
            "No model could be imported");
    SDL_Log("[runtime] Model import finished: resolved=%llu installed=%llu "
        "succeeded=%llu failed=%llu result=%d",
        (unsigned long long)job->resolved_count,
        (unsigned long long)job->installed_count,
        (unsigned long long)job->succeeded_count,
        (unsigned long long)job->failed_count, (int)job->result);
    /*
     * 交接点: 事件里的 data1 就是 job, 主线程收到后会释放它, 所以这里绝不能再碰。
     * 反过来说, 只要 notify 为假, job 的释放责任就还在本线程身上 (下面的分支),
     * 同时替主线程把 busy/计时状态清掉, 免得对话框永远显示"导入中"。
     */
    SDL_Event event = {0};
    SDL_LockMutex(job->dialog->mutex);
    bool active = job->dialog->active;
    bool notify = active;
    if (notify) {
        event.type = job->dialog->event_type;
        event.user.windowID = job->window_id;
        event.user.code = BONGO_CAT_IMPORT_COMPLETE_CODE;
        event.user.data1 = job;
        event.user.data2 = job->dialog;
        notify = SDL_PushEvent(&event);
    }
    if (active && !notify) job->dialog->completion_pending = true;
    if (!active) {
        job->dialog->worker_job = NULL;
        job->dialog->busy = false;
        job->dialog->started_ns = 0;
        job->dialog->completed = job->dialog->total = 0;
    }
    SDL_UnlockMutex(job->dialog->mutex);
    if (active && !notify) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Model import completion event was rejected; the main loop will "
        "reclaim the worker");
    if (!active) {
        BongoCatImportDialog *dialog = job->dialog;
        bongo_cat_preferences_import_job_free(job);
        bongo_cat_preferences_import_dialog_release(dialog);
    }
    return 0;
}