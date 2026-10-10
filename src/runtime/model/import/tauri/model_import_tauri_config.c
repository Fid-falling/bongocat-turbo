/*
 * 把归一化结果写成 Mver 规范包的 config.json —— 全流程里唯一产出该文件的地方。
 * 前提: img/<mode>/ 下的图片已经按矩阵行号排好 (见 model_import_tauri_input.c)。
 * 契约: 安装完成后整个目录会被重新扫描一次, 所以 mode 段的形态必须能通过 Mver 的
 * 发现判据 —— l2d 为真、手型是数组、手柄模式带 input_mode=1。
 * 这里写出去的 mode 整数与 BongoCatModelMode 的顺序不一致, 是另一个易错点。
 */
#include "model_import_tauri_internal.h"

#include "bongo_cat/json.h"

#include <yyjson.h>

/* 一个手型 = 一组按键行, 每行是该位置的键码数组 (行序即图片编号)。
 * left/right 允许 count == 0, 此时写出的是空数组。 */
static bool add_matrix(yyjson_mut_doc *document, yyjson_mut_val *mode,
    const char *name, const TauriKeyFiles *files) {
    yyjson_mut_val *matrix = yyjson_mut_obj_add_arr(document, mode, name);
    if (!matrix) return false;
    for (size_t i = 0; i < files->count; ++i) {
        yyjson_mut_val *row = yyjson_mut_arr_add_arr(document, matrix);
        if (!row || !yyjson_mut_arr_add_int(document, row,
                files->values[i].code)) return false;
    }
    return true;
}

/* 先构建 doc 再落盘: ok 短路链保证中途失败不会写出半截文件, 失败统一报 IO 错误。
 * 只写配置, 不搬任何资源; path 的父目录必须已经存在。 */
bool bongo_cat_tauri_write_config(const char *path, BongoCatModelMode mode,
    const TauriKeyFiles *left, const TauriKeyFiles *right,
    const TauriMverCalibration *calibration, BongoCatError *error) {
    if (!path || !left || !right || !calibration) return false;
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = document ? yyjson_mut_obj(document) : NULL;
    if (document) yyjson_mut_doc_set_root(document, root);
    yyjson_mut_val *decoration = root ? yyjson_mut_obj_add_obj(document, root,
        "decoration") : NULL;
    yyjson_mut_val *mode_object = root ? yyjson_mut_obj_add_obj(document, root,
        bongo_cat_mode_name(mode)) : NULL;
    yyjson_mut_val *window_size = decoration ? yyjson_mut_obj_add_arr(document,
        decoration, "window_size") : NULL;
    yyjson_mut_val *offset = decoration ? yyjson_mut_obj_add_arr(document,
        decoration, "l2d_offset") : NULL;
    bool ok = root && decoration && mode_object && window_size && offset &&
        yyjson_mut_arr_add_int(document, window_size,
            calibration->window_width) &&
        yyjson_mut_arr_add_int(document, window_size,
            calibration->window_height) &&
        yyjson_mut_obj_add_real(document, decoration, "l2d_correct",
            calibration->l2d_correct) &&
        yyjson_mut_arr_add_real(document, offset, calibration->l2d_offset_x) &&
        yyjson_mut_arr_add_real(document, offset, calibration->l2d_offset_y) &&
        yyjson_mut_obj_add_bool(document, decoration, "l2d_horizontal_flip",
            calibration->mirror) &&
        yyjson_mut_obj_add_bool(document, decoration, "l2d_auto_frame",
            calibration->auto_frame) &&
        /* 这里写的是 Mver 的模式编号 1/2/3, 不是枚举值 0/1/2 —— 与 BongoCatModelMode
         * 的顺序差一位, 改动前先确认下游怎么读它 (旧版独立精灵模式是 98)。 */
        yyjson_mut_obj_add_int(document, root, "mode",
            mode == BONGO_CAT_MODE_STANDARD ? 1 :
            mode == BONGO_CAT_MODE_KEYBOARD ? 2 : 3) &&
        yyjson_mut_obj_add_bool(document, mode_object, "l2d", true);
    if (ok && mode == BONGO_CAT_MODE_STANDARD)
        ok = add_matrix(document, mode_object, "hand", left) &&
            yyjson_mut_obj_add_bool(document, mode_object, "mouse", false) &&
            yyjson_mut_obj_add_arr(document, mode_object, "keyboard");
    if (ok && mode != BONGO_CAT_MODE_STANDARD)
        ok = add_matrix(document, mode_object, "lefthand", left) &&
            add_matrix(document, mode_object, "righthand", right) &&
            yyjson_mut_obj_add_arr(document, mode_object, "keyboard");
    /* input_mode=1 是手柄键码的开关: 重新扫描已安装包时 Mver 侧只认这个键 (缺失或写 0
     * 就会把 0..15 当成键盘键码), 所以它只在手柄模式下写, 而且不能省。 */

    if (ok && mode == BONGO_CAT_MODE_GAMEPAD)
        ok = yyjson_mut_obj_add_int(document, mode_object, "input_mode", 1);
    if (ok) ok = yyjson_mut_obj_add_arr(document, mode_object, "face") &&
        yyjson_mut_obj_add_arr(document, mode_object, "sounds") &&
        yyjson_mut_obj_add_arr(document, mode_object, "l2d_expression") &&
        yyjson_mut_obj_add_arr(document, mode_object, "l2d_motion") &&
        yyjson_mut_obj_add_arr(document, mode_object, "l2d_motion_lockhand");
    if (ok) ok = bongo_cat_json_write_file(path, document,
        YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(document);
    if (!ok && error) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot write normalized Mver configuration: %s", path);
    return ok;
}
