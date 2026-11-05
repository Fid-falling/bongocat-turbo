/*
 * 面捕快照 -> Cubism 参数。
 *
 * 分工（用户定下的规则）：面捕开启且这一帧有人脸时，**头部/眼睛/嘴/眉毛/身体**都归面捕，
 * 鼠标不再驱动头部（见 mouse_mapping.c 里对 dragging 的跳过）；手、键盘、拖动窗口仍然
 * 走原来的键鼠逻辑。两边各管一段，不会互相打架。
 *
 * 所有参数写入前都先问 bongo_cat_live2d_parameter()：内置模型没有身体角度、有的模型没有
 * 眉毛 —— "没有这个参数" 是正常状态，不是错误。
 */
#include "runtime.h"
#include "face_capture.h"

#include <math.h>
#include <string.h>

static bool has_parameter(BongoCatLive2D *live2d, const char *id) {
    BongoCatParameterRange range = {0};
    return bongo_cat_live2d_parameter(live2d, id, &range);
}

/* 写入并按模型自己声明的范围夹取: 不同模型的 ParamAngleX 可能是 ±30 也可能是 ±10。 */
static void write_parameter(BongoCatLive2D *live2d, const char *id, float value) {
    BongoCatParameterRange range = {0};
    if (!bongo_cat_live2d_parameter(live2d, id, &range)) return;
    if (range.maximum > range.minimum)
        value = fminf(fmaxf(value, range.minimum), range.maximum);
    bongo_cat_live2d_set_parameter(live2d, id, value);
}

bool bongo_cat_face_capture_apply(BongoCatFaceCapture *capture,
    BongoCatLive2D *live2d, const BongoCatModelPreferences *prefs) {
    if (!capture || !live2d || !prefs) return false;
    BongoCatFaceSnapshot snapshot;
    bongo_cat_face_capture_snapshot(capture, &snapshot);
    if (!snapshot.has_face) return false;
    const BongoCatFaceFrame *frame = &snapshot.frame;
    /* 置信度太低（半张脸/遮挡）就当作没有: 宁可不动，也不要抽搐 */
    if (frame->confidence < 0.15f) return false;

    float gain = prefs->face_capture_gain;
    if (gain < 0.2f || gain > 3.0f) gain = 1.0f;
    const float flip_h = prefs->face_capture_flip_h ? -1.0f : 1.0f;
    const float flip_v = prefs->face_capture_flip_v ? -1.0f : 1.0f;

    /* 头: 组件的 yaw/pitch 已经缩放到 ±30 度, 这里直接当参数用 */
    const float yaw = frame->yaw_deg * gain * flip_h;
    const float pitch = frame->pitch_deg * gain * flip_v;
    const float roll = -frame->roll_deg * gain * flip_h;
    write_parameter(live2d, "ParamAngleX", yaw);
    write_parameter(live2d, "ParamAngleY", pitch);
    write_parameter(live2d, "ParamAngleZ", roll);
    /* 身体跟着头走, 但幅度减半: 用户要的是"整个人跟着转", 不是脖子拧断 */
    write_parameter(live2d, "ParamBodyAngleX", yaw * 0.5f);
    write_parameter(live2d, "ParamBodyAngleY", pitch * 0.35f);
    write_parameter(live2d, "ParamBodyAngleZ", roll * 0.3f);

    /* 脸: 眼开/嘴/眉/腮 —— 0..1 直接对应模型参数, 不再缩放 */
    const float eye_l = fminf(fmaxf(frame->eye_left, 0.0f), 1.0f);
    const float eye_r = fminf(fmaxf(frame->eye_right, 0.0f), 1.0f);
    write_parameter(live2d, "ParamEyeLOpen", eye_l);
    write_parameter(live2d, "ParamEyeROpen", eye_r);
    write_parameter(live2d, "ParamMouthOpenY", fminf(fmaxf(frame->mouth_open, 0.0f), 1.0f));
    write_parameter(live2d, "ParamMouthForm", fminf(fmaxf(frame->mouth_form, -1.0f), 1.0f));
    write_parameter(live2d, "ParamBrowLY", fminf(fmaxf(frame->brow_left, 0.0f), 1.0f));
    write_parameter(live2d, "ParamBrowRY", fminf(fmaxf(frame->brow_right, 0.0f), 1.0f));
    write_parameter(live2d, "ParamCheek", fminf(fmaxf(frame->cheek, 0.0f), 1.0f));
    /* 视线: 当前组件用头姿近似, 幅度给一半, 避免和头部参数重复放大小动作 */
    write_parameter(live2d, "ParamEyeBallX", frame->gaze_x * 0.5f * flip_h);
    write_parameter(live2d, "ParamEyeBallY", frame->gaze_y * 0.5f * flip_v);
    return true;
}
