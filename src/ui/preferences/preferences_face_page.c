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
#include "preferences_notice.h"
#include "preferences_internal.h"

#include "preferences_widgets.h"

#include "../theme/ui_catime.h"

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
/*
 * 页面底部的下载进度条。
 *
 * 为什么不用 nk_progress: 它要求 NK_PROGRESSBAR 打开, 样式也是方格填充, 和这里其它
 * 圆角/柔和元素不搭。自己画: 圆角轨道 + 圆角填充 + 居中说明文字。
 * 位置按用户要求放在内容区最后一行 (页面下方), 不挤在状态行里。
 */
static void face_download_bar(struct nk_context *context,
    const BongoCatFacetrackInstallSnapshot *download, BongoCatUIPalette palette,
    const char *text) {
    nk_layout_row_dynamic(context, 40.0f, 1);
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    const float rounding = bounds.h * 0.5f;
    nk_fill_rect(canvas, bounds, rounding, palette.field);
    float ratio = download->total
        ? (float)((double)download->bytes / (double)download->total) : 0.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio > 0.0f)
        nk_fill_rect(canvas, nk_rect(bounds.x, bounds.y, bounds.w * ratio,
            bounds.h), rounding, palette.accent);
    if (text && text[0]) {
        const struct nk_user_font *font = context->style.font;
        if (font) {
            const float text_width = font->width(font->userdata, font->height,
                text, nk_strlen(text));
            const struct nk_rect label = nk_rect(
                bounds.x + (bounds.w - text_width) * 0.5f,
                bounds.y + (bounds.h - font->height) * 0.5f, text_width + 1.0f,
                font->height);
            nk_draw_text(canvas, label, text, nk_strlen(text), font,
                nk_rgba(0, 0, 0, 0), palette.text);
        }
    }
}

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
            /* 组件还没下载: 不直接开, 延到帧末弹确认框 (见 app.h 的说明) */
            app->face_confirm_pending = true;
            app->face_confirm_update = false;
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
    } else if (download.state == BONGO_CAT_FACETRACK_INSTALL_DONE &&
            download.latest_version[0] && !download.update_available) {
        snprintf(status, sizeof(status), "%s (%s %s)", face_tr(app,
            "pages.preference.cat.labels.faceUpToDate", "组件已是最新"),
            face_tr(app, "pages.preference.cat.labels.faceVersion", "版本"),
            download.latest_version);
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
    /* 有新版本: 给一行"更新"按钮, 点了弹确认再下载 */
    if (download.update_available && download.latest_version[0] &&
            download.state != BONGO_CAT_FACETRACK_INSTALL_WORKING) {
        char label[96];
        snprintf(label, sizeof(label), "%s %s", face_tr(app,
            "pages.preference.cat.labels.faceUpdateAvailable", "组件有新版本"),
            download.latest_version);
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        if (bongo_cat_pref_button(context, "face-page-update", label, "",
                face_tr(app, "pages.preference.cat.labels.faceUpdateAction",
                    "Update"))) {
            /* 与首次下载同一个确认框, 同样延到帧末 */
            app->face_confirm_pending = true;
            app->face_confirm_update = true;
        }
    }

    /* 预览窗: 类似 VTS 的摄像头画面, 让用户确认"看到的确实是自己的脸" */
    bongo_cat_preferences_face_preview(app->preferences, context);

    /* 手动检查更新: 自动检查每次会话只跑一次, 而且可能因为网络失败; 给用户一个重试入口 */
    if (ready) {
        char detail[128];
        if (download.state == BONGO_CAT_FACETRACK_INSTALL_CHECKING) {
            snprintf(detail, sizeof(detail), "%s", face_tr(app,
                "pages.preference.cat.labels.faceChecking", "正在检查…"));
        } else if (download.state == BONGO_CAT_FACETRACK_INSTALL_FAILED &&
                download.error[0]) {
            snprintf(detail, sizeof(detail), "%s", download.error);
        } else if (download.installed_version[0]) {
            snprintf(detail, sizeof(detail), "%s v%s", face_tr(app,
                "pages.preference.cat.labels.faceVersion", "版本"),
                download.installed_version);
        } else {
            detail[0] = 0;
        }
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        if (bongo_cat_pref_button(context, "face-page-check-update",
                face_tr(app, "pages.preference.cat.labels.faceCheckUpdate",
                    "检查组件更新"), detail,
                face_tr(app, "pages.preference.cat.labels.faceCheckUpdateAction",
                    "检查"))) {
            char root[BONGO_CAT_PATH_CAP] = {0};
            if (bongo_cat_face_capture_root(app, root, sizeof(root))) {
                if (app->facetrack_install) {
                    bongo_cat_facetrack_install_destroy(app->facetrack_install);
                    app->facetrack_install = NULL;
                }
                app->facetrack_install = bongo_cat_facetrack_install_create(root);
                if (app->facetrack_install)
                    bongo_cat_facetrack_install_check_start(app->facetrack_install);
                app->facetrack_checked = true;
            }
        }
    }

    /* 底部进度条: 只在"正在下载"和"刚下完"两种状态下出现, 用户要求放在页面下方 */
    if (!download.check_mode &&
            (download.state == BONGO_CAT_FACETRACK_INSTALL_WORKING ||
             download.state == BONGO_CAT_FACETRACK_INSTALL_DONE)) {
        char bar_text[128];
        if (download.state == BONGO_CAT_FACETRACK_INSTALL_DONE) {
            snprintf(bar_text, sizeof(bar_text), "%s 100%%",
                face_tr(app, "pages.preference.cat.labels.faceCaptureDownloading",
                    "下载中"));
        } else {
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
            snprintf(bar_text, sizeof(bar_text), "%d%%  ·  %.1f/%.1f MB  ·  %s",
                percent, (double)download.bytes / (1024.0 * 1024.0),
                (double)download.total / (1024.0 * 1024.0), speed);
        }
        BongoCatUIPalette palette = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
        face_download_bar(context, &download, palette, bar_text);
    }

    /* 下载完成: 立刻装载并打开摄像头。**检查完成不算** —— 两者都停在 DONE,
       但检查模式下把组件当"刚装好"会平白把面捕打开并重建采集线程 (实测会崩)。 */
    if (download.state == BONGO_CAT_FACETRACK_INSTALL_DONE && !download.check_mode) {
        bongo_cat_facetrack_install_destroy(app->facetrack_install);
        app->facetrack_install = NULL;
        model->face_capture = 1;
        app->dirty = true;
        /* 正在跑的组件是"已装载的旧 DLL", 新文件要重启后才用得上 —— 如实告诉用户,
           不要让人以为"更新完了却还是旧行为"。 */
        if (app->face_capture) {
            bongo_cat_face_capture_stop(app->face_capture);
            app->face_capture = NULL;
        }
        bongo_cat_preferences_notice_show(app, face_tr(app,
            "pages.preference.cat.labels.faceUpdateRestart",
            "组件已更新，重启后生效"), false);
    }

    /* 页面首次打开时查一次组件版本 (后台线程, 不挡界面) */
    if (ready && !app->facetrack_install && !app->facetrack_checked) {
        char root[BONGO_CAT_PATH_CAP] = {0};
        if (bongo_cat_face_capture_root(app, root, sizeof(root))) {
            app->facetrack_install = bongo_cat_facetrack_install_create(root);
            if (app->facetrack_install &&
                    bongo_cat_facetrack_install_check_start(app->facetrack_install))
                app->facetrack_checked = true;
        }
    }

    if (!model->face_capture) {
        /* 没开时不显示后面的调节项, 免得用户以为它们在起作用 */
        return;
    }
    /* GPU 推理开关: 默认关 —— 这套模型太小, 实测 CPU 26.7ms/帧、GPU 45.6ms/帧,
       GPU 只对 CPU 很弱的老机器有意义。改后端要重建推理会话, 所以切换后重启采集。 */
    {
        bool use_gpu = model->face_capture_gpu != 0;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        if (bongo_cat_pref_toggle(context, "face-page-gpu", face_tr(app,
                "pages.preference.cat.labels.faceGpu", "GPU 推理"),
                face_tr(app, "pages.preference.cat.labels.faceGpuHint",
                    "用显卡推理（DirectML）。模型很小，通常比 CPU 更慢，仅老 CPU 有收益"),
                &use_gpu)) {
            model->face_capture_gpu = use_gpu ? 1 : 0;
            app->dirty = true;
            /* 换后端必须重开会话 (set_backend 只在 open 之前生效)。这里只停、置空,
               由主循环等旧线程退出后重建 —— 立刻重建会和还在跑的线程抢同一个组件实例。 */
            bongo_cat_face_capture_stop(app->face_capture);
            app->face_capture = NULL;
        }
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
    /* 摄像头选择: 枚举设备, 第一项是"系统默认"。切换后要重启采集线程。 */
    {
        /* 枚举结果要缓存: SDL_GetCameras 每帧调一次既慢, 又可能和正在跑采集线程的
           SDL 相机后端并发 (用户切摄像头时崩溃的嫌疑就是这个)。 */
        static uint32_t ids[16];
        static char names[16 * 64];
        static int count = 0;
        static Uint64 enumerated_at = 0;
        const Uint64 now = SDL_GetTicks();
        if (app->face_capture) {
            /* 采集线程在跑: 只读它记下的设备列表, 绝不自己调 SDL_GetCameras */
            count = bongo_cat_face_capture_known_cameras(app->face_capture, ids,
                names, 64, 16);
        } else if (enumerated_at == 0 || now - enumerated_at > 5000) {
            count = bongo_cat_face_capture_cameras(ids, names, 64, 16);
            enumerated_at = now;
        }
        if (count > 0) {
            static const char *items[17];
            static char labels[17][96];
            items[0] = face_tr(app,
                "pages.preference.cat.labels.faceCameraDefault", "系统默认");
            int selected = 0;
            for (int i = 0; i < count; ++i) {
                snprintf(labels[i], sizeof(labels[i]), "%s", names + i * 64);
                items[i + 1] = labels[i];
                if ((uint32_t)model->face_capture_camera == ids[i]) selected = i + 1;
            }
            bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
            const int picked = bongo_cat_pref_combo(context, "face-page-camera",
                face_tr(app, "pages.preference.cat.labels.faceCamera",
                    "Camera"), "", items, count + 1, selected);
            if (picked != selected) {
                model->face_capture_camera = picked == 0 ? 0 : (int)ids[picked - 1];
                app->dirty = true;
                /* 设备换了: 同样交给主循环重建 */
                bongo_cat_face_capture_stop(app->face_capture);
                app->face_capture = NULL;
            }
        }
    }

    /* 中心校准 (等同 VTS 的"重新校准中心"): 组件 v1.1.0 起才有 */
    if (app->face_capture && bongo_cat_face_capture_has_recalibrate(app->face_capture)) {
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_CENTERED);
        if (bongo_cat_pref_button(context, "face-page-recalibrate", face_tr(app,
                "pages.preference.cat.labels.faceRecalibrate", "Center Calibration"),
                face_tr(app, "pages.preference.cat.labels.faceRecalibrateHint",
                    "正对摄像头后点击，重新采样中性表情"), face_tr(app,
                "pages.preference.cat.labels.faceRecalibrateAction", "Recalibrate"))) {
            bongo_cat_face_capture_recalibrate(app->face_capture);
            bongo_cat_preferences_notice_show(app, face_tr(app,
                "pages.preference.cat.labels.faceRecalibrateStarted",
                "正在重新校准，请保持正对摄像头"), false);
        }
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


