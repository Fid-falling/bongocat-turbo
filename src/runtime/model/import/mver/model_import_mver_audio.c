/*
 * Mver 音效素材转换: 按 sounds 下标找 assets/sounds/<下标>.wav|ogg|flac, 复制到 target/resources/sounds/,
 * 并追加一条 kind=sound 绑定 (绑定里的声音名取自源文件名)。时序: 由 bongo_cat_import_adapter_metadata 在
 * add_behaviors 之后调用; 只往调用方的 items 数组追加条目, 文档与 items 的所有权仍归调用方 (与特效生成同一约定)。
 * 契约: 缺音频文件的行静默跳过 (音效是可选素材), 但键位解析失败是硬错误。
 */
#include "model_import_mver_internal.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <yyjson.h>

/* 按下标探测三种扩展名 (wav 优先): 命中就写绝对源路径与写进绑定的相对路径 resources/sounds/<下标>.<扩展名>;
 * 三个都没有则返回 false, 此时两个输出缓冲区内容未定义。 */
static bool sound_source(const BongoCatImportCandidate *candidate, size_t index,
    char *source, size_t capacity, char *relative, size_t relative_capacity) {
    static const char *extensions[] = {"wav", "ogg", "flac"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        char name[40], sounds[BONGO_CAT_PATH_CAP];
        snprintf(name, sizeof(name), "%zu.%s", index, extensions[i]);
        if (!bongo_cat_path_join(sounds, sizeof(sounds), candidate->assets, "sounds") ||
            !bongo_cat_path_join(source, capacity, sounds, name) ||
            !bongo_cat_path_is_file(source)) continue;
        snprintf(relative, relative_capacity, "resources/sounds/%s", name);
        return true;
    }
    return false;
}

/* 触发条件刻意只看"存在且不是空数组": 这条绑定不带快捷键 (见下), 行内容没有可写的位置, 强行解析只会把
 * 占位写法 (字符串或数字) 变成硬错误。 */
static bool add_sound_clear(yyjson_mut_doc *output, yyjson_mut_val *items,
    yyjson_val *config) {
    yyjson_val *decoration = yyjson_obj_get(config, "decoration");
    yyjson_val *row = yyjson_obj_get(decoration, "soundClear");
    if (!row || (yyjson_is_arr(row) && yyjson_arr_size(row) == 0)) return true;
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(output, items);
    /* Stopping all audio is an explicit command and has no default shortcut. */
    return item && yyjson_mut_obj_add_str(output, item, "kind", "sound-clear");
}

/* 音效绑定生成入口: rows 为 null 表示该模式没有音效 (宽松处理, 与素材步一致), 非数组才算错误。副作用: 建
 * target/resources/sounds 目录并复制音频, 往 items 追加 kind=sound 条目 (overlap 取配置的 decoration.soundKeep,
 * 缺失时按 true 处理), 只在确实写出音效后追加一条 sound-clear; 下标是配置下标, 缺文件的行不改变其它行编号。 */
bool bongo_cat_mver_add_audio(void *raw_output, void *raw_items,
    void *raw_config, void *raw_rows,
    const BongoCatImportCandidate *candidate, const BongoCatMverLabels *labels,
    const char *target) {
    yyjson_mut_doc *output = raw_output;
    yyjson_mut_val *items = raw_items;
    yyjson_val *config = raw_config, *rows = raw_rows;
    if (!rows || yyjson_is_null(rows)) return true;
    if (!yyjson_is_arr(rows)) return false;
    char target_resources[BONGO_CAT_PATH_CAP], target_sounds[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(target_resources, sizeof(target_resources), target, "resources") ||
        !bongo_cat_path_join(target_sounds, sizeof(target_sounds), target_resources, "sounds") ||
        !bongo_cat_path_create_directory(target_sounds)) return false;
    yyjson_val *decoration = yyjson_obj_get(config, "decoration");
    yyjson_val *keep_value = yyjson_obj_get(decoration, "soundKeep");
    bool keep = !keep_value || yyjson_get_bool(keep_value);
    size_t emitted = 0, index, count; yyjson_val *row;
    yyjson_arr_foreach(rows, index, count, row) {
        char shortcut[BONGO_CAT_SHORTCUT_CAP], source[BONGO_CAT_PATH_CAP];
        char relative[BONGO_CAT_PATH_CAP], destination[BONGO_CAT_PATH_CAP];
        if (yyjson_is_null(row) || (yyjson_is_arr(row) && !yyjson_arr_size(row)))
            continue;
        if (!sound_source(candidate, index, source, sizeof(source), relative,
            sizeof(relative))) continue;
        if (!bongo_cat_mver_sound_chord(row, shortcut, sizeof(shortcut))) return false;
        const char *name = bongo_cat_path_name(source);
        if (!bongo_cat_path_join(destination, sizeof(destination), target_sounds, name) ||
            !bongo_cat_path_copy_file(source, destination)) return false;
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(output, items);
        const char *label = bongo_cat_mver_label(labels, "sounds", index);
        if (!item || !yyjson_mut_obj_add_str(output, item, "kind", "sound") ||
            !yyjson_mut_obj_add_strcpy(output, item, "shortcut", shortcut) ||
            !yyjson_mut_obj_add_strcpy(output, item, "sound", relative) ||
            (label && !yyjson_mut_obj_add_strcpy(output, item, "label", label)) ||
            !yyjson_mut_obj_add_bool(output, item, "overlap", keep)) return false;
        emitted++;
    }
    return !emitted || add_sound_clear(output, items, config);
}

