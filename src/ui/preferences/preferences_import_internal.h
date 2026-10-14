/*
 * 模型导入的跨线程协议 (主线程 <-> 导入工作线程 <-> 原生文件夹对话框线程)。
 *
 * 三个线程之间只靠 SDL 用户事件传递, 所有权规则必须记牢 —— 这里是本模块最容易
 * 出 use-after-free / 泄漏的地方:
 * - BongoCatImportJob 在"选好文件夹"的回调线程里创建, 通过事件的 data1 交给
 * 主线程; 主线程 start_job 成功后, job 归工作线程所有, 工作线程发完成事件,
 * 再由主线程 complete_job 负责释放;
 * - 任何一步拒绝接收 (对话框已关、窗口没了、pthread 创建失败、PushEvent 失败),
 * 都由拒绝方自己 free, 不能留给别人;
 * - BongoCatImportDialog 是引用计数的 (references), 因为原生对话框回调、工作
 * 线程都可能还在用它, 而窗口随时可能被关掉。
 *
 * 所有的 code 常量都挂在同一个 SDL 用户事件 type 下, 用 user.code 区分语义:
 * 选中文件夹 / 进度 / 完成。
 */
#ifndef BONGO_CAT_PREFERENCES_IMPORT_INTERNAL_H
#define BONGO_CAT_PREFERENCES_IMPORT_INTERNAL_H

#include "preferences_internal.h"
#include "model_import.h"

/* 一次"拖放多文件 / 批量导入"结束后的汇总。多个串行任务的结果合并到一起,
   主线程最后统一弹一条完成提示, 而不是每个文件弹一条。 */
typedef struct BongoCatImportSummary {
    char ids[BONGO_CAT_MODEL_CAP][BONGO_CAT_ID_CAP];
    bool imported[BONGO_CAT_MODEL_CAP];
    size_t count;
    size_t failed_count;
    size_t failed_name_count;
    char failed_names[BONGO_CAT_IMPORT_FAILURE_NAME_CAP][BONGO_CAT_ID_CAP];
    BongoCatError error;
} BongoCatImportSummary;

typedef struct BongoCatImportJob {
    struct BongoCatImportJob *next;
    SDL_WindowID window_id;
    size_t count;
    char **paths;
    char models_root[BONGO_CAT_PATH_CAP];
    char package_ids[BONGO_CAT_MODEL_CAP][BONGO_CAT_ID_CAP];
    bool package_refresh_requested[BONGO_CAT_MODEL_CAP];
    bool package_imported[BONGO_CAT_MODEL_CAP];
    size_t package_id_count;
    size_t resolved_count;
    size_t installed_count;
    size_t restored_count;
    size_t succeeded_count;
    size_t failed_count;
    size_t failed_name_count;
    char failed_names[BONGO_CAT_IMPORT_FAILURE_NAME_CAP][BONGO_CAT_ID_CAP];
    BongoCatResult result;
    BongoCatError error;
    BongoCatImportDialog *dialog;
} BongoCatImportJob;

/*
 * 工作线程内部的进度计数 (不共享给主线程): completed 是"已经处理完多少个",
 * 用来在收尾时补救那些一个都没报进度的批次, 保证进度条不会卡住不动。
 */
typedef struct BongoCatImportProgressContext {
    BongoCatImportJob *job;
    size_t completed;
} BongoCatImportProgressContext;

typedef struct BongoCatImportProgress {
    BongoCatImportJob *job;
    char package_id[BONGO_CAT_ID_CAP];
    size_t package_index;
    size_t installed_count;
} BongoCatImportProgress;

enum {
    BONGO_CAT_IMPORT_EVENT_CODE = 0x42434e49,
    BONGO_CAT_IMPORT_PROGRESS_CODE = 0x42434e4a,
    BONGO_CAT_IMPORT_COMPLETE_CODE = 0x42434e4b
};

/*
 * 工作线程的引用计数。注意它不是"对话框被引用了几次"那么普通:
 * 基数是 1 (对话框自身的持有), 外加每一个还在飞的回调/工作线程各占 1。
 * 归零时释放互斥量和内存, 所以递减必须持锁, 销毁必须解锁之后再做。
 */
struct BongoCatImportDialog {
    SDL_Mutex *mutex;
    Uint32 event_type;
    /* 发起导入时那个窗口的 ID: 窗口在导入过程中被关掉时, 用来判断结果还要不要 */
    SDL_WindowID window_id;
    SDL_Thread *worker;
    /* 当前在跑的任务; 完成事件用它做身份校验, 防止把过期事件套到新任务上 */
    BongoCatImportJob *worker_job;
    uint64_t worker_generation;
    /* 完成事件没能推给主线程时置位, 由 poll_imports 每帧兜底回收 worker */
    bool completion_pending;
    /* 拖放多个文件/连续导入时不丢弃, 先进 pending 队列, 逐个串行执行 */
    BongoCatImportJob *pending_head;
    BongoCatImportJob *pending_tail;
    BongoCatImportSummary summary;
    uint64_t started_ns;
    size_t completed;
    size_t total;
    int references;
    bool active;
    bool open;
    bool busy;
};

void bongo_cat_preferences_import_report_progress(BongoCatImportJob *job,
    const BongoCatImportReceipt *receipt, size_t package_index,
    size_t completed);
void bongo_cat_preferences_import_receive(void *userdata,
    const BongoCatImportReceipt *receipt);
void bongo_cat_preferences_import_record_failure(BongoCatImportJob *job,
    const char *source);
void bongo_cat_preferences_import_merge_failures(BongoCatImportJob *job,
    const BongoCatImportBatchStats *stats);
bool bongo_cat_preferences_import_progress_event(
    BongoCatImportDialog *dialog, BongoCatApp *app, const SDL_Event *event);
void bongo_cat_preferences_import_job_free(BongoCatImportJob *job);
void bongo_cat_preferences_import_dialog_release(
    BongoCatImportDialog *dialog);
int SDLCALL bongo_cat_preferences_import_worker(void *userdata);
void bongo_cat_preferences_import_merge(BongoCatImportSummary *summary,
    const BongoCatImportJob *job);
void bongo_cat_preferences_import_message(BongoCatApp *app,
    const BongoCatImportSummary *summary, char *message, size_t capacity);
void bongo_cat_preferences_import_complete(BongoCatApp *app,
    const BongoCatImportSummary *summary);

#endif
