/*
 * Mver 适配元数据（.bongo-cat-adapter.json）的生成端：读作者原版的 config（JSON5）与 Live2D 清单，把渲染
 * 参数、指针层、表情与动作、音效、特效绑定翻译成运行时读得懂的中间格式，写进适配目录。
 * 时序：由 bongo_cat_import_prepare_adapter 的 metadata 步骤调用，排在素材转换之后（要引用前面复制或合成
 * 出来的文件），失败会让整个候选导入失败。
 * 契约：schemaVersion 为 1 且 kind 为 "bongo-cat-runtime-adapter"；消费端按字段名对齐，改名等于改协议。
 * 易错点：bindings 的追加顺序决定消费端按"同族出现顺序"算出的绑定 id，已发布的 id 不能再变。
 */
#include "model_import.h"
#include "model_import_mver_internal.h"
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/image.h"
#include "bongo_cat/json.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

/* 写进 sourceFormat 的稳定标识；注意它与日志用的短名 bongo_cat_import_format_name 不是同一套字符串。 */
static const char *format_name(BongoCatImportFormat format) {
    if (format == BONGO_CAT_IMPORT_MVER) return "bongo-cat-mver";
    if (format == BONGO_CAT_IMPORT_MVER_PATCH) return "bongo-cat-mver-patch";
    return "tauri-live2d";
}

/* 可选数值字段的统一回落：缺失或类型不对时取 fallback，绝不让非法值进入渲染参数。 */
static double number_or(yyjson_val *value, double fallback) {
    return yyjson_is_num(value) ? yyjson_get_num(value) : fallback;
}

/* 可用 = 是文件、能解码、尺寸不是 1x1 级别的占位图；三者缺一就当这张贴图不存在。 */
static bool usable_pointer_image(const char *path) {
    int width = 0, height = 0;
    return bongo_cat_path_is_file(path) &&
        bongo_cat_image_info(path, &width, &height) &&
        (width > 1 || height > 1);
}

/* 指针贴图查找：贴图补丁（overrides）优先于基准包，与素材转换、特效的查找顺序保持一致。 */
static bool pointer_asset(const BongoCatImportCandidate *candidate,
    const char *name) {
    char path[BONGO_CAT_PATH_CAP];
    if (candidate->overrides[0] && bongo_cat_path_join(path, sizeof(path),
            candidate->overrides, name) && bongo_cat_path_is_file(path))
        return usable_pointer_image(path);
    return bongo_cat_path_join(path, sizeof(path), candidate->assets, name) &&
        usable_pointer_image(path);
}

/* 生成 standardPointer 段，只由静态贴图指针层消费。device_index 沿用 Mver 的数组约定：0 是鼠标、
   1 是数位板，offsetX、offsetY 与 scalar 都按它取值。enabled 与渲染端对齐 —— 手臂与当前设备的主图
   必须存在（缺了整层加载失败），左右键与侧键可以缺。 */
static bool add_standard_pointer(yyjson_mut_doc *output, yyjson_mut_val *root,
    yyjson_val *config, const BongoCatImportCandidate *candidate) {
    if (candidate->mode != BONGO_CAT_MODE_STANDARD) return true;
    yyjson_val *decoration = yyjson_obj_get(config, "decoration");
    yyjson_val *standard = yyjson_obj_get(config, "standard");
    yyjson_val *mouse_value = yyjson_obj_get(standard, "mouse");
    bool mouse = yyjson_is_bool(mouse_value) && yyjson_get_bool(mouse_value);
    yyjson_val *live2d_value = yyjson_obj_get(standard, "l2d");
    bool live2d = !yyjson_is_bool(live2d_value) || yyjson_get_bool(live2d_value);
    size_t device_index = mouse ? 0 : 1;
    yyjson_val *offset_x = yyjson_obj_get(decoration, "offsetX");
    yyjson_val *offset_y = yyjson_obj_get(decoration, "offsetY");
    yyjson_val *scale = yyjson_obj_get(decoration, "scalar");
    /* Live2D 模式下作者把手偏移写在 standard 段，贴图指针模式下写在 decoration 段，这两个位置不能互换。 */
    yyjson_val *hand_offset = yyjson_obj_get(
        live2d ? standard : decoration, "hand_offset");
    yyjson_val *line = yyjson_obj_get(decoration, "armLineColor");
    yyjson_val *left_handed = yyjson_obj_get(decoration, "leftHanded");
    /* 这几条相对路径与 bongo_cat_import_mver_assets 复制到的 resources/mver-pointer/ 一一对应；
       缺失的可选贴图由渲染端当作没有那一层，因此这里仍然无条件写出引用。 */
    const char *device = mouse ? "resources/mver-pointer/mouse.png" :
        "resources/mver-pointer/tablet.png";
    const char *left = mouse ? "resources/mver-pointer/mouse_left.png" :
        "resources/mver-pointer/tablet_left.png";
    const char *right = mouse ? "resources/mver-pointer/mouse_right.png" :
        "resources/mver-pointer/tablet_right.png";
    const char *side = mouse ? "resources/mver-pointer/mouse_side.png" : "";
    /* Mver 0.1.6's l2d switch replaces the sprite renderer. The older
       standalone mode 98 instead draws a sprite pointer over Live2D. */
    /* 只有显式 l2d = true 且不是老的独立模式 98 时才关掉贴图指针，缺字段的老配置照旧出指针层。 */
    bool sprite_pointer = !yyjson_is_true(live2d_value) ||
        yyjson_get_int(yyjson_obj_get(config, "mode")) == 98;
    bool enabled = sprite_pointer && pointer_asset(candidate, "arm.png") &&
        pointer_asset(candidate, mouse ? "mouse.png" : "tablet.png");
    yyjson_mut_val *pointer = yyjson_mut_obj_add_obj(output, root, "standardPointer");
    return pointer &&
        yyjson_mut_obj_add_bool(output, pointer, "enabled", enabled) &&
        yyjson_mut_obj_add_bool(output, pointer, "mouse", mouse) &&
        yyjson_mut_obj_add_bool(output, pointer, "leftHanded",
            yyjson_is_bool(left_handed) && yyjson_get_bool(left_handed)) &&
        yyjson_mut_obj_add_str(output, pointer, "arm",
            "resources/mver-pointer/arm.png") &&
        yyjson_mut_obj_add_strcpy(output, pointer, "device", device) &&
        yyjson_mut_obj_add_strcpy(output, pointer, "left", left) &&
        yyjson_mut_obj_add_strcpy(output, pointer, "right", right) &&
        yyjson_mut_obj_add_strcpy(output, pointer, "side", side) &&
        yyjson_mut_obj_add_real(output, pointer, "offsetX",
            number_or(yyjson_arr_get(offset_x, device_index), 0.0)) &&
        yyjson_mut_obj_add_real(output, pointer, "offsetY",
            number_or(yyjson_arr_get(offset_y, device_index), 0.0)) &&
        yyjson_mut_obj_add_real(output, pointer, "scale",
            number_or(yyjson_arr_get(scale, device_index), 1.0)) &&
        yyjson_mut_obj_add_real(output, pointer, "handOffsetX",
            number_or(yyjson_arr_get(hand_offset, 0), 0.0)) &&
        yyjson_mut_obj_add_real(output, pointer, "handOffsetY",
            number_or(yyjson_arr_get(hand_offset, 1), 0.0)) &&
        yyjson_mut_obj_add_int(output, pointer, "lineRed",
            (int)number_or(yyjson_arr_get(line, 0), 0.0)) &&
        yyjson_mut_obj_add_int(output, pointer, "lineGreen",
            (int)number_or(yyjson_arr_get(line, 1), 0.0)) &&
        yyjson_mut_obj_add_int(output, pointer, "lineBlue",
            (int)number_or(yyjson_arr_get(line, 2), 0.0));
}

/* 生成 render 段。profile 固定写 "mver-0.1.6"，消费端只认这一个取值（别的版本参数含义不同，会被整体
   忽略）；scale 必须落在 (0, 100]、参考分辨率必须为正，所以越界值直接换成 Mver 的默认值（1.1 与
   612x352），否则会因一条坏参数退回内置投影。布尔字段原样透传（缺失即 false）；指针范围四元数是屏幕
   坐标而不是比例，消费端要求 right > left 且 bottom > top。 */
static bool add_render_profile(yyjson_mut_doc *output, yyjson_mut_val *root,
    yyjson_val *config, const BongoCatImportCandidate *candidate) {
    yyjson_val *decoration = yyjson_obj_get(config, "decoration");
    yyjson_val *workarea = yyjson_obj_get(config, "workarea");
    yyjson_val *window = yyjson_obj_get(decoration, "window_size");
    yyjson_val *offset = yyjson_obj_get(decoration, "l2d_offset");
    yyjson_val *top_left = yyjson_obj_get(workarea, "top_left");
    yyjson_val *right_bottom = yyjson_obj_get(workarea, "right_bottom");
    double scale = number_or(yyjson_obj_get(decoration, "l2d_correct"), 1.1);
    int width = (int)number_or(yyjson_arr_get(window, 0), 612.0);
    int height = (int)number_or(yyjson_arr_get(window, 1), 352.0);
    double offset_x = number_or(yyjson_arr_get(offset, 0), 0.0);
    double offset_y = number_or(yyjson_arr_get(offset, 1), 0.0);
    yyjson_val *mirror_value = yyjson_obj_get(decoration, "l2d_horizontal_flip");
    bool mirror = yyjson_is_bool(mirror_value) && yyjson_get_bool(mirror_value);
    yyjson_val *left_handed_value = yyjson_obj_get(decoration, "leftHanded");
    bool left_handed = yyjson_is_bool(left_handed_value) &&
        yyjson_get_bool(left_handed_value);
    yyjson_val *force_value = yyjson_obj_get(decoration, "mouse_force_move");
    bool force = yyjson_is_bool(force_value) && yyjson_get_bool(force_value);
    double mouse_speed = number_or(yyjson_obj_get(decoration, "mouse_speed"), 1.0);
    yyjson_val *custom_value = yyjson_obj_get(workarea, "workarea");
    bool custom = yyjson_is_bool(custom_value) && yyjson_get_bool(custom_value);
    int left = (int)number_or(yyjson_arr_get(top_left, 0), 0.0);
    int top = (int)number_or(yyjson_arr_get(top_left, 1), 0.0);
    int right = (int)number_or(yyjson_arr_get(right_bottom, 0), 0.0);
    int bottom = (int)number_or(yyjson_arr_get(right_bottom, 1), 0.0);
    if (scale <= 0.0 || scale > 100.0) scale = 1.1;
    if (width <= 0 || height <= 0) { width = 612; height = 352; }
    yyjson_mut_val *render = yyjson_mut_obj_add_obj(output, root, "render");
    return render &&
        yyjson_mut_obj_add_str(output, render, "profile", "mver-0.1.6") &&
        yyjson_mut_obj_add_bool(output, render, "autoFrame",
            yyjson_get_bool(yyjson_obj_get(decoration, "l2d_auto_frame"))) &&
        yyjson_mut_obj_add_real(output, render, "projectionScale", scale) &&
        yyjson_mut_obj_add_real(output, render, "offsetX", offset_x) &&
        yyjson_mut_obj_add_real(output, render, "offsetY", offset_y) &&
        yyjson_mut_obj_add_int(output, render, "referenceWidth", width) &&
        yyjson_mut_obj_add_int(output, render, "referenceHeight", height) &&
        yyjson_mut_obj_add_bool(output, render, "mirror", mirror) &&
        yyjson_mut_obj_add_bool(output, render, "pointerLeftHanded", left_handed) &&
        yyjson_mut_obj_add_bool(output, render, "mouseForceMove", force) &&
        yyjson_mut_obj_add_real(output, render, "mouseSpeed", mouse_speed) &&
        yyjson_mut_obj_add_bool(output, render, "customPointerBounds", custom) &&
        yyjson_mut_obj_add_int(output, render, "pointerLeft", left) &&
        yyjson_mut_obj_add_int(output, render, "pointerTop", top) &&
        yyjson_mut_obj_add_int(output, render, "pointerRight", right) &&
        yyjson_mut_obj_add_int(output, render, "pointerBottom", bottom) &&
        add_standard_pointer(output, root, config, candidate);
}

/* Tauri 候选不写 Mver 渲染参数，只把 profile 标成 "native"，消费端据此沿用内置投影。 */
static bool add_native_render(yyjson_mut_doc *output, yyjson_mut_val *root) {
    yyjson_mut_val *render = yyjson_mut_obj_add_obj(output, root, "render");
    return render && yyjson_mut_obj_add_str(output, render, "profile", "native");
}

/* 生成 <target>/.bongo-cat-adapter.json。前提：target 目录已存在（prepare_adapter 负责创建）；返回值是
   否写出成功，失败时若 error 还空着就补一条 FORMAT（消息里带 config 路径），因为这步失败是用户可见的。
   副作用：音效与特效步骤会把素材复制进 target/resources，所以本函数不是纯内存操作，也不保证失败回滚 ——
   残留物由调用方的临时目录语义清理。 */
bool bongo_cat_import_adapter_metadata(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error) {
    bool mver = candidate->format != BONGO_CAT_IMPORT_TAURI;
    yyjson_doc *source = mver ? bongo_cat_json_read_file(candidate->config,
        YYJSON_READ_JSON5 | YYJSON_READ_ALLOW_INVALID_UNICODE, NULL) : NULL;
    yyjson_val *config = source ? yyjson_doc_get_root(source) : NULL;
    yyjson_val *mode = yyjson_obj_get(config, bongo_cat_mode_name(candidate->mode));
    BongoCatMverLabels labels = {0};
    if (mver) bongo_cat_mver_labels_load(candidate->config,
        bongo_cat_mode_name(candidate->mode), &labels);
    yyjson_mut_doc *output = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = output ? yyjson_mut_obj(output) : NULL;
    yyjson_mut_val *items = root ? yyjson_mut_obj_add_arr(output, root, "bindings") : NULL;
    if (output) yyjson_mut_doc_set_root(output, root);
    bool ok = items &&
        yyjson_mut_obj_add_int(output, root, "schemaVersion",
            BONGO_CAT_MODEL_ADAPTER_SCHEMA) &&
        yyjson_mut_obj_add_str(output, root, "kind", "bongo-cat-runtime-adapter") &&
        yyjson_mut_obj_add_strcpy(output, root, "sourceFormat",
            format_name(candidate->format));
    /* 追加顺序就是 bindings 的顺序：表情与动作、音效、特效。消费端按同族出现顺序编号，跨家族的先后不
       影响 id，但同一家族内不能重排。 */
    if (ok && mver) ok = yyjson_is_obj(mode) &&
        add_render_profile(output, root, config, candidate) &&
        bongo_cat_mver_add_behaviors(output, items, mode, candidate, &labels, error) &&
        bongo_cat_mver_add_audio(output, items, config, yyjson_obj_get(mode, "sounds"),
            candidate, &labels, target) &&
        bongo_cat_mver_effects(output, items, config, mode, candidate, target);
    else if (ok) ok = add_native_render(output, root);
    char path[BONGO_CAT_PATH_CAP];
    if (ok) ok = bongo_cat_path_join(path, sizeof(path), target,
        BONGO_CAT_MODEL_ADAPTER_FILE) &&
        bongo_cat_json_write_file(path, output, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(output);
    yyjson_doc_free(source);
    /* 写盘中途失败时 json_write_file 已经删掉了半成品文件，这里只补错误消息，且不覆盖已有的。 */
    if (!ok && error && !error->message[0])
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Cannot create runtime adapter metadata: %s", candidate->config);
    return ok;
}
