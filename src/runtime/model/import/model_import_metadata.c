/*
 * 适配元数据(.bongo-cat-adapter.json)的消费端: 行为快捷键绑定与 Mver 渲染参数。
 * 元数据由 Mver 适配器在导入或缓存时生成, 属于可选增强: 文件缺失、版本不符或字段非法
 * 都只是"没有增强", 不能让模型加载失败 —— 所以这里对错误一律静默返回。
 * 时序: 目录扫描与切换模型之后由 catalog 调 apply_metadata; 加载模型时读 render_options。
 * 最容易改错: 绑定 id 与字段默认值必须与生成端严格一致, 否则用户改过的快捷键会错位。
 */
#include "model_import.h"
#include "runtime.h"
#include "bongo_cat/json.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

/* schemaVersion == 1 且 kind == "bongo-cat-runtime-adapter" 才认作自己的元数据,
   否则整个文件忽略 —— 避免把别人放的 JSON 当成适配参数。 */
static bool metadata_version(yyjson_val *root) {
    if (!yyjson_is_obj(root)) return false;
    yyjson_val *schema = yyjson_obj_get(root, "schemaVersion");
    const char *kind = yyjson_get_str(yyjson_obj_get(root, "kind"));
    return yyjson_is_int(schema) &&
        yyjson_get_int(schema) == BONGO_CAT_MODEL_ADAPTER_SCHEMA && kind &&
        strcmp(kind, "bongo-cat-runtime-adapter") == 0;
}

/* "sound" 与 "sound-clear"、"effect" 与 "effect-clear" 视为同一族: 生成端把"清除绑定"
   也按顺序编号, 序号计数如果不合并同族, 读出来的 id 会与生成端整体错位。 */
static bool same_family(const char *left, const char *right) {
    bool left_effect = left && (strcmp(left, "effect") == 0 ||
        strcmp(left, "effect-clear") == 0);
    bool right_effect = right && (strcmp(right, "effect") == 0 ||
        strcmp(right, "effect-clear") == 0);
    bool left_sound = left && (strcmp(left, "sound") == 0 ||
        strcmp(left, "sound-clear") == 0);
    bool right_sound = right && (strcmp(right, "sound") == 0 ||
        strcmp(right, "sound-clear") == 0);
    return (left_effect && right_effect) || (left_sound && right_sound);
}

/* 序号 = 之前同族绑定的个数, 与生成端"按出现顺序编号"的规则对齐。O(n²) 但绑定数很少,
   换哈希表不值得。 */
static int sequential_index(yyjson_val *bindings, size_t index, const char *kind) {
    int result = 0;
    for (size_t before = 0; before < index; ++before) {
        yyjson_val *previous = yyjson_arr_get(bindings, before);
        const char *previous_kind = yyjson_get_str(yyjson_obj_get(previous, "kind"));
        if (same_family(previous_kind, kind)) result++;
    }
    return result;
}

/* 线性查找已有绑定; 返回 NULL 表示这是一条尚未存在的新绑定。 */
static BongoCatBehaviorShortcut *shortcut_for(BongoCatApp *app, const char *id) {
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i)
        if (strcmp(app->settings.behavior_shortcuts[i].id, id) == 0)
            return &app->settings.behavior_shortcuts[i];
    return NULL;
}

/* 生成持久化用的绑定 id, 必须与生成端逐字节一致: 声音与特效按同族序号, 表情按 index,
   动作按 group 加 index。返回 false 表示这种 kind 不参与快捷键, 该条直接跳过。 */
static bool behavior_id(char *id, size_t capacity, const char *model_id,
    yyjson_val *bindings, size_t index, yyjson_val *item) {
    const char *kind = yyjson_get_str(yyjson_obj_get(item, "kind"));
    const char *group = yyjson_get_str(yyjson_obj_get(item, "group"));
    if (!kind) return false;
    if (strcmp(kind, "sound") == 0 || strcmp(kind, "sound-clear") == 0)
        snprintf(id, capacity, "%s:sound:%d", model_id,
            sequential_index(bindings, index, kind));
    else if (strcmp(kind, "effect") == 0 || strcmp(kind, "effect-clear") == 0)
        snprintf(id, capacity, "%s:effect:%d", model_id,
            sequential_index(bindings, index, kind));
    else if (strcmp(kind, "expression") == 0)
        snprintf(id, capacity, "%s:expression:%d", model_id,
            (int)yyjson_get_int(yyjson_obj_get(item, "index")));
    else if (strcmp(kind, "motion") == 0)
        snprintf(id, capacity, "%s:motion:%s:%d", model_id, group ? group : "",
            (int)yyjson_get_int(yyjson_obj_get(item, "index")));
    else return false;
    return true;
}

/* 幂等: catalog 每次刷新都会调一次。已存在的绑定只补空的 label, 绝不覆盖用户改过的
   快捷键; 新增受 BONGO_CAT_BEHAVIOR_BINDING_CAP 限制, 满了就静默停止。
   只在内存里改 app->settings, 落盘由设置保存流程负责。 */
void bongo_cat_import_apply_metadata(BongoCatApp *app, const char *model_id,
    const char *directory) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_model_adapter_metadata_path(directory, path,
        sizeof(path))) return;
    yyjson_doc *document = bongo_cat_json_read_file(path, 0, NULL);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *bindings = yyjson_obj_get(root, "bindings");
    if (!metadata_version(root) || !yyjson_is_arr(bindings)) {
        yyjson_doc_free(document);
        return;
    }
    size_t index, count; yyjson_val *item;
    yyjson_arr_foreach(bindings, index, count, item) {
        char id[BONGO_CAT_PATH_CAP];
        const char *shortcut = yyjson_get_str(yyjson_obj_get(item, "shortcut"));
        const char *label = yyjson_get_str(yyjson_obj_get(item, "label"));
        if (!shortcut || !behavior_id(id, sizeof(id), model_id, bindings, index, item))
            continue;
        BongoCatBehaviorShortcut *existing = shortcut_for(app, id);
        if (existing) {
            if (label && !existing->label[0])
                snprintf(existing->label, sizeof(existing->label), "%s", label);
            continue;
        }
        if (app->settings.behavior_shortcut_count >= BONGO_CAT_BEHAVIOR_BINDING_CAP) break;
        BongoCatBehaviorShortcut *value =
            &app->settings.behavior_shortcuts[app->settings.behavior_shortcut_count++];
        snprintf(value->id, sizeof(value->id), "%s", id);
        snprintf(value->shortcut, sizeof(value->shortcut), "%s", shortcut);
        if (label) snprintf(value->label, sizeof(value->label), "%s", label);
    }
    yyjson_doc_free(document);
}

/* 返回值 = 是否成功读到导入适配的渲染参数: true 时 options 被填满; false 时调用方沿用
   内置 Live2D 默认投影(函数已清零 options, 可以安全忽略其内容)。
   profile 只认 "mver-0.1.6"(其余版本的参数含义可能不同), scale 必须落在 (0, 100],
   参考分辨率必须为正。 */
bool bongo_cat_import_render_options(const char *directory,
    BongoCatLive2DRenderOptions *options) {
    if (!options) return false;
    *options = (BongoCatLive2DRenderOptions){0};
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_model_adapter_metadata_path(directory, path,
        sizeof(path))) return false;
    yyjson_doc *document = bongo_cat_json_read_file(path, 0, NULL);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *render = yyjson_obj_get(root, "render");
    const char *profile = yyjson_get_str(yyjson_obj_get(render, "profile"));
    yyjson_val *scale = yyjson_obj_get(render, "projectionScale");
    int width = (int)yyjson_get_sint(yyjson_obj_get(render, "referenceWidth"));
    int height = (int)yyjson_get_sint(yyjson_obj_get(render, "referenceHeight"));
    bool valid = metadata_version(root) &&
        yyjson_is_obj(render) && profile && strcmp(profile, "mver-0.1.6") == 0 &&
        yyjson_is_num(scale) && yyjson_get_num(scale) > 0.0 &&
        yyjson_get_num(scale) <= 100.0 && width > 0 && height > 0;
    if (valid) {
        options->mver_projection = true;
        options->auto_frame = yyjson_get_bool(yyjson_obj_get(render, "autoFrame"));
        options->projection_scale = (float)yyjson_get_num(scale);
        options->offset_x = (float)yyjson_get_num(yyjson_obj_get(render, "offsetX"));
        options->offset_y = (float)yyjson_get_num(yyjson_obj_get(render, "offsetY"));
        options->reference_width = width;
        options->reference_height = height;
        yyjson_val *mirror = yyjson_obj_get(render, "mirror");
        options->source_mirror = yyjson_is_bool(mirror) && yyjson_get_bool(mirror);
        yyjson_val *left_handed = yyjson_obj_get(render, "pointerLeftHanded");
        options->pointer_left_handed = yyjson_is_bool(left_handed) &&
            yyjson_get_bool(left_handed);
        yyjson_val *force = yyjson_obj_get(render, "mouseForceMove");
        options->mouse_force_move = yyjson_is_bool(force) && yyjson_get_bool(force);
        yyjson_val *mouse_speed = yyjson_obj_get(render, "mouseSpeed");
        options->mouse_speed = yyjson_is_num(mouse_speed)
            ? (float)yyjson_get_num(mouse_speed) : 1.0f;
        yyjson_val *custom = yyjson_obj_get(render, "customPointerBounds");
        options->custom_pointer_bounds = yyjson_is_bool(custom) &&
            yyjson_get_bool(custom);
        options->pointer_left = (int)yyjson_get_sint(
            yyjson_obj_get(render, "pointerLeft"));
        options->pointer_top = (int)yyjson_get_sint(
            yyjson_obj_get(render, "pointerTop"));
        options->pointer_right = (int)yyjson_get_sint(
            yyjson_obj_get(render, "pointerRight"));
        options->pointer_bottom = (int)yyjson_get_sint(
            yyjson_obj_get(render, "pointerBottom"));
        /* 自定义指针范围必须满足 right > left 且 bottom > top, 否则退回内置范围:
           坏数据不应该把指针锁死在屏幕外。 */
        if (options->custom_pointer_bounds &&
            (options->pointer_right <= options->pointer_left ||
             options->pointer_bottom <= options->pointer_top))
            options->custom_pointer_bounds = false;
    }
    yyjson_doc_free(document);
    return valid;
}
