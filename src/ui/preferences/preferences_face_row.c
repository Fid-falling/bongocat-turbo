/*
 * 面捕设置行 (互动 → 输入)。
 *
 * 为什么单独一个文件: preferences_pages.c 贴着 500 行策略上限 (cmake/CheckLines.cmake),
 * 而面捕这块还有"开关 / 灵敏度 / 反转 / 状态"四行, 拆出来与 bubble rows 同一理由。
 *
 * 开关打开时立刻走 bongo_cat_face_capture_sync(): 有组件就开摄像头, 没有就记一行日志并
 * 保持关闭 —— 设置页要如实显示"为什么没生效"(缺组件 / 没摄像头 / ABI 不匹配)。
 */
#include "preferences_internal.h"

#include "preferences_widgets.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"

#include "../../runtime/input/face_capture.h"

#include <SDL3/SDL.h>

#include <stdio.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return app && app->i18n ? bongo_cat_i18n_get(app->i18n, key, fallback)
        : fallback;
}

void bongo_cat_preferences_face_rows(BongoCatApp *app,
    struct nk_context *context) {
    if (!app) return;
    BongoCatModelPreferences *model = &app->settings.model;
    bool enabled = model->face_capture != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    if (bongo_cat_pref_toggle(context, "face-capture", tr(app,
            "pages.preference.cat.labels.faceCapture", "Face Capture"),
            tr(app, "pages.preference.cat.labels.faceCaptureHint",
                "用摄像头驱动表情（需要下载组件）"), &enabled)) {
        model->face_capture = enabled ? 1 : 0;
        app->dirty = true;
        /* 开关直接生效: 打开就装载组件并开摄像头, 关掉就释放设备 */
        bongo_cat_face_capture_sync(app);
    }
    if (!model->face_capture) return;

    char status[192];
    if (app->face_capture && bongo_cat_face_capture_available(app->face_capture)) {
        BongoCatFaceSnapshot snapshot;
        bongo_cat_face_capture_snapshot(app->face_capture, &snapshot);
        snprintf(status, sizeof(status), "%s",
            snapshot.status[0] ? snapshot.status : "已开启");
    } else {
        snprintf(status, sizeof(status), "%s", tr(app,
            "pages.preference.cat.labels.faceCaptureMissing",
            "组件未安装或不可用"));
    }
    bongo_cat_pref_status(context, "face-capture-status", tr(app,
        "pages.preference.cat.labels.faceCaptureStatus", "状态"), status);

    float gain = model->face_capture_gain;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_CENTERED);
    bongo_cat_pref_slider(context, "face-gain", tr(app,
        "pages.preference.cat.labels.faceCaptureGain", "Sensitivity"), "",
        0.2f, &gain, 3.0f, 0.1f, 1.0f);
    if (gain != model->face_capture_gain) {
        model->face_capture_gain = gain;
        app->dirty = true;
    }

    bool flip_h = model->face_capture_flip_h != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MIRROR);
    if (bongo_cat_pref_toggle(context, "face-flip-h", tr(app,
            "pages.preference.cat.labels.faceFlipH", "Flip Horizontally"),
            "", &flip_h)) {
        model->face_capture_flip_h = flip_h ? 1 : 0;
        app->dirty = true;
    }
    bool flip_v = model->face_capture_flip_v != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MIRROR);
    if (bongo_cat_pref_toggle(context, "face-flip-v", tr(app,
            "pages.preference.cat.labels.faceFlipV", "Flip Vertically"),
            "", &flip_v)) {
        model->face_capture_flip_v = flip_v ? 1 : 0;
        app->dirty = true;
    }
}
