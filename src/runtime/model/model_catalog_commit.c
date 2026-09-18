#include "runtime.h"
#include "model_import.h"
#include "bongo_cat/path.h"

#include <string.h>

void bongo_cat_model_catalog_finish_package(BongoCatApp *app,
    const char *package_id) {
    if (!app || !package_id || !package_id[0]) return;
    char storage[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(storage, sizeof(storage), app->models_root,
            package_id)) return;
    for (size_t i = 0; i < app->models.count; ++i) {
        BongoCatModelEntry *entry = &app->models.entries[i];
        if (!strcmp(entry->storage_directory, storage))
            bongo_cat_import_apply_metadata(app, entry->id,
                entry->adapter_directory);
    }
}
