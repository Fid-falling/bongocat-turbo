/*
 * 导入进度: 工作线程 -> 主线程的单向汇报。
 * 只传"值"不传指针出去: 进度事件里的 BongoCatImportProgress 是每次新建的堆对象,
 * 排进队列就是主线程的 (由 progress_event 负责 free), 排不进去就立刻在这里 free。
 * 主线程侧做两件事: 记下这个包需要刷新目录, 以及把"上次被禁用、这次又装回来"的
 * 包恢复成启用 —— 恢复必须放在主线程做, 因为要动 settings。
 */
#include "preferences_import_internal.h"
#include "preferences_model_glyphs.h"
#include "model_import.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/*
 * 失败名单只留文件名、且封顶: 这条信息最终是显示给用户看的, 存全路径既长又可能
 * 泄露用户目录结构, 超出上限的失败安静丢弃 (总数另有 failed_count 统计)。
 */
void bongo_cat_preferences_import_record_failure(BongoCatImportJob *job,
    const char *source) {
    if (!job || job->failed_name_count >=
        BONGO_CAT_IMPORT_FAILURE_NAME_CAP) return;
    const char *name = bongo_cat_path_name(source);
    if (!name || !name[0]) return;
    snprintf(job->failed_names[job->failed_name_count++], BONGO_CAT_ID_CAP,
        "%s", name);
}

void bongo_cat_preferences_import_merge_failures(BongoCatImportJob *job,
    const BongoCatImportBatchStats *stats) {
    if (!job || !stats) return;
    for (size_t i = 0; i < stats->failure_name_count; ++i)
        bongo_cat_preferences_import_record_failure(job,
            stats->failure_names[i]);
}

/*
 * 记下这个包并返回它的下标 (用于 package_refresh_requested 去重)。
 * 已经记过就复用原下标; 表满时返回 BONGO_CAT_MODEL_CAP 这个哨兵值, 调用方据此
 * 跳过"标记刷新过"这一步, 但仍然照常刷新目录 —— 不能因为记不下就少刷新一次。
 */
static size_t remember_package(BongoCatImportJob *job, const char *id) {
    if (!job || !id || !id[0]) return BONGO_CAT_MODEL_CAP;
    for (size_t i = 0; i < job->package_id_count; ++i)
        if (!strcmp(job->package_ids[i], id)) return i;
    if (job->package_id_count >= BONGO_CAT_MODEL_CAP)
        return BONGO_CAT_MODEL_CAP;
    size_t index = job->package_id_count++;
    snprintf(job->package_ids[index], BONGO_CAT_ID_CAP, "%s", id);
    return index;
}

/*
 * 解包器每装好一个包就回调一次 (跑在工作线程上)。
 * 这里是唯一把 job->result 置回 OK 的地方 —— 走到这一步说明至少有一个包成功了,
 * 但工作线程开头预设的那个失败结果仍然保留着, 只在真的有成功时才被推翻。
 */
void bongo_cat_preferences_import_receive(void *userdata,
    const BongoCatImportReceipt *receipt) {
    BongoCatImportProgressContext *progress = userdata;
    BongoCatImportJob *job = progress->job;
    size_t package_index = receipt->count
        ? remember_package(job, receipt->ids[0]) : BONGO_CAT_MODEL_CAP;
    job->resolved_count += receipt->count;
    job->installed_count += receipt->installed_count;
    job->result = BONGO_CAT_OK;
    bongo_cat_preferences_import_report_progress(job, receipt, package_index,
        ++progress->completed);
}

/*
 * 每推进一步都走这里。current 是身份校验: 只有"对话框还活着"且"这个 job 仍是当前
 * 任务"才允许更新进度和发事件 —— 用户可能已经关窗重开、甚至又启动了新的导入, 过期
 * 任务的事件必须丢掉, 否则新任务会收到旧任务的数据。
 * 没有 receipt 的调用 (工作线程用来推空的步进) 不发事件, 只更新计数。
 */
void bongo_cat_preferences_import_report_progress(BongoCatImportJob *job,
    const BongoCatImportReceipt *receipt, size_t package_index,
    size_t completed) {
    BongoCatImportProgress *progress = receipt && receipt->count
        ? SDL_calloc(1, sizeof(*progress)) : NULL;
    if (progress) {
        progress->job = job;
        progress->package_index = package_index;
        progress->installed_count = receipt->installed_count;
        bongo_cat_import_package_base_id(progress->package_id,
            sizeof(progress->package_id), receipt->ids[0]);
    }
    SDL_Event event = {0};
    SDL_LockMutex(job->dialog->mutex);
    bool current = job->dialog->active &&
        job->dialog->worker_job == job;
    if (current) {
        job->dialog->completed = completed;
        if (job->dialog->total < completed)
            job->dialog->total = completed;
    }
    bool pushed = false;
    if (current && progress) {
        event.type = job->dialog->event_type;
        event.user.windowID = job->dialog->window_id;
        event.user.code = BONGO_CAT_IMPORT_PROGRESS_CODE;
        event.user.data1 = progress;
        event.user.data2 = job->dialog;
        pushed = SDL_PushEvent(&event);
    }
    SDL_UnlockMutex(job->dialog->mutex);
    if (!pushed) SDL_free(progress);
}

/*
 * 主线程消费进度事件。做一次和发送端同样的身份校验 (事件在队列里排队期间任务可能
 * 已经被换掉), 然后:
 * - 尝试把该模型包从"已禁用"恢复成启用, 并计入 restored_count;
 * - 只有真的装上或恢复了东西才去标脏字形图集并请求刷新目录 —— 否则每来一条进度
 * 都会触发一次目录扫描, 导入大包时会把主线程拖垮。
 * 最后无条件 SDL_free(progress): 事件载荷的所有权在这条路径上终结。
 */
bool bongo_cat_preferences_import_progress_event(
    BongoCatImportDialog *dialog, BongoCatApp *app, const SDL_Event *event) {
    if (!dialog || !event ||
        event->user.code != BONGO_CAT_IMPORT_PROGRESS_CODE) return false;
    BongoCatImportProgress *progress =
        (BongoCatImportProgress *)event->user.data1;
    SDL_LockMutex(dialog->mutex);
    bool current = progress && dialog->active &&
        dialog->worker_job == progress->job;
    SDL_UnlockMutex(dialog->mutex);
    if (current && app) {
        bool restored = bongo_cat_settings_restore_model_package(
            &app->settings, progress->package_id);
        if (restored) progress->job->restored_count++;
        if (progress->installed_count || restored)
            bongo_cat_preferences_model_glyphs_note(app->preferences,
                progress->package_id);
        if (progress->installed_count || restored) {
            if (progress->package_index < BONGO_CAT_MODEL_CAP)
                progress->job->package_refresh_requested[
                    progress->package_index] = true;
            bongo_cat_app_request_model_package_refresh(app,
                progress->package_id);
        }
    }
    SDL_free(progress);
    return true;
}
