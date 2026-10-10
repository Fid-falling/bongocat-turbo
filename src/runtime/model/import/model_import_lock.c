/* 模型目录的进程内全局锁：把“扫描 + 修改目录”的临界区串起来（导入、内置模型安装、已安装
 * 包扫描、删除与回滚、nearby 适配器生成都走它），避免两个后台任务同时改写同一棵 models 目录。
 * 互斥量懒创建且可降级为空操作（创建失败时 lock 与 unlock 都不做事），所以它提供互斥，
 * 但不保证“一定拿到锁”；由 lifecycle 关闭阶段调 ..._shutdown 释放。 */
#include "model_import_lock.h"

#include <SDL3/SDL.h>

static SDL_InitState storage_lock_init;
static SDL_Mutex *storage_lock;

static SDL_Mutex *storage_mutex(void) {
    if (SDL_ShouldInit(&storage_lock_init)) {
        storage_lock = SDL_CreateMutex();
        SDL_SetInitialized(&storage_lock_init, storage_lock != NULL);
    }
    return storage_lock;
}

void bongo_cat_import_storage_lock(void) {
    SDL_Mutex *mutex = storage_mutex();
    if (mutex) SDL_LockMutex(mutex);
}

/* 解锁路径直接读全局指针而不走 storage_mutex()：不能在解锁时把锁重新创建出来。 */
void bongo_cat_import_storage_unlock(void) {
    SDL_Mutex *mutex = storage_lock;
    if (mutex) SDL_UnlockMutex(mutex);
}

void bongo_cat_import_storage_lock_shutdown(void) {
    if (!SDL_ShouldQuit(&storage_lock_init)) return;
    SDL_Mutex *mutex = storage_lock;
    storage_lock = NULL;
    SDL_SetInitialized(&storage_lock_init, false);
    SDL_DestroyMutex(mutex);
}
