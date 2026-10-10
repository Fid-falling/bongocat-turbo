/*
 * 输入图归一化: 把 Tauri 的 left-keys/right-keys 里按名字编码的按键图重排成 Mver 的
 * <hand|lefthand|righthand>/<行号>.png, 并把行号与键码的对应关系交给 config.json。
 * 关键不变量: 编号与矩阵行序必须来自同一次排序结果 —— Mver 侧按矩阵行号取图, 行存在
 * 而图片缺失会被静默跳过 (表现成某些键不响应, 而且不报错)。
 * 缺组不是错误: 会补一张占位图和兜底键码, 保证包判据里的 0.png 一定存在。
 */
#include "model_import_tauri_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 收集一个按键目录里的 .png: 解不出键码的、以及目录枚举带上来的子目录都跳过;
 * 数量按 TAURI_KEY_CAP 截断, 满了之后的多余文件被静默丢弃。 */
static BongoCatPathVisit collect_keys(void *userdata,
    const char *dirname, const char *name) {
    TauriKeyFiles *files = userdata;
    if (files->count >= TAURI_KEY_CAP ||
        !bongo_cat_import_has_suffix_ci(name, ".png"))
        return BONGO_CAT_PATH_CONTINUE;
    int code = bongo_cat_tauri_key_code(name);
    if (code < 0) return BONGO_CAT_PATH_CONTINUE;
    TauriKeyFile *item = &files->values[files->count];
    if (!bongo_cat_path_join(item->path, sizeof(item->path), dirname, name))
        return BONGO_CAT_PATH_FAILURE;
    if (!bongo_cat_path_is_file(item->path)) return BONGO_CAT_PATH_CONTINUE;
    snprintf(item->name, sizeof(item->name), "%s", name);
    item->code = code;
    files->count++;
    return BONGO_CAT_PATH_CONTINUE;
}

/* 排序键是文件名 (大小写不敏感)。这一次排序同时决定 0.png..N.png 的编号与 config.json
 * 矩阵的行序, 两者必须同源 —— 分别排序就会出现“图对不上键码”的静默错位。 */
static int compare_keys(const void *left, const void *right) {
    const TauriKeyFile *a = left, *b = right;
    return SDL_strcasecmp(a->name, b->name);
}

/* 归一化一组按键图。源目录不存在直接返回 true (Tauri 允许某一侧缺席), 所以 false
 * 一定代表真错误: 枚举失败或某张图复制失败 (后者会写 error)。 */
static bool copy_keys(const BongoCatImportCandidate *candidate,
    const char *mode_root, const char *source_group, const char *target_group,
    TauriKeyFiles *files, BongoCatError *error) {
    char source[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_tauri_find_resource_directory(candidate, source_group,
            source, sizeof(source))) return true;
    if (!bongo_cat_path_enumerate(source, collect_keys, files)) return false;
    qsort(files->values, files->count, sizeof(files->values[0]), compare_keys);
    char destination[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(destination, sizeof(destination), mode_root,
            target_group) || !bongo_cat_path_create_directory(destination))
        return false;
    for (size_t i = 0; i < files->count; ++i) {
        char filename[32], path[BONGO_CAT_PATH_CAP];
        snprintf(filename, sizeof(filename), "%zu.png", i);
        if (!bongo_cat_path_join(path, sizeof(path), destination, filename) ||
            !bongo_cat_path_copy_file(files->values[i].path, path)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot copy Tauri input image: %s", files->values[i].path);
            return false;
        }
    }
    return true;
}

/* 兜底: 组里一张图都没有时补一个 0.png 占位和一条键码。没有它, 空矩阵会让这个包的输入
 * 完全不响应, 而且 Mver 的包判据只能靠 cat_model 里的 model3.json 兜住。 */
static bool ensure_key(const char *mode_root, const char *target_group,
    int fallback_code, TauriKeyFiles *files, BongoCatError *error) {
    if (files->count) return true;
    char group[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(group, sizeof(group), mode_root, target_group) ||
        !bongo_cat_path_create_directory(group) ||
        !bongo_cat_path_join(target, sizeof(target), group, "0.png") ||
        !bongo_cat_tauri_copy_image_or_placeholder(NULL, target, error))
        return false;
    files->values[0].code = fallback_code;
    files->count = 1;
    return true;
}

/* 入口: 输出 left/right 由本函数清零后填满, 调用方不需要预初始化。standard 只有一组
 * hand; keyboard/gamepad 是 lefthand + righthand, 兜底键码键盘用 65 (A 键), 手柄用
 * 按钮序号 0/1。 */

bool bongo_cat_tauri_copy_input_images(
    const BongoCatImportCandidate *candidate,
    const char *mode_root, TauriKeyFiles *left, TauriKeyFiles *right,
    BongoCatError *error) {
    *left = (TauriKeyFiles){0};
    *right = (TauriKeyFiles){0};
    if (candidate->mode == BONGO_CAT_MODE_STANDARD) {
        return copy_keys(candidate, mode_root, "left-keys", "hand", left,
            error) &&
            ensure_key(mode_root, "hand", 65, left, error);
    }
    return copy_keys(candidate, mode_root,
        "left-keys", "lefthand", left, error) &&
        copy_keys(candidate, mode_root,
            "right-keys", "righthand", right, error) &&
        ensure_key(mode_root, "lefthand",
            candidate->mode == BONGO_CAT_MODE_GAMEPAD ? 0 : 65, left, error) &&
        ensure_key(mode_root, "righthand",
            candidate->mode == BONGO_CAT_MODE_GAMEPAD ? 1 : 65, right, error);
}
