/*
 * 「开启面捕」的确认框。
 *
 * 为什么需要确认: 组件是按需下载的 (约 6.6 MB), 用户点开关时可能正在计费网络/限速网络,
 * 直接开始下载不合适。确认框说清楚"要下载什么、多大、只下这一次", 用户点确定后再下。
 *
 * 用系统原生对话框 (SDL_ShowMessageBox) 而不是自绘模态: 这是一个一次性的、阻塞的"要不要"
 * 询问, 原生框在用户关掉设置窗口/切到别的程序时行为最稳, 也不需要跟着主题适配。
 */
#include "face_capture.h"

#include "../update/facetrack_install.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <SDL3/SDL_messagebox.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    if (!app || !app->i18n) return fallback;
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/* 弹确认框。返回 true = 用户同意下载。 */
bool bongo_cat_face_capture_confirm_download(BongoCatApp *app) {
    if (!app) return false;
    const SDL_MessageBoxButtonData buttons[] = {
        /* 默认按钮放"取消": 回车不应该意外触发一次几十 MB 的下载 */
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0,
            tr(app, "pages.preference.face.confirmCancel", "Cancel") },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1,
            tr(app, "pages.preference.face.confirmOk", "Download") }
    };
    /* 组件大小**从清单里取**, 不写死: 换了运行时/加了模型, 包大小会变 (6.6 MB -> 16.5 MB),
       写死的话确认框会一直骗人。拿不到 (还没检查过) 就用不带数字的文案。 */
    char size_text[32] = {0};
    if (app->facetrack_install) {
        BongoCatFacetrackInstallSnapshot snapshot;
        bongo_cat_facetrack_install_snapshot(app->facetrack_install, &snapshot);
        if (snapshot.total > 0)
            snprintf(size_text, sizeof(size_text), "%.1f MB",
                (double)snapshot.total / (1024.0 * 1024.0));
    }
    static char body[512];
    if (size_text[0])
        snprintf(body, sizeof(body), tr(app,
            "pages.preference.face.confirmBody",
            "Face capture needs a one-time component download (about %s). "
            "It works offline afterwards.\n\nDownload now?"), size_text);
    else
        snprintf(body, sizeof(body), "%s", tr(app,
            "pages.preference.face.confirmBodyNoSize",
            "Face capture needs a one-time component download. "
            "It works offline afterwards.\n\nDownload now?"));
    const SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_INFORMATION,
        NULL,
        tr(app, "pages.preference.face.confirmTitle", "Face Capture"),
        body,
        SDL_arraysize(buttons),
        buttons,
        NULL
    };
    int pressed = 0;
    if (!SDL_ShowMessageBox(&data, &pressed)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 确认框打不开: %s",
            SDL_GetError());
        return false;
    }
    return pressed == 1;
}
