/* 删除一个已安装模型（设置窗口与托盘入口最终都走到这里）。难点全在顺序与回滚：先让渲染
 * 器和多宠选择离开这个模型，再决定是删物理目录还是只记墓碑，最后清理名称、行为状态与行
 * 为快捷键。同一个包导出的多个变体共用 storage_directory 时不能删目录，只能把 id 记进
 * settings.removed_models，由后续扫描过滤掉 —— 判定见 model_import_installed.c 中的
 * bongo_cat_settings_model_removed 调用。 */
#include "runtime.h"
#include "model_catalog_selection.h"
#include "model_import.h"
#include "model_import_lock.h"
#include "model_storage.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 统计有多少条目指向同一个物理目录；大于 1 表示该目录是多个变体共用的包目录，删除时必须
 * 退化成只记墓碑，否则会连带删掉兄弟模型。 */
static size_t models_using_storage(const BongoCatModelCatalog *models,
    const char *directory) {
    size_t count = 0;
    if (!models || !directory || !directory[0]) return 0;
    for (size_t i = 0; i < models->count; ++i)
        if (!strcmp(models->entries[i].storage_directory, directory)) count++;
    return count;
}

/* 由存储目录名反推包 id（包目录以 package_id 命名）；目录名不是合法包 id 时返回 false，调
 * 用方按 FORMAT 错误处理。 */
static bool storage_package_id(const char *directory,
    char output[BONGO_CAT_ID_CAP]) {
    const char *name = directory ? bongo_cat_path_name(directory) : NULL;
    return name && name[0] && bongo_cat_import_package_id(output,
        BONGO_CAT_ID_CAP, name);
}

/* 成功返回 BONGO_CAT_OK 并保证：目录已消失或已记墓碑、选中项与多宠附加列表都不再包含该
 * id、名称与行为快捷键已清理、settings 已被改动（落盘由调用方负责）。失败返回 ARGUMENT
 * （id 不存在、内置模型、附近托管、无法停用）、FORMAT（目录名不是合法包 id、墓碑表已满）、
 * PLATFORM（无法切换到替代模型）、CUBISM（找不到能显示的替代模型）、IO（物理删除失败）。
 * 任何失败路径都要把选择恢复到进入前的状态。 */
BongoCatResult bongo_cat_app_remove_model(BongoCatApp *app, const char *id,
    BongoCatError *error) {
    if (!app || !id) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Missing model id");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    const BongoCatModelEntry *entry = bongo_cat_models_find(&app->models, id);
    if (!entry) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Model is not installed: %s", id);
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    if (entry->preset || entry->managed) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT, entry->managed
            ? "Nearby models are managed by their source directory: %s"
            : "Built-in models cannot be removed: %s", id);
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    /* 被删模型在运行时的两种身份：primary 表示它是当前选中或已被渲染器加载的模型，
       additional 表示它只挂在多宠附加列表里。附加模型走 set_model_active 解绑，主模型必
       须先切到替代模型，否则 Live2D 还占着即将被删掉的文件。 */
    bool primary = !strcmp(id, app->session.active_model_id) ||
        !strcmp(id, app->loaded_model);
    bool additional = !primary && bongo_cat_app_model_active(app, id);
    BongoCatModelSelection previous_selection =
        bongo_cat_model_selection_capture(app);
    char directory[BONGO_CAT_PATH_CAP];
    snprintf(directory, sizeof(directory), "%s", entry->storage_directory);
    char package_id[BONGO_CAT_ID_CAP];
    if (!directory[0] || !storage_package_id(directory, package_id)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Model storage directory is invalid: %s", id);
        return BONGO_CAT_ERROR_FORMAT;
    }
    /* 墓碑表在共享目录场景下是唯一的已删除记录，所以先规范化设置再查一次：表满时宁可报错
       不删，也不能删了目录却留不下记录（下次扫描会把模型又找回来）。 */
    bool shared_storage = models_using_storage(&app->models, directory) > 1;
    bongo_cat_settings_validate(&app->settings);
    bool already_removed = bongo_cat_settings_model_removed(
        &app->settings, id);
    if (shared_storage && !already_removed &&
        app->settings.removed_model_count >= BONGO_CAT_MODEL_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Too many removed model versions are recorded");
        return BONGO_CAT_ERROR_FORMAT;
    }
    /* 先脱离渲染器再动磁盘，顺序不能反。切到替代模型失败时直接返回，此时磁盘尚未改动，目
       录与墓碑都保持原样。 */
    if (additional) {
        BongoCatError deactivate_error = {0};
        if (!bongo_cat_app_set_model_active(app, id, false,
                &deactivate_error)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Cannot stop the active model: %s",
                deactivate_error.message);
            return BONGO_CAT_ERROR_PLATFORM;
        }
    } else if (primary) {
        BongoCatError load_error = {0};
        bool replacement = false;
        for (size_t i = 0; i < app->models.count; ++i)
            if (strcmp(app->models.entries[i].id, id) &&
                bongo_cat_app_select_model_with_error(app,
                    app->models.entries[i].id, &load_error)) {
                replacement = true;
                break;
            }
        if (!replacement) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
                "Cannot delete the active model because no replacement could be displayed: %s",
                load_error.message[0] ? load_error.message :
                "no installed models");
            return BONGO_CAT_ERROR_CUBISM;
        }
    }
    if (shared_storage && !already_removed &&
        !bongo_cat_settings_set_model_removed(&app->settings, id, true)) {
        if (primary) {
            BongoCatError restore_error = {0};
            if (!bongo_cat_app_select_model_with_error(app, id,
                    &restore_error))
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to restore model after recording deletion failed: %s",
                    restore_error.message[0] ? restore_error.message :
                    "unknown error");
        }
        if (additional)
            bongo_cat_model_selection_restore(app, &previous_selection);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Cannot record removed model version: %s", id);
        return BONGO_CAT_ERROR_FORMAT;
    }
    /* 只有独占目录才做物理删除，且必须在导入锁内 —— 同一时刻可能有 worker 在枚举
       models_root（锁不可重入，这里不能嵌套其它加锁调用）。共享目录此时已经记好墓碑。 */
    bool removed = true;
    if (!shared_storage) {
        bongo_cat_import_storage_lock();
        removed = bongo_cat_model_remove_tree(directory, error);
        bongo_cat_import_storage_unlock();
    }
    /* 删除失败：重扫目录让内存状态回到现实，再尽量把选择还原回去；还原失败只记日志，因为
       此时目录内容已经不可信，统一返回 IO 让调用方提示用户。 */
    if (!removed) {
        bongo_cat_app_rescan_models(app);
        bool restore_selection = additional;
        if (primary && bongo_cat_models_find(&app->models, id)) {
            BongoCatError restore_error = {0};
            restore_selection = bongo_cat_app_select_model_with_error(app, id,
                &restore_error);
            if (!restore_selection)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to restore model after deletion failed: %s",
                    restore_error.message[0] ? restore_error.message :
                    "unknown error");
        }
        if (restore_selection)
            bongo_cat_model_selection_restore(app, &previous_selection);
        return BONGO_CAT_ERROR_IO;
    }
    if (!shared_storage)
        bongo_cat_settings_restore_model_package(&app->settings, package_id);
    bongo_cat_app_forget_behavior_state(app, id);
    bongo_cat_settings_set_model_label(&app->settings, id, "");
    /* 行为快捷键 id 形如 <模型 id>:<种类>:<序号>（见 model_import_metadata.c 的
       behavior_id），用前缀加冒号双重校验按模型清理，避免误删 id 恰好以此为前缀的其它模
       型；双指针原地压缩，output 只会落后于 i。 */
    size_t output = 0;
    size_t id_length = strlen(id);
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i) {
        BongoCatBehaviorShortcut *shortcut = &app->settings.behavior_shortcuts[i];
        if (!strncmp(shortcut->id, id, id_length) &&
            shortcut->id[id_length] == ':') continue;
        app->settings.behavior_shortcuts[output++] = *shortcut;
    }
    app->settings.behavior_shortcut_count = output;
    bongo_cat_app_rescan_models(app);
    return BONGO_CAT_OK;
}
