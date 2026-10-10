/*
 * 老 Tauri 包的标定恢复。源包里没有 bongocat.mver.json、或它的 decoration 缺字段时,
 * 只能从图片反推投影: 背景/封面图尺寸当参考分辨率, 输入图尺寸当画布纵横比, 转换器
 * 产物固定按 2.0 的缩放处理。全部是启发式, 只在源包没给出对应字段时使用 (优先级由
 * model_import_tauri_metadata.c 决定), 因此这里绝不能覆盖已经拿到的授权值。
 * 这些区间常量与 metadata.c 的合法区间 (64..8192) 必须同步, 否则两边判据会打架。
 */
#include "model_import_tauri_internal.h"
#include "bongo_cat/image.h"

/* 与 metadata.c 的 window_size 区间一致: 太小或太大都不是真实的模型画布, 而是占位图
 * 或损坏的图片, 遇到就放弃用图片反推。 */
static bool valid_canvas_size(int width, int height) {
    return width >= 64 && width <= 8192 && height >= 64 && height <= 8192;
}

/* 判定“旧版转换器产物”: 必须有 left-keys 目录 (转换器一定会导出它), 画布还要足够大
 * (>= 256); standard 另加“正方形且 >= 512” —— 转换器给 standard 导出的就是方形画布,
 * 非方形的 standard 包是别的工具做的, 套用 2.0 这个经验值只会把投影弄坏。 */
static bool legacy_converter_double_scale(
    const BongoCatImportCandidate *candidate, int width, int height) {
    if (!candidate || !valid_canvas_size(width, height) ||
        width < 256 || height < 256) return false;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_tauri_find_resource_directory(candidate, "left-keys",
            path, sizeof(path))) return false;
    if (candidate->mode != BONGO_CAT_MODE_STANDARD) return true;
    return width == height && width >= 512;
}

/* 遍历输入图记录画布尺寸: 尺寸不一致时 mismatch 置位且不再复位 —— 一旦不一致, 这张
 * 画布就不能当参考分辨率, 只能放弃反推。 */
typedef struct InputCanvas {
    int width, height;
    bool mismatch;
} InputCanvas;

static BongoCatPathVisit inspect_input_canvas(void *userdata,
    const char *directory, const char *name) {
    InputCanvas *canvas = userdata;
    char path[BONGO_CAT_PATH_CAP];
    int width = 0, height = 0;
    if (bongo_cat_tauri_key_code(name) < 0 ||
        !bongo_cat_path_join(path, sizeof(path), directory, name) ||
        !bongo_cat_image_info(path, &width, &height) ||
        !valid_canvas_size(width, height)) return BONGO_CAT_PATH_CONTINUE;
    if (canvas->width && (canvas->width != width || canvas->height != height))
        canvas->mismatch = true;
    canvas->width = width;
    canvas->height = height;
    return BONGO_CAT_PATH_CONTINUE;
}

/* 用输入图的实际画布反推投影: 先保持竖直方向的比例不变 (window_height 那一项不动),
 * 再按画布纵横比修正缩放, 这样手型图和模型的相对位置不会因为换参考分辨率而跑掉;
 * 越界的缩放 (<= 0.01 或 > 100) 宁可放弃。 */
static void legacy_input_canvas(const BongoCatImportCandidate *candidate,
    TauriMverCalibration *calibration) {
    InputCanvas canvas = {0};
    const char *groups[] = {"left-keys", "right-keys"};
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        if (bongo_cat_tauri_find_resource_directory(candidate, groups[i],
                path, sizeof(path)) &&
            !bongo_cat_path_enumerate(path, inspect_input_canvas, &canvas))
            return;
    }
    if (!canvas.width || canvas.mismatch ||
        (canvas.width == calibration->window_width &&
         canvas.height == calibration->window_height)) return;
    /* Legacy exporters can leave a square placeholder background while keeping
       the original input canvas. Preserve vertical projection when recovering
       its aspect ratio; a cropped cover is not calibration metadata. */
    double vertical_scale = calibration->l2d_correct *
        calibration->window_width / calibration->window_height;
    double scale = vertical_scale * canvas.height / canvas.width;
    if (scale <= 0.01 || scale > 100.0) return;
    calibration->window_width = canvas.width;
    calibration->window_height = canvas.height;
    calibration->l2d_correct = scale;
}

/* 入口: 先按默认值起底, 再尽力从图片推断。auto_frame 置 true 是因为老包已经丢掉了
 * 作者的构图信息, 只能让运行时自动留边距, 否则模型容易被窗口裁掉。 */

void bongo_cat_tauri_legacy_calibration(const BongoCatImportCandidate *candidate,
    TauriMverCalibration *calibration) {
    bongo_cat_tauri_calibration_defaults(calibration);
    /* Legacy packages lost the authored projection. Keep aligned layers, but
       allow visible model geometry to extend beyond the inferred canvas. */
    calibration->auto_frame = true;
    char path[BONGO_CAT_PATH_CAP];
    const char *names[] = {"background.png", "cover.png"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!bongo_cat_tauri_find_resource_file(candidate, names[i], path,
                sizeof(path))) continue;
        int width = 0, height = 0;
        if (bongo_cat_image_info(path, &width, &height) &&
            valid_canvas_size(width, height)) {
            calibration->window_width = width;
            calibration->window_height = height;
            break;
        }
    }
    /* Converter packages before schemaVersion 1 omitted their Mver projection
       calibration. Keyboard/gamepad packages and the converter's square
       standard profile use approximately 2.0. Non-square standard packages
       retain Mver's 1.1 default; cover.png is only a cropped preview and must
       not be used to infer runtime projection. */
    if (legacy_converter_double_scale(candidate, calibration->window_width,
            calibration->window_height)) {
        calibration->l2d_correct = 2.0;
        if (candidate->mode == BONGO_CAT_MODE_STANDARD &&
            calibration->window_width == calibration->window_height)
            calibration->l2d_offset_y = -0.005;
        legacy_input_canvas(candidate, calibration);
    }
}

