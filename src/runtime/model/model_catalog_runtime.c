/* 模型目录的同步扫描与刷新（启动、导入后、删除后走这里；后台线程版本见
 * model_catalog_refresh.c）。几个刷新函数共同的模式：先调
 * bongo_cat_model_refresh_invalidate() 作废在途的后台结果，再在堆上拷一份整表快照，扫描
 * 失败就整表回滚 —— 扫描会先清空目录表，没有快照失败后就会丢掉整个列表。契约：低层入口
 * bongo_cat_model_catalog_scan() 只负责重建表，收尾（reconcile 加元数据）由调用方在主线程
 * 调 bongo_cat_model_catalog_finish() 完成。 */
#include "runtime.h"
#include "model_catalog_selection.h"
#include "model_import.h"
#include "model_import_lock.h"
#include "model_storage.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 堆上整表拷贝（BongoCatModelCatalog 是值类型，一份含 BONGO_CAT_MODEL_CAP 个条目），作为
 * 扫描失败时的回滚点。返回的指针归调用方，必须 free；分配失败返回 NULL。 */
static BongoCatModelCatalog *model_catalog_snapshot(
    const BongoCatModelCatalog *models) {
    if (!models) return NULL;
    BongoCatModelCatalog *snapshot = malloc(sizeof(*snapshot));
    if (snapshot) *snapshot = *models;
    return snapshot;
}

/* 回滚用：整表写回，active_model_id 非 NULL 时一并还原（传 NULL 表示这条路径自己负责恢复
 * 它）。 */
static void restore_model_catalog(BongoCatApp *app,
    const BongoCatModelCatalog *snapshot, const char *active_model_id) {
    if (!app || !snapshot) return;
    app->models = *snapshot;
    if (active_model_id)
        snprintf(app->session.active_model_id,
            sizeof(app->session.active_model_id), "%s", active_model_id);
}

/* 附近目录是可选便利（默认根目录常常就是便携版可执行文件所在目录），根不存在属正常情况，
 * 直接返回 OK。返回非 OK 只代表扫描不完整，调用方必须保留已经加进来的条目。 */
static BongoCatResult scan_nearby_root(BongoCatApp *app, const char *root) {
    if (!root || !root[0] || !bongo_cat_path_is_dir(root))
        return BONGO_CAT_OK;
    size_t before = app->models.count;
    BongoCatError error = {0};
    BongoCatResult result = bongo_cat_import_nearby_scan(app, root,
        &error);
    size_t added = app->models.count - before;
    if (added) SDL_Log("Nearby model scan added %llu models from %s",
        (unsigned long long)added, root);
    if (result != BONGO_CAT_OK && error.message[0])
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
    return result;
}

/* 把 asset_root/models 当只读回退源并进目录（preset=true，不需要内置标记文件）。
 * replace=false 时同 id 已存在（models_root 里的副本或已安装包）就保留现有条目 —— 这是在
 * 可写存储不可用时仍保证至少有一个模型能显示的兜底。返回是否真的扫到了内置资源。 */
bool bongo_cat_model_catalog_add_bundled(BongoCatApp *app, bool replace) {
    char root[BONGO_CAT_PATH_CAP];
    if (!app || !bongo_cat_path_join(root, sizeof(root), app->asset_root,
            "models")) return false;
    BongoCatModelCatalog *bundled = calloc(1, sizeof(*bundled));
    if (!bundled) return false;
    BongoCatError error = {0};
    BongoCatResult result = bongo_cat_models_scan(bundled, root, true, &error);
    /* 就地合并：同 id 命中已存在的槽位，否则追加；index 超过容量时跳过，保证兜底资源不会
       撑爆目录。 */
    for (size_t i = 0; i < bundled->count; ++i) {
        const BongoCatModelEntry *entry = &bundled->entries[i];
        size_t index = 0;
        while (index < app->models.count &&
            strcmp(app->models.entries[index].id, entry->id)) index++;
        if (index < app->models.count && !replace) continue;
        if (index >= BONGO_CAT_MODEL_CAP) continue;
        app->models.entries[index] = *entry;
        if (index == app->models.count) app->models.count++;
    }
    bool available = bundled->count > 0 && result == BONGO_CAT_OK;
    if (!available) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Bundled model scan failed: path=%s error=%s", root,
        error.message[0] ? error.message : "no bundled models found");
    free(bundled);
    return available;
}

/* 拥有目录（models_root）的完整重建流程，顺序有依赖：先装内置模型（可能复制文件）并清表
 * 扫描，再补内置资源兜底，最后才枚举已安装包 —— 兜底条目必须早于可选发现，否则第三方的
 * 畸形模型会让内置目录整块消失。cleanup 为真时顺带清理导入残留（.import-*.tmp），只有启
 * 动与删除后的整表重扫才传真。 */
static BongoCatResult scan_owned_models(BongoCatApp *app, bool cleanup) {
    BongoCatError error = {0};
    bongo_cat_models_init(&app->models);
    /* 导入锁覆盖装内置、清理残留、扫拥有目录三步；已安装包扫描自己会再加锁，而锁不可重
       入，所以上面必须先解锁。 */
    bongo_cat_import_storage_lock();
    BongoCatResult result = bongo_cat_model_install_builtins(app->asset_root,
        app->models_root, &error);
    if (result != BONGO_CAT_OK)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Built-in model installation failed; using available models: "
            "path=%s error=%s", app->models_root, error.message);
    /* 每个可选步骤前清空 error：上一步的失败信息不能冒充下一步的原因。 */
    error = (BongoCatError){0};
    if (cleanup && !bongo_cat_model_cleanup_imports(app->models_root, &error))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Model cleanup failed; continuing scan: path=%s error=%s",
            app->models_root, error.message);
    error = (BongoCatError){0};
    result = bongo_cat_models_scan(&app->models, app->models_root, false,
        &error);
    bongo_cat_import_storage_unlock();
    if (result != BONGO_CAT_OK) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Stored built-in model scan incomplete: path=%s error=%s",
        app->models_root, error.message);
    /* Establish fallback entries before optional discovery can fail or fill
       the catalog. Bundled assets do not depend on writable model storage. */
    bongo_cat_model_catalog_add_bundled(app, false);
    error = (BongoCatError){0};
    result = bongo_cat_import_installed_models(app, app->models_root, &error);
    if (result != BONGO_CAT_OK)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Installed model scan incomplete: path=%s result=%d error=%s",
            app->models_root, (int)result,
            error.message[0] ? error.message : "scan unavailable");
    return app->models.count ? BONGO_CAT_OK :
        (result != BONGO_CAT_OK ? result : BONGO_CAT_ERROR_IO);
}

/* 扫描收尾：先 reconcile 修复选中项（部分扫描缺条目不等于用户删了模型），再为每个条目补
 * 运行时元数据，最后在选中项被改动过时通知偏好设置窗口。扫描路径必须成对调用它。 */
void bongo_cat_model_catalog_finish(BongoCatApp *app) {
    if (!app) return;
    /* Absence from a partial scan does not mean the user deleted a model. */
    bool selection_changed = bongo_cat_model_catalog_reconcile(app);
    for (size_t i = 0; i < app->models.count; ++i)
        bongo_cat_import_apply_metadata(app, app->models.entries[i].id,
            app->models.entries[i].adapter_directory);
    if (selection_changed && app->preferences) bongo_cat_preferences_models_changed(app->preferences);
}

/* 全量扫描入口。返回 OK 的判据是目录里至少有能用的模型，而不是每一步都成功：部分失败只
 * 写日志，桌面宠物宁可少几个模型也不能启动失败。nearby_root 为空表示不做附近扫描；环境变
 * 量 BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN 可全局关掉它。 */
BongoCatResult bongo_cat_model_catalog_scan(BongoCatApp *app, bool cleanup,
    const char *nearby_root) {
    if (!app) return BONGO_CAT_ERROR_ARGUMENT;
    BongoCatResult result = scan_owned_models(app, cleanup);
    if (nearby_root && !SDL_getenv("BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN")) {
        /* Nearby folders are an optional convenience (the default root is
           often the folder from which the portable executable was launched).
           A malformed third-party model must not make the built-in catalog
           disappear: the scan can add valid entries before it reports the
           malformed one. Keep those entries and let startup continue. */
        BongoCatResult nearby = scan_nearby_root(app, nearby_root);
        if (nearby != BONGO_CAT_OK)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Nearby model scan was incomplete; continuing with %llu "
                "usable model(s)",
                (unsigned long long)app->models.count);
    }
    return app->models.count ? BONGO_CAT_OK : result;
}

/* 目录外部变化后的整表重扫（删除模型后、启动就绪前）。失败时整表连同 active_model_id 一
 * 起回滚并保留旧目录，不做部分采纳。 */
void bongo_cat_app_rescan_models(BongoCatApp *app) {
    if (!app) return;
    bongo_cat_model_refresh_invalidate(app);
    BongoCatModelCatalog *previous = model_catalog_snapshot(&app->models);
    if (!previous) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Model catalog rescan skipped: cannot allocate catalog snapshot");
        return;
    }
    char active_model_id[BONGO_CAT_ID_CAP];
    snprintf(active_model_id, sizeof(active_model_id), "%s",
        app->session.active_model_id);
    BongoCatResult result = bongo_cat_model_catalog_scan(app, true,
        app->nearby_root);
    if (result != BONGO_CAT_OK) {
        restore_model_catalog(app, previous, active_model_id);
        free(previous);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Model catalog rescan failed; keeping previous catalog");
        return;
    }
    free(previous);
    bongo_cat_model_catalog_finish(app);
}

/* 只重建 models_root（导入完成后走这条）。成功后要把快照里 managed（附近来源）的条目补回
 * 尾部 —— 本函数不扫附近目录，不补就等于把它们从列表里抹掉；目录满时放弃多出来的条目。 */
void bongo_cat_app_refresh_installed_models(BongoCatApp *app) {
    if (!app) return;
    bongo_cat_model_refresh_invalidate(app);
    BongoCatModelCatalog *previous = model_catalog_snapshot(&app->models);
    if (!previous) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Installed model refresh skipped: cannot allocate catalog snapshot");
        return;
    }
    BongoCatResult result = scan_owned_models(app, false);
    if (result != BONGO_CAT_OK) {
        restore_model_catalog(app, previous, NULL);
        free(previous);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Installed model refresh failed; keeping previous catalog");
        return;
    }
    for (size_t i = 0; i < previous->count &&
        app->models.count < BONGO_CAT_MODEL_CAP; ++i) {
        const BongoCatModelEntry *entry = &previous->entries[i];
        if (!entry->managed || bongo_cat_models_find(&app->models, entry->id))
            continue;
        app->models.entries[app->models.count++] = *entry;
    }
    free(previous);
    bongo_cat_model_catalog_finish(app);
}

/* 附近目录变化的增量刷新（由偏好设置窗口的文件监视触发）。与整表重扫一样做快照回滚，另外
 * 在扫描前后显式对齐 active_model_id，保证选中项不被这次扫描改动。 */
void bongo_cat_app_refresh_nearby_models(BongoCatApp *app) {
    if (!app || SDL_getenv("BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN")) return;
    bongo_cat_model_refresh_invalidate(app);
    BongoCatModelCatalog *previous = model_catalog_snapshot(&app->models);
    if (!previous) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Nearby model refresh skipped: cannot allocate catalog snapshot");
        return;
    }
    char active_model_id[BONGO_CAT_ID_CAP];
    snprintf(active_model_id, sizeof(active_model_id), "%s",
        app->session.active_model_id);
    BongoCatResult result = bongo_cat_model_catalog_scan(app, false,
        app->nearby_root);
    if (result != BONGO_CAT_OK) {
        restore_model_catalog(app, previous, active_model_id);
        free(previous);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Nearby model refresh failed; keeping previous catalog");
        return;
    }
    free(previous);
    snprintf(app->session.active_model_id,
        sizeof(app->session.active_model_id), "%s", active_model_id);
    bongo_cat_model_catalog_finish(app);
}
