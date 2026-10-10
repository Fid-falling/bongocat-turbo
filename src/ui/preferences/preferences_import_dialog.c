/*
 * 导入器的创建/销毁与状态查询 (真正的导入逻辑在 preferences_import.c 与
 * preferences_import_worker.c)。
 * 这里只强调两条: references 初始为 1 表示"对话框自身"这一次持有, 销毁必须走
 * bongo_cat_preferences_import_destroy (它会先停工作线程), 不能直接 free;
 * 下面两个查询函数都会加锁, 因为调用方是渲染/事件路径, 而工作线程随时在改这些字段。
 */
#include "preferences_import_internal.h"

BongoCatImportDialog *bongo_cat_preferences_import_create(void) {
    BongoCatImportDialog *dialog = SDL_calloc(1, sizeof(*dialog));
    if (!dialog) return NULL;
    dialog->event_type = SDL_RegisterEvents(1);
    dialog->mutex = SDL_CreateMutex();
    if (dialog->event_type == (Uint32)-1 || !dialog->mutex) {
        if (dialog->mutex) SDL_DestroyMutex(dialog->mutex);
        SDL_free(dialog);
        return NULL;
    }
    dialog->references = 1;
    dialog->active = true;
    return dialog;
}

/*
 * "用户还在等导入" —— "浏览"对话框开着 (open) 或者后台正在跑 (busy) 都算。
 * 设置窗口的 release_idle_window 靠它决定能不能把窗口连同 UI 后端一起收掉:
 * 收早了, 导入结果回来时就没有窗口可以汇报了。
 */
bool bongo_cat_preferences_import_is_open(const BongoCatImportDialog *dialog) {
    if (!dialog) return false;
    SDL_LockMutex(dialog->mutex);
    bool open = dialog->open || dialog->busy;
    SDL_UnlockMutex(dialog->mutex);
    return open;
}

/*
 * 取进度快照。四个字段一次性在锁内拷出来, 分多次读会让进度条出现"已完成 > 总数"
 * 这种一闪而过的错乱 (每帧读的时候 worker 还在推进)。
 */
bool bongo_cat_preferences_import_status(const BongoCatImportDialog *dialog,
    uint64_t *started_ns, size_t *completed, size_t *total) {
    if (!dialog) return false;
    SDL_LockMutex(dialog->mutex);
    bool busy = dialog->busy;
    if (started_ns) *started_ns = dialog->started_ns;
    if (completed) *completed = dialog->completed;
    if (total) *total = dialog->total;
    SDL_UnlockMutex(dialog->mutex);
    return busy;
}
