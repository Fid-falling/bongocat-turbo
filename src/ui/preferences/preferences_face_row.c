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
#include "../../runtime/update/facetrack_install.h"
#include "bongo_cat/path.h"

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

    /* 组件缺失时先给"下载"，下完了自动接上；已经装的直接显示运行状态。 */
    char root[BONGO_CAT_PATH_CAP] = {0};
    const bool have_root = bongo_cat_face_capture_root(app, root, sizeof(root));
    const bool present = have_root && bongo_cat_facetrack_install_present(root);
    if (!present) {
        BongoCatFacetrackInstallSnapshot download = {0};
        if (app->facetrack_install)
            bongo_cat_facetrack_install_snapshot(app->facetrack_install, &download);
        if (download.state == BONGO_CAT_FACETRACK_INSTALL_WORKING) {
            char status[192];
            snprintf(status, sizeof(status), "%u/%u %s  %.1f/%.1f MB",
                download.file_index, download.file_count, download.file_name,
                (double)download.bytes / (1024.0 * 1024.0),
                (double)download.total / (1024.0 * 1024.0));
            bongo_cat_pref_status(context, "face-capture-status", tr(app,
                "pages.preference.cat.labels.faceCaptureStatus", "状态"), status);
        } else {
            if (download.state == BONGO_CAT_FACETRACK_INSTALL_DONE) {
                /* 刚下完: 立刻装载组件, 状态行下一帧就会变成"正在跟踪" */
                bongo_cat_facetrack_install_destroy(app->facetrack_install);
                app->facetrack_install = NULL;
                bongo_cat_face_capture_sync(app);
            }
            /* 借用开关行的整行点击当"下载"按钮: 点一下就开始, 打开状态的开关只表示
               "本次要下", 真正的进度在状态行里显示。失败原因挂在说明文字里。 */
            bool download_wanted = download.state == BONGO_CAT_FACETRACK_INSTALL_WORKING;
            const char *hint = download.state == BONGO_CAT_FACETRACK_INSTALL_FAILED
                ? download.error : tr(app,
                    "pages.preference.cat.labels.faceCaptureDownloadHint",
                    "组件约 6.6 MB，只需下载一次");
            bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
            if (bongo_cat_pref_toggle(context, "face-capture-download", tr(app,
                    "pages.preference.cat.labels.faceCaptureDownload",
                    "Download Component (6.6 MB)"), hint, &download_wanted))
                bongo_cat_face_capture_download(app);
            bongo_cat_pref_status(context, "face-capture-status", tr(app,
                "pages.preference.cat.labels.faceCaptureStatus", "状态"),
                download.state == BONGO_CAT_FACETRACK_INSTALL_IDLE
                    ? tr(app, "pages.preference.cat.labels.faceCaptureMissing",
                        "组件未安装或不可用")
                    : (download.state == BONGO_CAT_FACETRACK_INSTALL_FAILED
                        ? download.error : "下载中…"));
        }
    } else {
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
    }

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
