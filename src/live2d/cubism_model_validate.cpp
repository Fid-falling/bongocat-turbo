/*
 * model3.json 的预校验层。
 *
 * 模型是用户可下载、可导入的数据, 而 SDK 的 CubismModelSettingJson 会无条件相信里面的
 * 路径和数组长度: 一条 "../../../x" 就能让加载器读到模型目录之外, 一个声明了上万条动作
 * 的数组就能把内存吃光。所以在把 JSON 交给 SDK 之前先在这里过一遍 —— 宁可判成"格式
 * 错误", 也不要让坏数据进到 SDK 里变成越界读。
 *
 * 这里只做结构与上限检查, 不检查文件是否真的存在: 缺文件由各加载路径自己报错。
 */
#include "cubism_model.hpp"

#include <cstring>
#include <yyjson.h>

namespace bongo_cat {
namespace {

/* 路径安全: 只接受相对路径, 任何一段 ".." 都不允许。同时拒绝绝对路径、盘符 (冒号)
 * 与 UNC 前缀, 这样模型包无法指向自己目录之外的任何东西。 */
bool safe_path_value(yyjson_val *value) {
    const char *path = yyjson_is_str(value) ? yyjson_get_str(value) : nullptr;
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' ||
        std::strchr(path, ':')) return false;
    const char *part = path;
    while (*part) {
        while (*part == '/' || *part == '\\') ++part;
        if (part[0] == '.' && part[1] == '.' &&
            (!part[2] || part[2] == '/' || part[2] == '\\')) return false;
        const char *next = std::strpbrk(part, "/\\");
        if (!next) break;
        part = next;
    }
    return true;
}

/* 可选字段的语义: 字段不存在、或是空字符串都算合法 (SDK 用空串表示"没有这个文件"),
 * 但只要非空就必须是安全的相对路径。 */
bool optional_path(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return !value || (yyjson_is_str(value) && !yyjson_get_str(value)[0]) ||
        safe_path_value(value);
}

bool optional_number(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return !value || yyjson_is_num(value);
}

/* 下面这些上限不是按"正常模型有多大"定的, 而是按"坏数据最多能让我们分配多少"定的;
 * 真实模型离这些数字很远 (内置图集最多几十张)。 */
bool valid_textures(yyjson_val *value) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0 ||
        yyjson_arr_size(value) > 256) return false;
    size_t index, count; yyjson_val *item;
    yyjson_arr_foreach(value, index, count, item)
        if (!safe_path_value(item)) return false;
    return true;
}

bool valid_expressions(yyjson_val *value) {
    if (!value) return true;
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 1024) return false;
    size_t index, count; yyjson_val *item;
    yyjson_arr_foreach(value, index, count, item) {
        if (!yyjson_is_obj(item) || !yyjson_is_str(yyjson_obj_get(item, "Name")) ||
            !safe_path_value(yyjson_obj_get(item, "File"))) return false;
    }
    return true;
}

bool valid_motion(yyjson_val *item) {
    return yyjson_is_obj(item) && safe_path_value(yyjson_obj_get(item, "File")) &&
        optional_path(item, "Sound") && optional_number(item, "FadeInTime") &&
        optional_number(item, "FadeOutTime");
}

bool valid_motions(yyjson_val *value) {
    if (!value) return true;
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) > 1024) return false;
    size_t index, maximum; yyjson_val *key, *group;
    yyjson_obj_foreach(value, index, maximum, key, group) {
        if (!yyjson_is_str(key) || !yyjson_is_arr(group) ||
            yyjson_arr_size(group) > 4096) return false;
        size_t item_index, count; yyjson_val *item;
        yyjson_arr_foreach(group, item_index, count, item)
            if (!valid_motion(item)) return false;
    }
    return true;
}

bool valid_groups(yyjson_val *value) {
    if (!value) return true;
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 1024) return false;
    size_t index, count; yyjson_val *group;
    yyjson_arr_foreach(value, index, count, group) {
        yyjson_val *ids = yyjson_obj_get(group, "Ids");
        if (!yyjson_is_obj(group) || !yyjson_is_str(yyjson_obj_get(group, "Target")) ||
            !yyjson_is_str(yyjson_obj_get(group, "Name")) || !yyjson_is_arr(ids) ||
            yyjson_arr_size(ids) > 4096) return false;
        size_t id_index, id_count; yyjson_val *id;
        yyjson_arr_foreach(ids, id_index, id_count, id)
            if (!yyjson_is_str(id)) return false;
    }
    return true;
}

bool valid_hit_areas(yyjson_val *value) {
    if (!value) return true;
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 1024) return false;
    size_t index, count; yyjson_val *item;
    yyjson_arr_foreach(value, index, count, item)
        if (!yyjson_is_obj(item) || !yyjson_is_str(yyjson_obj_get(item, "Id")) ||
            !yyjson_is_str(yyjson_obj_get(item, "Name"))) return false;
    return true;
}

bool valid_layout(yyjson_val *value) {
    if (!value) return true;
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) > 64) return false;
    size_t index, maximum; yyjson_val *key, *item;
    yyjson_obj_foreach(value, index, maximum, key, item)
        if (!yyjson_is_str(key) || !yyjson_is_num(item)) return false;
    return true;
}

bool valid_references(yyjson_val *value) {
    return yyjson_is_obj(value) && safe_path_value(yyjson_obj_get(value, "Moc")) &&
        valid_textures(yyjson_obj_get(value, "Textures")) &&
        optional_path(value, "Physics") && optional_path(value, "Pose") &&
        optional_path(value, "DisplayInfo") && optional_path(value, "UserData") &&
        valid_expressions(yyjson_obj_get(value, "Expressions")) &&
        valid_motions(yyjson_obj_get(value, "Motions"));
}

} // namespace

bool validate_model_setting_json(const std::vector<unsigned char> &json,
    const char *setting_file, BongoCatError *error) {
    if (json.empty() || json.size() > 4 * 1024 * 1024) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Model setting is empty or too large: %s", setting_file ? setting_file : "");
        return false;
    }
    yyjson_read_err parse_error = {};
    yyjson_doc *document = yyjson_read_opts(
        reinterpret_cast<char *>(const_cast<unsigned char *>(json.data())),
        json.size(), 0, nullptr, &parse_error);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : nullptr;
    /* 只接受 Version 3: 更低版本缺 Offscreen/BlendShape 之类的字段, SDK 要到运行时才会
     * 以空指针的形式炸掉, 不如在这里一次判死, 错误信息也更清楚。 */
    bool valid = yyjson_is_obj(root) && yyjson_is_int(yyjson_obj_get(root, "Version")) &&
        yyjson_get_int(yyjson_obj_get(root, "Version")) == 3 &&
        valid_references(yyjson_obj_get(root, "FileReferences")) &&
        valid_groups(yyjson_obj_get(root, "Groups")) &&
        valid_hit_areas(yyjson_obj_get(root, "HitAreas")) &&
        valid_layout(yyjson_obj_get(root, "Layout"));
    if (document) yyjson_doc_free(document);
    if (valid) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "Invalid model setting JSON: %s (%s)", setting_file ? setting_file : "",
        parse_error.msg ? parse_error.msg : "unsupported model3 schema");
    return false;
}

} // namespace bongo_cat
