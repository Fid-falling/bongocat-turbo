/*
 * 来源归一化: 把用户选中的东西(目录、模型文件、图片) 折算成"从哪个目录开始导入"。
 * 位置: 会话安装与渐进导入的第一步, 得到的目录随后交给 discover/probe 识别格式与包根。
 * 契约: 只有返回 BONGO_CAT_OK 时 directory 才有效(失败时可能是被截断的中间态); 常见返回是
 * ARGUMENT 表示选择本身不可用(不存在或路径过长), FORMAT 表示这个文件没有可导入的含义。
 */
#include "model_import.h"
#include "model_import_path.h"
#include "model_import_probe.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 从一张 PNG 反推它所属的包根: 向上找名为 img 的素材目录, 再取其父目录(包根)。
 * 12 层是防御性上限(正常包只有 1~2 层), 找不到就返回 false; 只在来源已确认是文件时调用。 */
static bool image_package_root(const char *source, char *directory,
    size_t capacity) {
    char current[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_import_parent_path(source, current, sizeof(current)))
        return false;
    for (int depth = 0; depth < 12; ++depth) {
        if (SDL_strcasecmp(bongo_cat_path_name(current), "img") == 0)
            return bongo_cat_import_parent_path(current, directory, capacity);
        char parent[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_import_parent_path(current, parent, sizeof(parent)))
            return false;
        snprintf(current, sizeof(current), "%s", parent);
    }
    return false;
}

/* 把来源折算成导入目录: 目录原样返回; 文件按类型分派 —— .moc3 回溯拥有它的包根, .png 必须位于
 * img 素材树下, 其余只接受 config.json、bongocat.skin.json 与 *.model3.json 并取父目录。
 * 只读文件系统属性, 没有副作用; error 只在失败路径写入, 且失败时 directory 内容不可读。 */
BongoCatResult bongo_cat_import_source_directory(const char *source,
    char *directory, size_t capacity, BongoCatError *error) {
    if (!source || !source[0] || !directory || !capacity) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Missing model import source");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    if (bongo_cat_path_is_dir(source)) {
        int length = snprintf(directory, capacity, "%s", source);
        if (length >= 0 && (size_t)length < capacity) return BONGO_CAT_OK;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Model import path is too long");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    if (!bongo_cat_path_is_file(source)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Selected model source does not exist: %s", source);
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    const char *name = bongo_cat_path_name(source);
    if (name && bongo_cat_import_has_suffix_ci(name, ".moc3"))
        return bongo_cat_import_probe_live2d_owner(source, directory,
            capacity, error);
    if (name && bongo_cat_import_has_suffix_ci(name, ".png") &&
        image_package_root(source, directory, capacity)) return BONGO_CAT_OK;
    if (!name || (SDL_strcasecmp(name, "config.json") != 0 &&
        SDL_strcasecmp(name, BONGO_CAT_SKIN_CONFIG_FILE) != 0 &&
        !bongo_cat_import_has_suffix_ci(name, ".model3.json"))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Select a BongoCat skin file, Mver config.json, image-patch PNG, "
            "or Live2D .model3.json/.moc3 file");
        return BONGO_CAT_ERROR_FORMAT;
    }
    if (bongo_cat_import_parent_path(source, directory, capacity))
        return BONGO_CAT_OK;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
        "Cannot determine the selected model file directory: %s", source);
    return BONGO_CAT_ERROR_ARGUMENT;
}
