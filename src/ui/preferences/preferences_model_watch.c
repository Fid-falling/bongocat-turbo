#include "preferences_state.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>

#define MODEL_DIRECTORY_WATCH_INTERVAL_NS 750000000ull

typedef struct ModelDirectorySnapshot {
    uint64_t sum;
    uint64_t xor_value;
    size_t count;
    bool available;
} ModelDirectorySnapshot;

static uint64_t mix_hash(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

static uint64_t name_hash(const char *name) {
    uint64_t value = 1469598103934665603ull;
    for (const unsigned char *cursor = (const unsigned char *)name;
         cursor && *cursor; ++cursor) {
        value ^= *cursor;
        value *= 1099511628211ull;
    }
    return value;
}

static BongoCatPathVisit snapshot_item(void *userdata,
    const char *directory, const char *name) {
    ModelDirectorySnapshot *snapshot = userdata;
    if (!name || name[0] == '.') return BONGO_CAT_PATH_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), directory, name) ||
        !bongo_cat_path_is_dir(path)) return BONGO_CAT_PATH_CONTINUE;
    uint64_t hash = name_hash(name);
    snapshot->sum += mix_hash(hash);
    snapshot->xor_value ^= mix_hash(hash ^ 0x9e3779b97f4a7c15ull);
    snapshot->count++;
    return BONGO_CAT_PATH_CONTINUE;
}

static ModelDirectorySnapshot directory_snapshot(const char *root) {
    ModelDirectorySnapshot snapshot = {0};
    snapshot.available = root && bongo_cat_path_is_dir(root) &&
        bongo_cat_path_enumerate(root, snapshot_item, &snapshot);
    return snapshot;
}

void bongo_cat_preferences_model_watch(BongoCatPreferences *value,
    uint64_t now) {
    if (!value) return;
    bool active = value->visible && value->page == 1 && value->window;
    if (!active) {
        value->model_directory_watch_active = false;
        return;
    }
    if (!value->model_directory_watch_active) {
        value->model_directory_watch_active = true;
        if (!value->model_directory_watch_known)
            value->model_directory_watch_refresh = true;
        value->model_directory_watch_due_ns = 0;
    }
    if (value->model_directory_watch_due_ns > now) return;
    value->model_directory_watch_due_ns = now +
        MODEL_DIRECTORY_WATCH_INTERVAL_NS;
    if (bongo_cat_preferences_import_status(value->import_dialog,
            NULL, NULL, NULL)) return;

    ModelDirectorySnapshot current = directory_snapshot(
        value->app->models_root);
    bool changed = value->model_directory_watch_known &&
        (value->model_directory_watch_available != current.available ||
         value->model_directory_watch_count != current.count ||
         value->model_directory_watch_sum != current.sum ||
         value->model_directory_watch_xor != current.xor_value);
    value->model_directory_watch_known = true;
    value->model_directory_watch_available = current.available;
    value->model_directory_watch_count = current.count;
    value->model_directory_watch_sum = current.sum;
    value->model_directory_watch_xor = current.xor_value;
    if (changed || value->model_directory_watch_refresh) {
        value->model_directory_watch_refresh = false;
        bongo_cat_app_request_model_refresh(value->app);
    }
}
