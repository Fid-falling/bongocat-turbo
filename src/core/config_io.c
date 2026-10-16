#include "config_internal.h"
#include "bongo_cat/path.h"

#include <stdlib.h>
#include <string.h>

/*
 * settings.json 的读取侧(写入在 config_settings_save.c, 那里才是权威)。
 *
 * 不变量: 所有字段先读进栈上的副本 loaded, 只有全部通过校验才会执行
 * *settings = loaded; 任何一步失败都保留调用方原来的设置, 不会留下一个
 * "读了一半"的残缺配置被后续逻辑当成真值使用。
 */
#define SETTINGS_FORMAT "bongocat/settings"
#define type_error(error, field, expected) bongo_cat_config_type_error( \
    "Settings", error, field, expected)
#define read_value(object, key, target, error) bongo_cat_config_read_value( \
    "Settings", object, key, target, error)
#define read_object(object, key, target, error) bongo_cat_config_read_object( \
    "Settings", object, key, target, error)
#define read_array(object, key, target, error) bongo_cat_config_read_array( \
    "Settings", object, key, target, error)
#define read_bool(object, key, target, error) bongo_cat_config_read_bool( \
    "Settings", object, key, target, error)
#define read_int(object, key, target, error) bongo_cat_config_read_int( \
    "Settings", object, key, target, false, error)
/*
 * read_int 的 required 固定为 false: 设置项一律可选读, 老配置文件里缺键时
 * 沿用 BongoCatSettings 已有的默认值, 不能因此判定配置损坏。
 */
#define read_float(object, key, target, error) bongo_cat_config_read_float( \
    "Settings", object, key, target, error)
#define read_string(object, key, target, length, error) \
    bongo_cat_config_read_string( \
        "Settings", object, key, target, length, error)
#define read_text(object, key, target, capacity, error) \
    bongo_cat_config_read_text( \
        "Settings", object, key, target, capacity, error)
#define copy_text(target, capacity, value, length, field, error) \
    bongo_cat_config_copy_text( \
        "Settings", target, capacity, value, length, field, error)

static bool parse_theme(const char *value, BongoCatTheme *target) {
    if (!strcmp(value, "auto")) *target = BONGO_CAT_THEME_AUTO;
    else if (!strcmp(value, "light")) *target = BONGO_CAT_THEME_LIGHT;
    else if (!strcmp(value, "dark")) *target = BONGO_CAT_THEME_DARK;
    else return false;
    return true;
}

static bool parse_background_color(const char *value,
    BongoCatObsBackgroundColor *target) {
    for (int i = 0; i < BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT; ++i)
        if (!strcmp(value, bongo_cat_obs_background_color_name(
                (BongoCatObsBackgroundColor)i))) {
            *target = (BongoCatObsBackgroundColor)i;
            return true;
        }
    return false;
}

/*
 * 下面三个 read_* 是同一套路: 先用 && 把布尔/数值字段读完, 再单独处理枚举
 * 字符串(renderer / theme / language)。read_string 对缺键返回 true 并把
 * text 置 NULL, 所以 text && 表示"键确实存在", 这时才需要检查取值是否在
 * 白名单里; 键不存在就保留默认值 —— 老配置里没有这些字段是正常情况。
 */
static bool read_model(yyjson_val *object, BongoCatModelPreferences *value,
    BongoCatError *error) {
    if (!read_bool(object, "multiplePets", &value->multiple_pets, error) ||
        !read_bool(object, "modelMirrored", &value->mirror, error) ||
        !read_bool(object, "modelFlippedVertically", &value->vertical_flip,
            error) ||
        !read_bool(object, "pointerMirrored", &value->mouse_mirror, error) ||
        !read_bool(object, "pointerFlippedVertically",
            &value->mouse_vertical_flip, error) ||
        !read_bool(object, "centerPointerTracking", &value->mouse_centered,
            error) ||
        !read_bool(object, "ignorePointerInput", &value->ignore_mouse, error) ||
        !read_int(object, "maximumFps", &value->max_fps, error) ||
        /*
         * 垂直同步: 可选读。缺键时保留 -1(sentinel), 由 bongo_cat_settings_validate()
         * 按"老版本里同步跟随最大帧率"的规则收口, 老配置因此不会因为升级而突变。
         * 用 read_int 而不是 read_bool 就是为了保住这个第三态。
         */
        !read_int(object, "vsync", &value->vsync, error) ||
        /* 可选读: 老配置缺这两个键时保留默认值 (多键显示打开, 每侧 5 个), 于是老用户
           升级后立刻拿到多键显示, 而写盘时又必然带上具体取值 (见 config_settings_save.c)。 */
        !read_int(object, "multiKeyDisplay", &value->multi_key_display, error) ||
        !read_int(object, "multiKeyLimit", &value->multi_key_limit, error) ||
        !read_int(object, "bubbleMerge", &value->bubble_merge, error))
        return false;
    const char *text;
    size_t length;
    if (!read_string(object, "renderer", &text, &length, error)) return false;
    (void)length;
    if (text && !bongo_cat_renderer_parse(text, &value->renderer))
        return type_error(error, "renderer", "auto, opengl, or vulkan");
    return true;
}

static bool read_window(yyjson_val *object, BongoCatWindowPreferences *value,
    BongoCatError *error) {
    if (!read_bool(object, "clickThrough", &value->pass_through, error) ||
        !read_bool(object, "alwaysOnTop", &value->always_on_top, error) ||
        !read_bool(object, "hideOnPointerOver", &value->hide_on_hover, error) ||
        !read_bool(object, "keepOnScreen", &value->keep_in_screen, error) ||
        !read_bool(object, "edgeSnap", &value->edge_snap, error) ||
        !read_int(object, "edgeSnapThreshold", &value->edge_snap_threshold,
            error) ||
        !read_bool(object, "slideAnimation", &value->slide_animation,
            error) ||
        !read_int(object, "slideSlowdownPercent",
            &value->slide_slowdown_percent, error) ||
        /* 可选读: 老配置里没有 slideNoDecay 时保留默认值 false, 即仍是"会衰减"的旧行为。 */
        !read_bool(object, "slideNoDecay", &value->slide_no_decay, error) ||
        !read_bool(object, "captureOnly", &value->capture_only, error) ||
        !read_int(object, "supersample", &value->supersample, error) ||
        !read_int(object, "fontRasterCap", &value->font_raster_cap, error) ||
        !read_int(object, "disableUiEffects", &value->disable_effects, error) ||
        !read_int(object, "glTransparency", &value->gl_transparency, error) ||
        !read_bool(object, "captureBackground", &value->obs_background,
            error) ||
        !read_bool(object, "randomExpression", &value->random_expression,
            error) ||
        !read_bool(object, "randomMotion", &value->random_motion, error) ||
        !read_bool(object, "randomSound", &value->random_sound, error) ||
        !read_bool(object, "sequentialModel", &value->sequential_model, error) ||
        !read_bool(object, "roundedCorners", &value->rounded_corners, error) ||
        !read_float(object, "cornerRadiusPercent", &value->corner_radius_percent,
            error) ||
        !read_float(object, "hideDelaySeconds", &value->hide_delay_seconds,
            error) ||
        !read_float(object, "hideFadeSeconds", &value->hide_fade_seconds,
            error) ||
        !read_float(object, "randomExpressionIntervalSeconds",
            &value->random_expression_interval_seconds,
            error) ||
        !read_float(object, "randomMotionIntervalSeconds",
            &value->random_motion_interval_seconds, error) ||
        !read_float(object, "randomSoundIntervalSeconds",
            &value->random_sound_interval_seconds, error) ||
        !read_float(object, "sequentialModelIntervalSeconds",
            &value->sequential_model_interval_seconds, error)) return false;
    const char *color;
    size_t length;
    if (!read_string(object, "captureBackgroundColor", &color, &length,
            error)) return false;
    (void)length;
    if (color && !parse_background_color(color, &value->obs_background_color))
        return type_error(error, "captureBackgroundColor",
            "a supported color string");
    return true;
}

static bool read_app(yyjson_val *object, BongoCatApplicationPreferences *value,
    BongoCatError *error) {
    if (!read_bool(object, "launchAtLogin", &value->autostart, error) ||
        !read_bool(object, "runAsAdmin", &value->run_as_admin, error) ||
        !read_bool(object, "showTrayIcon", &value->tray_visible, error) ||
        /* 可选读: 老配置里没有这两个键时保留 config_defaults 的默认值(false)。 */
        !read_bool(object, "live2dCorePromptShown",
            &value->live2d_core_prompt_shown, error) ||
        !read_bool(object, "live2dCorePromptDismissed",
            &value->live2d_core_prompt_dismissed, error))
        return false;
    const char *text;
    size_t length;
    if (!read_string(object, "theme", &text, &length, error)) return false;
    (void)length;
    if (text && !parse_theme(text, &value->theme))
        return type_error(error, "theme", "auto, light, or dark");
    if (!read_string(object, "language", &text, &length, error)) return false;
    if (text && !bongo_cat_language_parse(text, &value->language))
        return type_error(error, "language", "a supported locale string");
    return true;
}

static bool read_shortcuts(yyjson_val *object,
    BongoCatShortcutPreferences *value, BongoCatError *error) {
    return read_text(object, "toggleVisibility", value->toggle_pet_visibility,
            sizeof(value->toggle_pet_visibility), error) &&
        read_text(object, "openSettings", value->visible_preferences,
            sizeof(value->visible_preferences), error) &&
        read_text(object, "toggleModelMirror", value->mirror,
            sizeof(value->mirror), error) &&
        read_text(object, "toggleClickThrough", value->pass_through,
            sizeof(value->pass_through), error) &&
        read_text(object, "toggleAlwaysOnTop", value->always_on_top,
            sizeof(value->always_on_top), error);
}

/*
 * 三个数组字段(behaviorOverrides / modelOverrides / removedModels)共用一条
 * 约定: 数组整体缺省(!array)属正常情况, 直接成功返回; 数组存在则先清零
 * count 再逐项填充, 这样重复 load 不会把上一次的条目追加进来。容量检查放在
 * 写入之前, 超限一律报错而不是截断。
 */
static bool read_behaviors(yyjson_val *array, BongoCatSettings *settings,
    BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        return type_error(error, "behaviorOverrides", "a smaller array");
    settings->behavior_shortcut_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        if (!yyjson_is_obj(item))
            return type_error(error, "behaviorOverrides[]", "an object");
        const char *id, *shortcut, *label;
        size_t id_length, shortcut_length, label_length;
        if (!read_string(item, "behaviorId", &id, &id_length, error) ||
            !read_string(item, "shortcut", &shortcut, &shortcut_length,
                error) ||
            !read_string(item, "displayName", &label, &label_length, error))
            return false;
        if (!id || !id_length)
            return type_error(error, "behaviorId", "a non-empty string");
        BongoCatBehaviorShortcut *entry = &settings->behavior_shortcuts[
            settings->behavior_shortcut_count++];
        memset(entry, 0, sizeof(*entry));
        if (!read_bool(item, "shortcutDisabled", &entry->shortcut_disabled, error)) return false;
        if (!copy_text(entry->id, sizeof(entry->id), id, id_length,
                "behaviorId", error) ||
            !copy_text(entry->shortcut, sizeof(entry->shortcut), shortcut,
                shortcut_length, "shortcut", error) ||
            !copy_text(entry->label, sizeof(entry->label), label,
                label_length, "displayName", error)) return false;
    }
    return true;
}

static bool read_model_labels(yyjson_val *array, BongoCatSettings *settings,
    BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_MODEL_CAP)
        return type_error(error, "modelOverrides", "a smaller array");
    settings->model_label_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        if (!yyjson_is_obj(item))
            return type_error(error, "modelOverrides[]", "an object");
        const char *id, *label;
        size_t id_length, label_length;
        if (!read_string(item, "modelId", &id, &id_length, error) ||
            !read_string(item, "displayName", &label, &label_length, error))
            return false;
        if (!id || !id_length || !label || !label_length)
            return type_error(error, "modelOverrides[]",
                "non-empty modelId and displayName strings");
        BongoCatModelLabel *entry =
            &settings->model_labels[settings->model_label_count++];
        memset(entry, 0, sizeof(*entry));
        if (!copy_text(entry->id, sizeof(entry->id), id, id_length,
                "modelId", error) ||
            !copy_text(entry->label, sizeof(entry->label), label,
                label_length, "displayName", error)) return false;
    }
    return true;
}

static bool read_removed_models(yyjson_val *array, BongoCatSettings *settings,
    BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_MODEL_CAP)
        return type_error(error, "removedModels", "a smaller array");
    settings->removed_model_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        const char *id = yyjson_get_str(item);
        size_t length = yyjson_get_len(item);
        if (!yyjson_is_str(item) || !id || !length || strlen(id) != length)
            return type_error(error, "removedModels[]",
                "a non-empty string without embedded nulls");
        BongoCatRemovedModel *entry =
            &settings->removed_models[settings->removed_model_count++];
        memset(entry, 0, sizeof(*entry));
        if (!copy_text(entry->id, sizeof(entry->id), id, length,
                "removedModels[]", error)) return false;
    }
    return true;
}

/* hiddenModels / randomDisabled 与 removedModels 同一条数组约定: 数组整体缺省属
   正常情况直接成功; 存在则先清零 count 再逐项填充; 超限一律报错不截断。 */
static bool read_hidden_models(yyjson_val *array, BongoCatSettings *settings,
    BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_MODEL_CAP)
        return type_error(error, "hiddenModels", "a smaller array");
    settings->hidden_model_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        const char *id = yyjson_get_str(item);
        size_t length = yyjson_get_len(item);
        if (!yyjson_is_str(item) || !id || !length || strlen(id) != length)
            return type_error(error, "hiddenModels[]",
                "a non-empty string without embedded nulls");
        BongoCatRemovedModel *entry =
            &settings->hidden_models[settings->hidden_model_count++];
        memset(entry, 0, sizeof(*entry));
        if (!copy_text(entry->id, sizeof(entry->id), id, length,
                "hiddenModels[]", error)) return false;
    }
    return true;
}

static bool read_random_disabled(yyjson_val *array, BongoCatSettings *settings,
    BongoCatError *error) {
    if (!array) return true;
    if (yyjson_arr_size(array) > BONGO_CAT_RANDOM_DISABLED_CAP)
        return type_error(error, "randomDisabled", "a smaller array");
    settings->random_disabled_count = 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, count, item) {
        const char *id = yyjson_get_str(item);
        size_t length = yyjson_get_len(item);
        if (!yyjson_is_str(item) || !id || !length || strlen(id) != length)
            return type_error(error, "randomDisabled[]",
                "a non-empty string without embedded nulls");
        char *entry = settings->random_disabled[
            settings->random_disabled_count++];
        memset(entry, 0, BONGO_CAT_BEHAVIOR_ID_CAP);
        if (!copy_text(entry, BONGO_CAT_BEHAVIOR_ID_CAP, id, length,
                "randomDisabled[]", error)) return false;
    }
    return true;
}

/*
 * extensions 是留给"未来版本或其它工具"写的未知子树: 这里把它原样序列化成
 * 文本存进 settings->extensions_json, 下次保存时再写回文件, 于是老版本读写
 * 一遍也不会把新版本加的字段抹掉(前向兼容)。
 * 缓冲区放不下时宁可报错也不截断 —— 截断出来的不是合法 JSON, 会直接污染
 * 用户的配置文件。
 */
static BongoCatResult read_extensions(yyjson_val *value,
    BongoCatSettings *settings, BongoCatError *error) {
    if (!value) return BONGO_CAT_OK;
    if (!yyjson_is_obj(value)) {
        type_error(error, "extensions", "an object");
        return BONGO_CAT_ERROR_FORMAT;
    }
    size_t length = 0;
    char *json = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, &length);
    if (!json) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot preserve settings extensions");
        return BONGO_CAT_ERROR_MEMORY;
    }
    if (length >= sizeof(settings->extensions_json)) {
        free(json);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Settings extensions exceed the %u-byte limit",
            (unsigned)(sizeof(settings->extensions_json) - 1));
        return BONGO_CAT_ERROR_FORMAT;
    }
    memset(settings->extensions_json, 0, sizeof(settings->extensions_json));
    memcpy(settings->extensions_json, json, length + 1);
    free(json);
    return BONGO_CAT_OK;
}

BongoCatResult bongo_cat_settings_load(const char *path,
    BongoCatSettings *settings, BongoCatError *error) {
    if (!path || !settings) return BONGO_CAT_ERROR_ARGUMENT;
    /* 文件不存在 = 首次运行: 成功返回, 由调用方使用默认值。 */
    if (!bongo_cat_path_is_file(path)) return BONGO_CAT_OK;
    yyjson_doc *document = NULL;
    BongoCatResult result = bongo_cat_config_read_document(path,
        SETTINGS_FORMAT, BONGO_CAT_SETTINGS_SCHEMA, &document, error);
    if (result != BONGO_CAT_OK) return result;
    BongoCatSettings loaded = *settings;
    yyjson_val *root = yyjson_doc_get_root(document);
    yyjson_val *model = NULL;
    yyjson_val *window = NULL;
    yyjson_val *application = NULL;
    yyjson_val *shortcuts = NULL;
    yyjson_val *behaviors = NULL;
    yyjson_val *models = NULL;
    yyjson_val *removed_models = NULL;
    yyjson_val *hidden_models = NULL;
    yyjson_val *random_disabled = NULL;
    yyjson_val *extensions_value = NULL;
    bool valid = read_object(root, "rendering", &model, error) &&
        read_object(root, "window", &window, error) &&
        read_object(root, "application", &application, error) &&
        read_object(root, "shortcuts", &shortcuts, error) &&
        read_array(root, "behaviorOverrides", &behaviors, error) &&
        read_array(root, "modelOverrides", &models, error) &&
        read_array(root, "removedModels", &removed_models, error) &&
        read_array(root, "hiddenModels", &hidden_models, error) &&
        read_array(root, "randomDisabled", &random_disabled, error) &&
        read_value(root, "extensions", &extensions_value, error) &&
        (!model || read_model(model, &loaded.model, error)) &&
        (!window || read_window(window, &loaded.window, error)) &&
        (!application || read_app(application, &loaded.app, error)) &&
        (!shortcuts || read_shortcuts(shortcuts, &loaded.shortcuts, error)) &&
        read_behaviors(behaviors, &loaded, error) &&
        read_model_labels(models, &loaded, error) &&
        read_removed_models(removed_models, &loaded, error) &&
        read_hidden_models(hidden_models, &loaded, error) &&
        read_random_disabled(random_disabled, &loaded, error);
    /* cache_path is optional; missing means the platform default is used. */
    valid = valid && read_text(root, "cachePath", loaded.cache_root,
        sizeof(loaded.cache_root), error);
    /* 读链失败时 result 还是上面的 OK, 这里补上总的结果码;
       具体是哪个字段错了已经由 read_* 写进 error 了。 */
    if (!valid) result = BONGO_CAT_ERROR_FORMAT;
    if (valid) result = read_extensions(extensions_value, &loaded, error);
    yyjson_doc_free(document);
    if (result != BONGO_CAT_OK) return result;
    /* 走到这里才交出结果: 前面的失败路径都直接 return, *settings 保持原样。 */
    bongo_cat_settings_validate(&loaded);
    *settings = loaded;
    return BONGO_CAT_OK;
}