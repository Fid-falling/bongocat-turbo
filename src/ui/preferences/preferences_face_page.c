/*
 * 「面捕」独立设置页。
 *
 * 为什么单独一页 (而不是塞进「互动」): 这一页自带组件下载、设备状态、灵敏度与两个反转,
 * 放在「互动」里会把那一页撑到需要滚动, 而且下载是一个有进度、会失败的异步动作, 需要
 * 自己的状态行。侧栏独立后, 用户找"面捕"只要看名字。
 *
 * 交互契约:
 *   1. 打开开关 -> 先弹确认框 (说明要下载约 6.6 MB 的组件), 确认后**自动开始下载**;
 *   2. 下载中可以关掉; 关掉开关只停止使用, 已下载的组件保留 (下次打开不再下载);
 *   3. 组件已就位时开关直接生效, 不再弹确认框。
 */
#include "preferences_state.h"
#include "preferences_internal.h"

#include "preferences_widgets.h"

#include "../../runtime/input/face_capture.h"
#include "../../runtime/update/facetrack_install.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>

#include <stdio.h>

static const char *face_tr(BongoCatApp *app, const char *key,
    const char *fallback) {
    return app && app->i18n ? bongo_cat_i18n_get(app->i18n, key, fallback)
        : fallback;
}

/* 组件是否已装好 (DLL + onnxruntime + 两个模型)。 */
static bool component_ready(BongoCatApp *app) {
    char root[BONGO_CAT_PATH_CAP] = {0};
    if (!bongo_cat_face_capture_root(app, root, sizeof(root))) return false;
    return bongo_cat_facetrack_install_present(root);
}

void bongo_cat_preferences_page_face(BongoCatPreferences *value,
    struct nk_context *context) {
    if (!value) return;
    bongo_cat_preferences_face_content(value->app, context);
}

/* 正文: 由 preferences_face_row.c 提供 (行控件在那里), 这里只负责页面骨架。 */
void bongo_cat_preferences_face_content(BongoCatApp *app,
    struct nk_context *context) {
    BongoCatModelPreferences *model = &app->settings.model;
    /* 不画分区标题: 页面标题本身就叫「面部捕捉」, 再来一个同名分区会重复两遍
       (截图里一眼就能看出来)。 */
    bongo_cat_pref_section(context, face_tr(app,
        "pages.preference.face.section", "设置"));

    bool enabled = model->face_capture != 0;
    const bool ready = component_ready(app);
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    if (bongo_cat_pref_toggle(context, "face-page-enable", face_tr(app,
            "pages.preference.cat.labels.faceCapture", "Face Capture"),
            face_tr(app, "pages.preference.cat.labels.faceCaptureHint",
                "用摄像头驱动表情"), &enabled)) {
        if (enabled && !ready) {
            /* 组件还没下载: 不直接开, 先问用户 (要联网下 6.6 MB), 确认后自动下 */
            if (bongo_cat_face_capture_confirm_download(app)) {
                model->face_capture = 1;
                app->dirty = true;
                bongo_cat_face_capture_download(app);
            }
        } else {
            model->face_capture = enabled ? 1 : 0;
            app->dirty = true;
            bongo_cat_face_capture_sync(app);
        }
    }

    /* 状态行 */
    char status[192];
    BongoCatFacetrackInstallSnapshot download = {0};
    if (app->facetrack_install)
        bongo_cat_facetrack_install_snapshot(app->facetrack_install, &download);
    if (download.state == BONGO_CAT_FACETRACK_INSTALL_WORKING) {
        /* 下载进度要说到能判断"要不要等"的程度: 百分比 + 当前文件 + 已下/总量 + 速度。 */
        const double done_mb = (double)download.bytes / (1024.0 * 1024.0);
        const double total_mb = (double)download.total / (1024.0 * 1024.0);
        const int percent = download.total
            ? (int)(download.bytes * 100u / download.total) : 0;
        char speed[32];
        if (download.bytes_per_second >= 1024u * 1024u)
            snprintf(speed, sizeof(speed), "%.1f MB/s",
                (double)download.bytes_per_second / (1024.0 * 1024.0));
        else if (download.bytes_per_second)
            snprintf(speed, sizeof(speed), "%llu KB/s",
                (unsigned long long)(download.bytes_per_second / 1024u));
        else
            snprintf(speed, sizeof(speed), "…");
        snprintf(status, sizeof(status), "%s %d%%  ·  %u/%u %s  ·  %.1f/%.1f MB  ·  %s",
            face_tr(app, "pages.preference.cat.labels.faceCaptureDownloading", "下载中"),
            percent, download.file_index, download.file_count, download.file_name,
            done_mb, total_mb, speed);
    } else if (download.state == BONGO_CAT_FACETRACK_INSTALL_FAILED) {
        snprintf(status, sizeof(status), "%s", download.error);
    } else if (!ready) {
        snprintf(status, sizeof(status), "%s", face_tr(app,
            "pages.preference.cat.labels.faceCaptureMissing", "组件未安装或不可用"));
    } else if (app->face_capture &&
            bongo_cat_face_capture_available(app->face_capture)) {
        BongoCatFaceSnapshot snapshot;
        bongo_cat_face_capture_snapshot(app->face_capture, &snapshot);
        snprintf(status, sizeof(status), "%s",
            snapshot.status[0] ? snapshot.status : "已开启");
    } else {
        snprintf(status, sizeof(status), "%s", face_tr(app,
            "pages.preference.cat.labels.faceCaptureReady", "组件已就绪"));
    }
    bongo_cat_pref_status(context, "face-page-status", face_tr(app,
        "pages.preference.cat.labels.faceCaptureStatus", "状态"), status);
    /* 下载完成: 立刻装载并打开摄像头 */
    if (download.state == BONGO_CAT_FACETRACK_INSTALL_DONE) {
        bongo_cat_facetrack_install_destroy(app->facetrack_install);
        app->facetrack_install = NULL;
        model->face_capture = 1;
        app->dirty = true;
        bongo_cat_face_capture_sync(app);
    }

    if (!model->face_capture) {
        /* 没开时不显示后面的调节项, 免得用户以为它们在起作用 */
        return;
    }
    /* 滑杆控件统一按百分比显示 ("%"), 所以这里用 20%..300% 表示 0.2..3.0 倍;
       直接传 1.0 会显示成 "1%" (截图里就是这样)。 */
    float gain_percent = model->face_capture_gain * 100.0f;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_CENTERED);
    if (bongo_cat_pref_slider(context, "face-page-gain", face_tr(app,
            "pages.preference.cat.labels.faceCaptureGain", "Sensitivity"), "",
            20.0f, &gain_percent, 300.0f, 5.0f, 100.0f)) {
        model->face_capture_gain = gain_percent / 100.0f;
        app->dirty = true;
    }
    bool flip_h = model->face_capture_flip_h != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MIRROR);
    if (bongo_cat_pref_toggle(context, "face-page-flip-h", face_tr(app,
            "pages.preference.cat.labels.faceFlipH", "Flip Horizontally"), "",
            &flip_h)) {
        model->face_capture_flip_h = flip_h ? 1 : 0;
        app->dirty = true;
    }
    bool flip_v = model->face_capture_flip_v != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MIRROR);
    if (bongo_cat_pref_toggle(context, "face-page-flip-v", face_tr(app,
            "pages.preference.cat.labels.faceFlipV", "Flip Vertically"), "",
            &flip_v)) {
        model->face_capture_flip_v = flip_v ? 1 : 0;
        app->dirty = true;
    }
}
