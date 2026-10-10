/* 单个包导入或更新后的收尾：只对 storage_directory 落在该包目录下的条目补跑运行时元数
 * 据，而不是像 bongo_cat_model_catalog_finish() 那样遍历整张表（条目多时退化成 O(n²)）。
 * 由后台刷新完成回调在主线程调用；它不增删条目，因此不会让当前选中项失效。 */
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
