/*
 * 把 Mver 的按键绑定翻译成运行时适配器的 bindings 条目: 配置里的每一行按行号对应 Live2D
 * manifest (Expressions 与 Motions 分组) 里的同名资源, 只有清单声明的文件真实存在时才落条目。
 * 输出 doc 与 items 数组都归调用方所有, 这里只追加。
 * 易错点: 行号是 Mver 配置的数组下标, 跳过某行时对齐关系仍按原下标成立, 不要改成成功计数。
 */
#include "model_import_mver_internal.h"
#include "model_import_mver_manifest.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <yyjson.h>

/* 把一个绑定矩阵与清单里的可用资源逐行配对: rows 为 NULL 表示没有绑定; available 缺组时截断成
   0 条。文件不存在就跳过该行 (清单可能引用可选资源), 但键位解析不出受支持的组合是硬错误 ——
   静默丢弃会让用户按键没反应却查不出原因。 */
static bool add_rows(yyjson_mut_doc *output, yyjson_mut_val *items,
    yyjson_val *rows, const BongoCatImportCandidate *candidate,
    const char *kind, const char *field, const char *group, yyjson_val *available,
    const BongoCatMverLabels *labels,
    BongoCatError *error) {
    if (!rows || yyjson_is_null(rows)) return true;
    if (!yyjson_is_arr(rows)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Mver %s bindings exceed the matching Live2D manifest entries", kind);
        return false;
    }
    size_t limit = yyjson_arr_size(rows);
    if (limit > yyjson_arr_size(available)) limit = yyjson_arr_size(available);
    /* 配置行数多于清单资源时按清单截断且不报错: 多出来的行没有可播放的文件, 忽略它比让整个模型
       导入失败更合适。 */
    size_t index, count; yyjson_val *row;
    yyjson_arr_foreach(rows, index, count, row) {
        if (index >= limit) break;
        if (yyjson_is_null(row) || (yyjson_is_arr(row) && !yyjson_arr_size(row)))
            continue;
        const char *file = yyjson_get_str(yyjson_obj_get(
            yyjson_arr_get(available, index), "File"));
        char path[BONGO_CAT_PATH_CAP];
        if (!file || !bongo_cat_path_join(path, sizeof(path), candidate->directory,
                file) || !bongo_cat_path_is_file(path)) continue;
        char shortcut[BONGO_CAT_SHORTCUT_CAP];
        if (!bongo_cat_mver_chord(candidate, row, shortcut, sizeof(shortcut))) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "Mver %s binding %zu is not a supported input chord", kind, index);
            return false;
        }
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(output, items);
        const char *label = bongo_cat_mver_label(labels, field, index);
        if (!item || !yyjson_mut_obj_add_strcpy(output, item, "kind", kind) ||
            !yyjson_mut_obj_add_int(output, item, "index", (int)index) ||
            !yyjson_mut_obj_add_strcpy(output, item, "shortcut", shortcut) ||
            (label && !yyjson_mut_obj_add_strcpy(output, item, "label", label)) ||
            (group && !yyjson_mut_obj_add_strcpy(output, item, "group", group))) return false;
    }
    return true;
}

/* 读候选的清单文件; 返回的 doc 由调用方 free, 写出的 references 是文档内部的借用指针, 随 doc
   一起失效; 读失败时 doc 与 references 都是 NULL。 */
static yyjson_doc *manifest(const BongoCatImportCandidate *candidate,
    yyjson_val **references) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), candidate->directory,
        candidate->setting)) return NULL;
    yyjson_doc *document = bongo_cat_import_mver_manifest_read(path, NULL);
    *references = document ? yyjson_obj_get(yyjson_doc_get_root(document),
        "FileReferences") : NULL;
    return document;
}

/* 取清单里 group 对应的可用动作数组; 配置与清单对不上时按"没有绑定"处理, 不算错误。 */
static bool add_motion_group(yyjson_mut_doc *output, yyjson_mut_val *items,
    yyjson_val *configured, yyjson_val *motions, const char *field,
    const char *group, const BongoCatImportCandidate *candidate,
    const BongoCatMverLabels *labels, BongoCatError *error) {
    yyjson_val *available = yyjson_obj_get(motions, group);
    if (yyjson_is_arr(configured) && !yyjson_is_arr(available)) return true;
    return add_rows(output, items, configured, candidate, "motion", field,
        group, available, labels, error);
}

/* 追加 expression 与两组 motion 绑定。output 与 items 是调用方持有的可变文档, references 在本
   函数内释放; 失败时若 error 还是空的就补一条通用格式错误, 方便上层直接展示。 */
bool bongo_cat_mver_add_behaviors(void *raw_output, void *raw_items,
    void *raw_mode, const BongoCatImportCandidate *candidate,
    const BongoCatMverLabels *labels, BongoCatError *error) {
    yyjson_mut_doc *output = raw_output;
    yyjson_mut_val *items = raw_items;
    yyjson_val *mode = raw_mode, *references = NULL;
    yyjson_doc *document = manifest(candidate, &references);
    yyjson_val *expressions = yyjson_obj_get(references, "Expressions");
    yyjson_val *motions = yyjson_obj_get(references, "Motions");
    bool ok = document && add_rows(output, items,
        yyjson_obj_get(mode, "l2d_expression"), candidate, "expression",
        "l2d_expression", NULL,
        expressions, labels, error) &&
        add_motion_group(output, items, yyjson_obj_get(mode, "l2d_motion"),
            motions, "l2d_motion", "CAT_motion", candidate, labels, error) &&
        add_motion_group(output, items,
            yyjson_obj_get(mode, "l2d_motion_lockhand"), motions,
            "l2d_motion_lockhand", "CAT_motion_lock", candidate, labels, error);
    yyjson_doc_free(document);
    if (!ok && error && !error->message[0])
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Cannot map Mver behaviors to the Live2D manifest");
    return ok;
}
