/*
 * 单次导入的应用层封装: 目前只有启动期的 CI 冒烟路径(--ci-import=)会调用它, 语义是
 * "导入并切换到新模型"; 批量导入走 BongoCatImportSession 的渐进接口, 不经过这里。
 * Cubism 构建下新模型加载失败会回滚刚装的目录, 所以本文件的失败语义与会话层不同。
 */
#include "model_import.h"
#include "model_import_lock.h"
#include "model_storage.h"
#include "runtime.h"
#include "bongo_cat/path.h"

#include <stdio.h>

#ifdef BONGO_CAT_HAS_CUBISM
/* 回滚辅助: 删除本次提交的目录, 全程持存储锁; 删除失败被忽略(error 传 NULL), 因为
   回滚过程不应该再产生第二个错误。 */
static void remove_receipt(const char *models_root,
    const BongoCatImportReceipt *receipt) {
    char path[BONGO_CAT_PATH_CAP];
    if (!receipt || !models_root) return;
    bongo_cat_import_storage_lock();
    for (size_t i = 0; i < receipt->count; ++i)
        if (receipt->installed[i] &&
            bongo_cat_path_join(path, sizeof(path), models_root, receipt->ids[i]))
            bongo_cat_model_remove_tree(path, NULL);
    bongo_cat_import_storage_unlock();
}
#endif

/* 返回值: OK 表示新模型已进入目录(并尽量选中); 失败时 error 一定有码, 且 receipt 里的
   目录在 Cubism 构建下已经被删除(无 Cubism 时保留)。previous 只在回滚后恢复原来的
   选择, 不参与成功路径。 */
BongoCatResult bongo_cat_app_import_model(BongoCatApp *app, const char *source,
    BongoCatError *error) {
    if (!app || !source) return BONGO_CAT_ERROR_ARGUMENT;
    BongoCatImportReceipt receipt;
    BongoCatResult result = bongo_cat_import_install(source, app->models_root,
        &receipt, error);
    if (result != BONGO_CAT_OK) {
        if (error && !error->message[0]) bongo_cat_error_set(error, result,
            "Model import failed while installing: %s", source);
        return result;
    }
    char previous[BONGO_CAT_PATH_CAP];
    snprintf(previous, sizeof(previous), "%s", app->session.active_model_id);
    size_t preferred = 0;
    for (size_t i = 0; i < receipt.count; ++i)
        if (receipt.installed[i]) { preferred = i; break; }
    const char *imported_id = receipt.count ? receipt.ids[preferred] : NULL;
    if (receipt.count)
        /* 重新导入曾经被用户删掉的包时, 先把它从"已移除"名单里解除: 否则新模型会在
           目录扫描时因为删除记录被过滤掉。传变体 id 也可以, 这里按包 id 前缀匹配。 */
        bongo_cat_settings_restore_model_package(&app->settings,
            receipt.ids[0]);
    bongo_cat_app_refresh_installed_models(app);
    for (size_t i = 0; i < receipt.count; ++i)
        if (receipt.installed[i])
            bongo_cat_app_forget_behavior_state(app, receipt.ids[i]);
    if (imported_id && bongo_cat_app_select_model(app, imported_id))
        return BONGO_CAT_OK;
#ifndef BONGO_CAT_HAS_CUBISM
    const BongoCatModelEntry *entry = imported_id ?
        bongo_cat_models_find(&app->models, imported_id) : NULL;
    if (entry) {
        snprintf(app->session.active_model_id, sizeof(app->session.active_model_id),
            "%s", imported_id);
        app->loaded_mode = entry->mode;
    }
    return BONGO_CAT_OK;
#else
    remove_receipt(app->models_root, &receipt);
    bongo_cat_app_refresh_installed_models(app);
    if (previous[0]) bongo_cat_app_select_model(app, previous);
    bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
        "Model import was rolled back because the Live2D model could not be loaded");
    return BONGO_CAT_ERROR_CUBISM;
#endif
}
