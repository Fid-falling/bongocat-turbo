/*
 * 导入兼容性报告: 在适配器目录写 .bongo-cat-import-report.json, 说明本次导入保留了哪些能力、
 * 哪些 Mver 配置被有意丢弃, 以及素材与音效的声明数与磁盘实际可用数。
 * 位置: model_import_adapter.c 适配步骤表的最后一步(素材复制与元数据之后、模式标记之前)。
 * 契约: 只供人诊断, 运行时不再读回, 但 schemaVersion 与字段名属于对外格式, 改了等于破坏外部工具。
 * 最容易改错: Mver 候选必须能读出 config 清单, 缺它整个步骤失败; Tauri 候选反而没有 config,
 * 两条路径的字段集合并不相同。
 */
#include "model_import.h"
#include "mver/model_import_mver_manifest.h"
#include "bongo_cat/file.h"
#include "bongo_cat/json.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

#define IMPORT_REPORT ".bongo-cat-import-report.json"

/* 清单第 index 项素材在磁盘上是否存在: 补丁候选优先查 overrides/<group>, 没有就回退
 * assets/<group>; 文件名约定是 "<index>.png", 与 Mver 素材转换的产物同名。只做存在性判断。 */
static bool source_asset(const BongoCatImportCandidate *candidate,
    const char *group, size_t index) {
    char name[32], directory[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
    snprintf(name, sizeof(name), "%zu.png", index);
    if (candidate->overrides[0] &&
        bongo_cat_path_join(directory, sizeof(directory), candidate->overrides, group) &&
        bongo_cat_path_join(path, sizeof(path), directory, name) &&
        bongo_cat_path_is_file(path)) return true;
    return bongo_cat_path_join(directory, sizeof(directory), candidate->assets, group) &&
        bongo_cat_path_join(path, sizeof(path), directory, name) &&
        bongo_cat_path_is_file(path);
}

/* 同上的音效版本: 音效后缀不固定(.wav/.ogg/.flac), 按后缀顺序找第一个存在的文件; 只查
 * assets/sounds, 补丁的 overrides 不参与音效查找。 */
static bool source_sound(const BongoCatImportCandidate *candidate, size_t index) {
    static const char *extensions[] = {"wav", "ogg", "flac"};
    char directory[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP], name[32];
    if (!bongo_cat_path_join(directory, sizeof(directory), candidate->assets,
        "sounds")) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        snprintf(name, sizeof(name), "%zu.%s", index, extensions[i]);
        if (bongo_cat_path_join(path, sizeof(path), directory, name) &&
            bongo_cat_path_is_file(path)) return true;
    }
    return false;
}

/* 写一个素材组的统计对象: declared 是清单数组长度, available 是磁盘上真实存在的数量,
 * missing = declared - available, 因此素材缺失是"记录"而不是错误。
 * 键不存在时整个条目都不写(该模式没有这类素材); 键存在但值为 null 或非数组时按声明 0 个处理,
 * 与 mver 素材转换里对 null 的宽松处理保持一致。
 * 返回 false 只是写 JSON 失败, 不代表素材缺失。 */
static bool add_stat(yyjson_mut_doc *output, yyjson_mut_val *stats,
    yyjson_val *mode, const BongoCatImportCandidate *candidate,
    const char *key, const char *directory, bool sound) {
    yyjson_val *rows = yyjson_obj_get(mode, key);
    if (!rows) return true;
    /*
     * Mver 配置用 null (或其它非数组值) 表示"这个模式没有这类素材", 例如没有
     * 手部贴图的模型。这不是格式错误: 记 0 个素材即可, 与
     * bongo_cat_import_mver_assets 里对 null 的宽松处理保持一致。以前这里直接
     * 返回 false, 会让整个 adapter 生成失败, 结果是导入的模型没有预览图、
     * 也渲染不出来。
     */
    size_t declared = yyjson_is_arr(rows) ? yyjson_arr_size(rows) : 0;
    size_t available = 0;
    for (size_t i = 0; i < declared; ++i)
        if (sound ? source_sound(candidate, i) : source_asset(candidate, directory, i))
            available++;
    yyjson_mut_val *value = yyjson_mut_obj_add_obj(output, stats, key);
    return value && yyjson_mut_obj_add_uint(output, value, "declared", declared) &&
        yyjson_mut_obj_add_uint(output, value, "available", available) &&
        yyjson_mut_obj_add_uint(output, value, "missing", declared - available);
}

/* 追加一条降级记录: field 是配置字段路径(如 decoration.window), reason 是给用户看的解释。 */
static bool add_degradation(yyjson_mut_doc *output, yyjson_mut_val *items,
    const char *field, const char *reason) {
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(output, items);
    return item && yyjson_mut_obj_add_strcpy(output, item, "field", field) &&
        yyjson_mut_obj_add_strcpy(output, item, "reason", reason);
}

/* 清单里显式出现该键才算"配置过": 值为 null 也算出现, 因为 Mver 用 null 表示关闭这一项。 */
static bool configured(yyjson_val *object, const char *key) {
    return yyjson_is_obj(object) && yyjson_obj_get(object, key) != NULL;
}

/* 比对 Mver 清单里 BongoCat 有意不采纳的配置, 生成降级清单: 窗口尺寸与置顶(属于跨平台用户
 * 偏好)、精灵像素偏移(绑定 Mver 固定画布)、手柄摇杆偏移(本作使用归一化轴)。 */
static bool add_degradations(yyjson_mut_doc *output, yyjson_mut_val *items,
    yyjson_val *config, const BongoCatImportCandidate *candidate) {
    yyjson_val *decoration = yyjson_obj_get(config, "decoration");
    yyjson_val *mode = yyjson_obj_get(config, bongo_cat_mode_name(candidate->mode));
    bool ok = true;
    if (configured(decoration, "window_size") || configured(decoration, "topWindow"))
        ok = add_degradation(output, items, "decoration.window",
            "BongoCat keeps window size and always-on-top as cross-platform user preferences");
    if (ok && (configured(decoration, "offsetX") || configured(decoration, "offsetY") ||
        configured(decoration, "scalar") || configured(decoration, "hand_offset")))
        ok = add_degradation(output, items, "decoration.sprite_geometry",
            "Mver pixel offsets are tied to its fixed canvas and are not applied to BongoCat layouts");
    if (ok && candidate->mode == BONGO_CAT_MODE_GAMEPAD &&
        (configured(mode, "stick_offset_L") || configured(mode, "stick_offset_R")))
        ok = add_degradation(output, items, "gamepad.stick_offsets",
            "BongoCat uses normalized controller axes rather than Mver sprite offsets");
    return ok;
}

/* 统计 FileReferences.Motions 引用的音效里有多少不在磁盘上(允许缺失, 只作提示)。
 * 两种格式读法不同: Tauri 走通用 JSON 读取, Mver 走带兼容修补的清单读取; document 为 NULL 时
 * 后续查询靠 yyjson 的空指针容忍返回 NULL, 因此不需要提前分支。 */
static size_t missing_motion_sounds(const BongoCatImportCandidate *candidate) {
    char manifest_path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(manifest_path, sizeof(manifest_path),
        candidate->directory, candidate->setting)) return 0;
    yyjson_doc *document = candidate->format == BONGO_CAT_IMPORT_TAURI
        ? bongo_cat_json_read_file(manifest_path, 0, NULL)
        : bongo_cat_import_mver_manifest_read(manifest_path, NULL);
    yyjson_val *refs = document ? yyjson_obj_get(yyjson_doc_get_root(document),
        "FileReferences") : NULL;
    yyjson_val *motions = yyjson_obj_get(refs, "Motions");
    size_t missing = 0, group_index, group_count; yyjson_val *key, *group;
    yyjson_obj_foreach(motions, group_index, group_count, key, group) {
        size_t index, count; yyjson_val *item;
        yyjson_arr_foreach(group, index, count, item) {
            const char *sound = yyjson_get_str(yyjson_obj_get(item, "Sound"));
            char path[BONGO_CAT_PATH_CAP];
            if (sound && (!bongo_cat_path_join(path, sizeof(path),
                candidate->directory, sound) || !bongo_cat_path_is_file(path))) missing++;
        }
    }
    yyjson_doc_free(document);
    return missing;
}

/* format 字符串是报告对外格式的一部分, 改名会让外部工具认不出旧报告。 */
static const char *format_name(BongoCatImportFormat format) {
    if (format == BONGO_CAT_IMPORT_MVER) return "bongo-cat-mver";
    if (format == BONGO_CAT_IMPORT_MVER_PATCH) return "bongo-cat-mver-patch";
    return "tauri-live2d";
}

/* 报告入口: target 目录必须已存在(由适配步骤保证), 文件固定写到
 * target/.bongo-cat-import-report.json。
 * 只有 Mver 系候选(mver 为真)才做素材统计与降级比对, Tauri 候选没有对应的清单结构。
 * 前提: Mver 候选必须能读出 config 清单, 缺它整个步骤失败 —— 报告宁可失败也不写出半份字段。
 * 返回 false 表示生成或写盘失败, 错误码固定为 IO; 写坏的文件由 json 层负责删掉。 */
bool bongo_cat_import_write_report(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error) {
    yyjson_doc *source = candidate->config[0] ? bongo_cat_json_read_file(
        candidate->config, YYJSON_READ_JSON5 | YYJSON_READ_ALLOW_INVALID_UNICODE, NULL) : NULL;
    yyjson_val *config = source ? yyjson_doc_get_root(source) : NULL;
    yyjson_val *mode = yyjson_obj_get(config, bongo_cat_mode_name(candidate->mode));
    yyjson_mut_doc *output = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = output ? yyjson_mut_obj(output) : NULL;
    yyjson_mut_val *stats = root ? yyjson_mut_obj_add_obj(output, root, "assets") : NULL;
    yyjson_mut_val *capabilities = root ? yyjson_mut_obj_add_obj(output, root,
        "capabilities") : NULL;
    yyjson_mut_val *degraded = root ? yyjson_mut_obj_add_arr(output, root,
        "degradations") : NULL;
    if (output) yyjson_mut_doc_set_root(output, root);
    bool mver = candidate->format != BONGO_CAT_IMPORT_TAURI;
    bool ok = root && stats && capabilities && degraded &&
        yyjson_mut_obj_add_int(output, root, "schemaVersion", 1) &&
        yyjson_mut_obj_add_strcpy(output, root, "format", format_name(candidate->format)) &&
        yyjson_mut_obj_add_str(output, root, "runtimeProfile",
            mver ? "mver-0.1.6" : "native") &&
        yyjson_mut_obj_add_strcpy(output, root, "mode",
            bongo_cat_mode_name(candidate->mode)) &&
        yyjson_mut_obj_add_bool(output, capabilities, "live2dModel", true) &&
        yyjson_mut_obj_add_bool(output, capabilities, "previewAssets", true) &&
        yyjson_mut_obj_add_bool(output, capabilities, "sourceStructurePreserved", true) &&
        yyjson_mut_obj_add_bool(output, capabilities, "adapterIsolation", true) &&
        yyjson_mut_obj_add_bool(output, capabilities, "handInputImages", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "keyboardChords", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "gamepadButtons",
            mver && candidate->gamepad_buttons) &&
        yyjson_mut_obj_add_bool(output, capabilities, "expressionsAndMotions", true) &&
        yyjson_mut_obj_add_bool(output, capabilities, "shortcutAudio", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "imageEffects", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "mverRuntime", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "tauriAdapter", !mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "mverProjection", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "mverPointerDomain", mver) &&
        yyjson_mut_obj_add_bool(output, capabilities, "imagePatch",
            candidate->format == BONGO_CAT_IMPORT_MVER_PATCH) &&
        yyjson_mut_obj_add_uint(output, root, "optionalMissingMotionSounds",
            missing_motion_sounds(candidate));
    /* 键名不一致是 Mver 清单的历史约定: standard 模式的左手素材放在 hand 组, 且没有 righthand。 */
    if (ok && mver) {
        const char *left = candidate->mode == BONGO_CAT_MODE_STANDARD
            ? "hand" : "lefthand";
        ok = yyjson_is_obj(config) && yyjson_is_obj(mode) &&
            add_stat(output, stats, mode, candidate, left, left, false) &&
            (candidate->mode == BONGO_CAT_MODE_STANDARD ||
                add_stat(output, stats, mode, candidate, "righthand", "righthand", false)) &&
            add_stat(output, stats, mode, candidate, "keyboard", "keyboard", false) &&
            add_stat(output, stats, mode, candidate, "face", "face", false) &&
            add_stat(output, stats, mode, candidate, "sounds", "sounds", true) &&
            add_degradations(output, degraded, config, candidate);
    }
    if (ok) ok = yyjson_mut_obj_add_str(output, root, "status",
        yyjson_mut_arr_size(degraded) ? "imported-with-documented-degradations" : "imported");
    char path[BONGO_CAT_PATH_CAP];
    if (ok) ok = bongo_cat_path_join(path, sizeof(path), target, IMPORT_REPORT) &&
        bongo_cat_json_write_file(path, output, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(output);
    yyjson_doc_free(source);
    if (!ok) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot write model compatibility report");
    return ok;
}
