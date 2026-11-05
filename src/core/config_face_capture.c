/*
 * 面捕相关设置的默认值与收口。
 *
 * 单独一个文件是为了守住 config_defaults.c 的行数策略 (cmake/CheckLines.cmake)：
 * 那个文件已经贴着 500 行, 而面捕又带进来 5 个字段。取值规则本身没有特殊之处:
 *   - face_capture 是 int (可选读 read_int), 老配置缺键时保持 0 = 关闭;
 *   - gain 钳在 0.2..3.0, 手改配置填 0/负数/超大值都在这里被纠正;
 *   - camera 是 SDL_CameraID, 负数没有意义, 归 0 = "系统默认设备"。
 */
#include "bongo_cat/config.h"

#include <math.h>

static float clamp_gain(float value) {
    if (!(value == value)) return 1.0f;   /* NaN: 手改配置写坏了 */
    if (value < 0.2f) return 0.2f;
    if (value > 3.0f) return 3.0f;
    return value;
}

void bongo_cat_settings_face_capture_defaults(BongoCatModelPreferences *model) {
    if (!model) return;
    model->face_capture = 0;
    model->face_capture_gain = 1.0f;
    model->face_capture_flip_h = 0;
    model->face_capture_flip_v = 0;
    model->face_capture_camera = 0;
    model->face_capture_gpu = 0;
}

void bongo_cat_settings_face_capture_validate(BongoCatModelPreferences *model) {
    if (!model) return;
    model->face_capture = model->face_capture != 0;
    model->face_capture_gain = clamp_gain(model->face_capture_gain);
    model->face_capture_flip_h = model->face_capture_flip_h != 0;
    model->face_capture_flip_v = model->face_capture_flip_v != 0;
    if (model->face_capture_camera < 0) model->face_capture_camera = 0;
    model->face_capture_gpu = model->face_capture_gpu != 0;
}
