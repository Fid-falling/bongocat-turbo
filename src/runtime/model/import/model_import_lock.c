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
