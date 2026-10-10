#include "config_internal.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

/*
 * session.json 的读写。session 是"可以丢"的临时状态: 上次退出时的窗口位置/
 * 尺寸/缩放/不透明度、当前模型、以及已启用的模型动作。
 *
 * 与 settings.json 的分工: 这里读失败不必打扰用户, 缺字段就沿用调用方传入的
 * 默认值; 但写入依然走 config_file.c 的原子替换, 免得留下半个文件。
 *
 * 不变量: 与 config_io.c 相同 —— 先读进副本 loaded, 全部通过才赋值; 保存时也
 * 先规范化副本, 不修改调用方传进来的结构。
 */
#define SESSION_FORMAT "bongocat/session"
#define type_error(error, field, expected) bongo_cat_config_type_error( \
    "Session", error, field, expected)
#define read_value(object, key, target, error) bongo_cat_config_read_value( \
    "Session", object, key, target, error)
#define read_object(object, key, target, error) bongo_cat_config_read_object( \
    "Session", object, key, target, error)
#define read_array(object, key, target, error) bongo_cat_config_read_array( \
    "Session", object, key, target, error)
#define read_bool(object, key, target, error) bongo_cat_config_read_bool( \
    "Session", object, key, target, error)
/* 这里的 read_int 保留 required 开关: 窗口位置和尺寸要么整组存在、要么整组
   不存在, 只存一半坐标没有意义, 所以那几个字段按必读处理(见下面的 load)。 */
#define read_int(object, key, target, required, error) \
    bongo_cat_config_read_int( \
        "Session", object, key, target, required, error)
#define read_float(object, key, target, error) bongo_cat_config_read_float( \
    "Session", object, key, target, error)
#define read_string(object, key, target, length, error) \
    bongo_cat_config_read_string( \
        "Session", object, key, target, length, error)
#define copy_text(target, capacity, value, length, field, error) \
    bongo_cat_config_copy_text( \
        "Session", target, capacity, value, length, field, error)

/* 键不存在时保留调用方原有的 active_model_id(可能是空串); 但键一旦存在就
   必须是长度合法的非空字符串 —— 空串算损坏, 不能当成"还没选模型"。 */
static bool read_active_model(yyjson_val *root,
    BongoCatSessionState *session, BongoCatError *error) {
    yyjson_val *value;
    if (!read_value(root, "activeModelId", &value, error)) return false;
    if (!value) return true;
    if (!yyjson_is_str(value))
        return type_error(error, "activeModelId", "a string");
    const char *text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    if (!length || length >= sizeof(session->active_model_id) ||
        strlen(text) != length)
        return type_error(error, "activeModelId",
            "a non-empty string within the supported length");
    memset(session->active_model_id, 0, sizeof(session->active_model_id));
    memcpy(session->active_model_id, text, length);
    return true;
}

static bool read_active_behaviors(yyjson_val *array,
    BongoCatSessionState *session, BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        return type_error(error, "activeBehaviors", "a smaller array");
    session->active_behavior_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        if (!yyjson_is_obj(item))
            return type_error(error, "activeBehaviors[]", "an object");
        const char *model_id, *behavior_id;
        size_t model_length, behavior_length;
        if (!read_string(item, "modelId", &model_id, &model_length,
                error) ||
            !read_string(item, "behaviorId", &behavior_id,
                &behavior_length, error)) return false;
        if (!model_id || !model_length || !behavior_id || !behavior_length)
            return type_error(error, "activeBehaviors[]",
                "non-empty modelId and behaviorId strings");
        BongoCatActiveBehavior *entry = &session->active_behaviors[
            session->active_behavior_count++];
        memset(entry, 0, sizeof(*entry));
        if (!copy_text(entry->model_id, sizeof(entry->model_id), model_id,
                model_length, "modelId", error) ||
            !copy_text(entry->behavior_id, sizeof(entry->behavior_id),
                behavior_id, behavior_length, "behaviorId", error))
            return false;
    }
    return true;
}

static bool read_additional_models(yyjson_val *array,
    BongoCatSessionState *session, BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_ADDITIONAL_MODEL_CAP)
        return type_error(error, "additionalModelIds", "a smaller array");
    session->additional_model_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        if (!yyjson_is_str(item) || !yyjson_get_len(item) ||
            yyjson_get_len(item) >= BONGO_CAT_ID_CAP ||
            strlen(yyjson_get_str(item)) != yyjson_get_len(item))
            return type_error(error, "additionalModelIds[]",
                "a non-empty model id within the supported length");
        snprintf(session->additional_model_ids[
            session->additional_model_count++], BONGO_CAT_ID_CAP, "%s",
            yyjson_get_str(item));
    }
    return true;
}

/*
 * 读 session.json。position 是可选的整组: 只有 x/y 都读成功才置 position_known,
 * 否则窗口位置交回平台层决定。size 里 width/height 必读, contentWidth/
 * contentHeight 可选 —— 老版本只存外框尺寸, 这时内容尺寸回落到外框尺寸。
 */
BongoCatResult bongo_cat_session_load(const char *path,
    BongoCatSessionState *session, BongoCatError *error) {
    if (!path || !session) return BONGO_CAT_ERROR_ARGUMENT;
    if (!bongo_cat_path_is_file(path)) return BONGO_CAT_OK;
    yyjson_doc *document = NULL;
    BongoCatResult result = bongo_cat_config_read_document(path,
        SESSION_FORMAT, BONGO_CAT_SESSION_SCHEMA, &document, error);
    if (result != BONGO_CAT_OK) return result;
    BongoCatSessionState loaded = *session;
    yyjson_val *root = yyjson_doc_get_root(document);
    yyjson_val *window = NULL;
    yyjson_val *active_behaviors = NULL;
    yyjson_val *additional_models = NULL;
    bool valid = read_object(root, "window", &window, error) &&
        read_array(root, "activeBehaviors", &active_behaviors, error) &&
        read_array(root, "additionalModelIds", &additional_models, error);
    if (valid && window) {
        valid = read_bool(window, "visible", &loaded.window.visible, error) &&
            read_float(window, "scalePercent",
                &loaded.window.scale_percent, error) &&
            read_float(window, "opacityPercent",
                &loaded.window.opacity_percent, error);
        yyjson_val *position = NULL;
        yyjson_val *size = NULL;
        if (valid) valid = read_object(
            window, "position", &position, error);
        if (valid) valid = read_object(window, "size", &size, error);
        if (valid && position) {
            valid = read_int(position, "x", &loaded.window.x, true, error) &&
                read_int(position, "y", &loaded.window.y, true, error);
            if (valid) loaded.window.position_known = true;
        }
        if (valid && size) {
            int content_width = 0, content_height = 0;
            valid = read_int(size, "width", &loaded.window.width, true,
                    error) &&
                read_int(size, "height", &loaded.window.height, true, error) &&
                read_int(size, "contentWidth", &content_width, false, error) &&
                read_int(size, "contentHeight", &content_height, false, error);
            if (valid) {
                loaded.window.content_width = content_width > 0
                    ? content_width : loaded.window.width;
                loaded.window.content_height = content_height > 0
                    ? content_height : loaded.window.height;
            }
        }
    }
    if (valid) valid = read_active_model(root, &loaded, error) &&
        read_int(root, "lastUpdateCheckDay",
            &loaded.last_update_check_day, false, error) &&
        bongo_cat_config_read_text("Session", root,
            "lastUpdateCheckVersion", loaded.last_update_check_version,
            sizeof(loaded.last_update_check_version), error) &&
        bongo_cat_config_read_text("Session", root,
            "availableUpdateVersion", loaded.available_update_version,
            sizeof(loaded.available_update_version), error) &&
        read_additional_models(additional_models, &loaded, error) &&
        read_active_behaviors(active_behaviors, &loaded, error);
    yyjson_doc_free(document);
    if (!valid) return BONGO_CAT_ERROR_FORMAT;
    bongo_cat_session_validate(&loaded);
    *session = loaded;
    return BONGO_CAT_OK;
}

/* 保存路径上的失败基本都是 yyjson 分配失败(文档只在内存里构建), 所以统一用
   同一个错误信息。 */
static BongoCatResult build_error(BongoCatError *error) {
    bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
        "Cannot allocate session JSON");
    return BONGO_CAT_ERROR_MEMORY;
}

/* 两个 write_* 都是"先把数组挂到对象上再逐项填"的写法: yyjson 里数组必须先
   成为文档的一部分才能继续添加元素; 失败返回 false, 由调用方统一回收文档。 */
static bool write_active_behaviors(yyjson_mut_doc *doc,
    yyjson_mut_val *root, const BongoCatSessionState *session) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    if (!array || !yyjson_mut_obj_add_val(
            doc, root, "activeBehaviors", array)) return false;
    for (size_t i = 0; i < session->active_behavior_count; ++i) {
        const BongoCatActiveBehavior *entry =
            &session->active_behaviors[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        if (!item || !yyjson_mut_obj_add_strcpy(
                doc, item, "modelId", entry->model_id) ||
            !yyjson_mut_obj_add_strcpy(
                doc, item, "behaviorId", entry->behavior_id) ||
            !yyjson_mut_arr_add_val(array, item)) return false;
    }
    return true;
}

static bool write_additional_models(yyjson_mut_doc *doc,
    yyjson_mut_val *root, const BongoCatSessionState *session) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    if (!array || !yyjson_mut_obj_add_val(
            doc, root, "additionalModelIds", array)) return false;
    for (size_t i = 0; i < session->additional_model_count; ++i)
        if (!yyjson_mut_arr_add_strcpy(
                doc, array, session->additional_model_ids[i])) return false;
    return true;
}

/*
 * 保存前先对本地副本 canonical 调用 validate: 落盘的值必须满足不变量, 但不应该
 * "顺手"改掉调用方正在用的结构。
 */
BongoCatResult bongo_cat_session_save(const char *path,
    const BongoCatSessionState *session, BongoCatError *error) {
    if (!path || !session) return BONGO_CAT_ERROR_ARGUMENT;
    BongoCatSessionState canonical = *session;
    bongo_cat_session_validate(&canonical);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return build_error(error);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        return build_error(error);
    }
    yyjson_mut_doc_set_root(doc, root);
    bool built = yyjson_mut_obj_add_strcpy(
            doc, root, "format", SESSION_FORMAT) &&
        yyjson_mut_obj_add_int(doc, root, "schemaVersion",
            BONGO_CAT_SESSION_SCHEMA);
    yyjson_mut_val *window = built ?
        yyjson_mut_obj_add_obj(doc, root, "window") : NULL;
    built = built && window &&
        yyjson_mut_obj_add_bool(doc, window, "visible",
            canonical.window.visible) &&
        yyjson_mut_obj_add_real(doc, window, "scalePercent",
            canonical.window.scale_percent) &&
        yyjson_mut_obj_add_real(doc, window, "opacityPercent",
            canonical.window.opacity_percent);
    if (built && canonical.window.position_known) {
        yyjson_mut_val *position =
            yyjson_mut_obj_add_obj(doc, window, "position");
        built = position &&
            yyjson_mut_obj_add_int(doc, position, "x", canonical.window.x) &&
            yyjson_mut_obj_add_int(doc, position, "y", canonical.window.y);
    }
    yyjson_mut_val *size = built ?
        yyjson_mut_obj_add_obj(doc, window, "size") : NULL;
    built = built && size &&
        yyjson_mut_obj_add_int(doc, size, "width", canonical.window.width) &&
        yyjson_mut_obj_add_int(doc, size, "height", canonical.window.height) &&
        yyjson_mut_obj_add_int(doc, size, "contentWidth",
            canonical.window.content_width) &&
        yyjson_mut_obj_add_int(doc, size, "contentHeight",
            canonical.window.content_height) &&
        yyjson_mut_obj_add_strcpy(doc, root, "activeModelId",
            canonical.active_model_id) &&
        yyjson_mut_obj_add_int(doc, root, "lastUpdateCheckDay",
            canonical.last_update_check_day) &&
        yyjson_mut_obj_add_strcpy(doc, root, "lastUpdateCheckVersion",
            canonical.last_update_check_version) &&
        yyjson_mut_obj_add_strcpy(doc, root, "availableUpdateVersion",
            canonical.available_update_version) &&
        write_additional_models(doc, root, &canonical) &&
        write_active_behaviors(doc, root, &canonical);
    if (!built) {
        yyjson_mut_doc_free(doc);
        return build_error(error);
    }
    BongoCatResult result = bongo_cat_config_write_document(path, doc,
        "session file", error);
    yyjson_mut_doc_free(doc);
    return result;
}
