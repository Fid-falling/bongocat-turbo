/*
 * Tauri → Mver 的转换编排。调用顺序本身就是契约: 建目录树并整树拷贝模型文件, 生成
 * 按键图与矩阵, 读标定, 最后写 config.json 与预览/运行时图层。
 * copy_preview 必须早于 copy_runtime_images —— 后者拿 mousebg.png 当兜底图源。
 * target 是安装流程给的一次性暂存目录, 失败时由调用方整目录删除; 成功后整个目录会被
 * 重新扫描, 因此产物必须能通过 Mver 的发现判据 (img/<mode>/ + config.json)。
 */
#include "model_import_tauri_internal.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct TauriConversionWorkspace {
    TauriKeyFiles left;
    TauriKeyFiles right;
} TauriConversionWorkspace;

/* 建出 Mver 的目录骨架 target/img/<mode>/cat_model, 并通过 mode_root/model_root 返回
 * 两个根路径; 输出缓冲的容量由调用方保证, 失败时内容未定义。 */
static bool create_target_tree(const char *target, BongoCatModelMode mode,
    char *mode_root, size_t mode_capacity, char *model_root,
    size_t model_capacity) {
    char image_root[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_create_directory(target) &&
        bongo_cat_path_join(image_root, sizeof(image_root), target, "img") &&
        bongo_cat_path_join(mode_root, mode_capacity, image_root,
            bongo_cat_mode_name(mode)) &&
        bongo_cat_path_join(model_root, model_capacity, mode_root,
            "cat_model") && bongo_cat_path_create_directory(mode_root) &&
        bongo_cat_path_create_directory(model_root);
}

/* 转换入口。前提: candidate 是 Tauri 格式且 target 可写 (调用方负责清理)。
 * 返回 true 时 installed 被完整填成 BONGO_CAT_IMPORT_MVER 的形态 (config、directory、
 * assets、package_root 全部指向新包, overrides 与 patch_root 清空); 返回 false 时
 * 本函数不碰 installed, 但 target 下可能留下半成品, 由调用方连目录一起删。
 */

bool bongo_cat_import_tauri_convert_to_mver(
    const BongoCatImportCandidate *candidate, const char *target,
    BongoCatImportCandidate *installed, BongoCatError *error) {
    if (!candidate || !target || !installed ||
        candidate->format != BONGO_CAT_IMPORT_TAURI) return false;
    char mode_root[BONGO_CAT_PATH_CAP], model_root[BONGO_CAT_PATH_CAP];
    if (!create_target_tree(target, candidate->mode, mode_root,
            sizeof(mode_root), model_root, sizeof(model_root)) ||
        !bongo_cat_tauri_copy_model_tree(candidate->directory, model_root, 0,
            error)) return false;
    TauriConversionWorkspace *work = calloc(1, sizeof(*work));
    if (!work) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate Tauri conversion workspace");
        return false;
    }
    if (!bongo_cat_tauri_copy_input_images(candidate,
            mode_root, &work->left, &work->right, error)) {
        free(work);
        return false;
    }
    TauriMverCalibration calibration;
    char config[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_tauri_read_calibration(candidate, &calibration) ||
        !bongo_cat_path_join(config, sizeof(config), target, "config.json") ||
        !bongo_cat_tauri_write_config(config, candidate->mode, &work->left,
            &work->right, &calibration, error) ||
        !bongo_cat_tauri_copy_preview(candidate, mode_root, error) ||
        !bongo_cat_tauri_copy_runtime_images(candidate, mode_root, error)) {
        free(work);
        return false;
    }
    free(work);
    *installed = *candidate;
    snprintf(installed->directory, sizeof(installed->directory), "%s",
        model_root);
    snprintf(installed->assets, sizeof(installed->assets), "%s", mode_root);
    snprintf(installed->package_root, sizeof(installed->package_root), "%s",
        target);
    snprintf(installed->config, sizeof(installed->config), "%s", config);
    installed->overrides[0] = '\0';
    installed->patch_root[0] = '\0';
    installed->format = BONGO_CAT_IMPORT_MVER;
    installed->gamepad_buttons = candidate->mode == BONGO_CAT_MODE_GAMEPAD;
    return true;
}
