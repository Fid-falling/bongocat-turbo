/*
 * Live2D 清单(model3.json)校验: 结构字段, 以及每个被引用资源是否真的落在包目录里。
 * 两条调用路径: 导入时 allow_missing_optional = false(严格, 缺文件即拒绝); 识别已装或
 * 内置模型时 = true(只做结构与路径安全校验, 允许资源缺失)。
 * 安全职责: safe_reference 拒绝包外引用, 同类检查在 Mver 策略与指针覆盖层里各有一份,
 * 三处规则必须一致。本文件只读盘, 不写任何东西。
 */
#include "model_import_manifest.h"
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/image.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

/* 只允许包内相对路径: 拒绝空值、以 / 或 \ 开头、含冒号(盘符与 URI)以及任何一段为
   ".." 的路径。逐段扫描是必要的, "a/../../b" 这种夹在中间的穿越同样要拦下。 */
static bool safe_reference(const char *value) {
    if (!value || !value[0] || value[0] == '/' || value[0] == '\\' ||
        strchr(value, ':')) return false;
    const char *part = value;
    while (*part) {
        while (*part == '/' || *part == '\\') part++;
        if (part[0] == '.' && part[1] == '.' &&
            (!part[2] || part[2] == '/' || part[2] == '\\')) return false;
        part = strpbrk(part, "/\\");
        if (!part) break;
    }
    return true;
}
static bool referenced_file(const char *root, const char *relative) {
    char path[BONGO_CAT_PATH_CAP];
    return safe_reference(relative) &&
        bongo_cat_path_join(path, sizeof(path), root, relative) &&
        bongo_cat_path_is_file(path);
}

/* 纹理只存在还不够: 必须能被 stb 从文件头读出有效宽高(用 bongo_cat_image_info 探测),
   否则 0 字节或损坏的 PNG 会一路混到渲染阶段才失败。 */
static bool referenced_texture(const char *root, const char *relative) {
    char path[BONGO_CAT_PATH_CAP];
    if (!safe_reference(relative) ||
        !bongo_cat_path_join(path, sizeof(path), root, relative)) return false;
    return bongo_cat_image_info(path, NULL, NULL);
}

/* 字段缺失视为通过; allow_missing 为真时只校验路径安全性, 不要求文件存在(已装或内置
   模型允许缺可选资源)。 */
static bool optional_reference(const char *root, yyjson_val *refs,
    const char *name, bool allow_missing) {
    yyjson_val *value = yyjson_obj_get(refs, name);
    if (!value) return true;
    const char *relative = yyjson_get_str(value);
    return relative && (allow_missing ? (!relative[0] || safe_reference(relative))
        : referenced_file(root, relative));
}

/* Expressions 与 Motions 里的 File 走与主清单相同的落地检查; Motion 的 Sound 只要求
   路径安全 —— 音频缺失不算包损坏, 最多是没声音。 */
static bool behavior_references(const char *root, yyjson_val *refs,
    bool allow_missing) {
    yyjson_val *expressions = yyjson_obj_get(refs, "Expressions");
    if (expressions && !yyjson_is_arr(expressions)) return false;
    size_t index, count; yyjson_val *item;
    yyjson_arr_foreach(expressions, index, count, item) {
        const char *file = yyjson_get_str(yyjson_obj_get(item, "File"));
        if (!(allow_missing ? safe_reference(file) : referenced_file(root, file)))
            return false;
    }
    yyjson_val *motions = yyjson_obj_get(refs, "Motions");
    if (motions && !yyjson_is_obj(motions)) return false;
    size_t group_index, group_count; yyjson_val *key, *group;
    yyjson_obj_foreach(motions, group_index, group_count, key, group) {
        if (!yyjson_is_arr(group)) return false;
        yyjson_arr_foreach(group, index, count, item) {
            const char *file = yyjson_get_str(yyjson_obj_get(item, "File"));
            if (!(allow_missing ? safe_reference(file) : referenced_file(root, file)))
                return false;
            const char *sound = yyjson_get_str(yyjson_obj_get(item, "Sound"));
            if (sound && !(allow_missing && !sound[0]) && !safe_reference(sound))
                return false;
        }
    }
    return true;
}

/* 纯函数: 不读盘, 也不接管 document 的生命周期(调用方负责 yyjson_doc_free)。
   Version 必须是 3, 即 Cubism 3/4/5 的 model3 格式。 */
bool bongo_cat_import_manifest_document_valid(const char *root,
    yyjson_doc *document, bool allow_missing_optional) {
    yyjson_val *manifest = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *refs = yyjson_is_obj(manifest)
        ? yyjson_obj_get(manifest, "FileReferences") : NULL;
    const char *moc = yyjson_get_str(yyjson_obj_get(refs, "Moc"));
    yyjson_val *textures = yyjson_obj_get(refs, "Textures");
    bool valid = yyjson_get_int(yyjson_obj_get(manifest, "Version")) == 3 &&
        yyjson_is_obj(refs) && referenced_file(root, moc) && yyjson_is_arr(textures) &&
        yyjson_arr_size(textures) > 0;
    size_t index, maximum; yyjson_val *texture;
    yyjson_arr_foreach(textures, index, maximum, texture)
        valid = valid && referenced_texture(root, yyjson_get_str(texture));
    valid = valid && optional_reference(root, refs, "Physics", allow_missing_optional) &&
        optional_reference(root, refs, "Pose", allow_missing_optional) &&
        optional_reference(root, refs, "DisplayInfo", allow_missing_optional) &&
        (!allow_missing_optional || optional_reference(root, refs, "UserData", true)) &&
        behavior_references(root, refs, allow_missing_optional);
    return valid;
}

/* 打开失败与内容非法一律返回 false 并写 FORMAT 错误(消息里带清单路径);
   allow_missing_optional 固定为 false —— 这是导入前的严格校验。 */
bool bongo_cat_import_manifest_valid(const char *root, const char *setting,
    BongoCatError *error) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), root, setting)) return false;
    FILE *file = bongo_cat_file_open(path, "rb");
    yyjson_doc *document = file ? yyjson_read_fp(file, 0, NULL, NULL) : NULL;
    if (file) fclose(file);
    bool valid = bongo_cat_import_manifest_document_valid(root, document, false);
    yyjson_doc_free(document);
    if (!valid && error) bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "Model manifest or referenced assets are invalid: %s", path);
    return valid;
}
