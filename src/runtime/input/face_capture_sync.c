/*
 * 面捕的启停入口: 决定用哪个组件、什么时候创建/销毁。
 *
 * 组件按需下载, 所以启动时可能根本没有: 这里全部按"可选"处理 —— 找不到就记一行日志,
 * 面捕保持关闭, 桌宠照常工作。用户真正打开开关时再由设置页调用同一个入口重试。
 *
 * 组件目录约定: <storage_root>/components/facetrack/
 *   bongo_facetrack.dll    组件
 *   models/*.onnx          模型 (mnv3_detection_opt / lm_model0_opt)
 * onnxruntime.dll 与组件放在同一目录 (Windows 从 exe 目录和 DLL 目录都能找到)。
 */
#include "runtime.h"
#include "face_capture.h"

#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 组件根目录: 环境变量优先 (测试/高级用户手动放置), 否则 <storage>/components/facetrack */
static bool facetrack_root(BongoCatApp *app, char *output, size_t capacity) {
    const char *override = SDL_getenv("BONGO_CAT_FACETRACK_DIR");
    if (override && override[0]) {
        snprintf(output, capacity, "%s", override);
        return true;
    }
    if (!app->storage_root[0]) return false;
    return bongo_cat_path_join(output, capacity, app->storage_root,
        "components/facetrack");
}

bool bongo_cat_face_capture_sync(BongoCatApp *app) {
    if (!app) return false;
    const bool wanted = app->settings.model.face_capture != 0;
    if (!wanted) {
        if (app->face_capture) {
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 面捕已关闭");
            bongo_cat_face_capture_destroy(app->face_capture);
            app->face_capture = NULL;
        }
        return true;
    }
    if (app->face_capture) return true;   // 已经开着

    char root[BONGO_CAT_PATH_CAP] = {0};
    if (!facetrack_root(app, root, sizeof(root))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 面捕不可用: 没有存储根目录");
        return false;
    }
    char dll[BONGO_CAT_PATH_CAP] = {0};
    char models[BONGO_CAT_PATH_CAP] = {0};
    if (!bongo_cat_path_join(dll, sizeof(dll), root, "bongo_facetrack.dll") ||
        !bongo_cat_path_join(models, sizeof(models), root, "models")) return false;
    if (!bongo_cat_path_is_file(dll)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[face] 面捕不可用: 缺少组件 %s (需要在设置页下载)", dll);
        return false;
    }
    /* 组件的依赖 (onnxruntime.dll) 与它放在同一目录: SDL_LoadObject 用的是
       LOAD_WITH_ALTERED_SEARCH_PATH, 会先在该 DLL 自己的目录里找依赖, 不需要改工作目录。 */
    char err[256] = {0};
    BongoCatFaceCapture *capture = bongo_cat_face_capture_create(dll, models, err,
        sizeof(err));
    if (!capture) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 面捕不可用: %s", err);
        return false;
    }
    char start_err[256] = {0};
    if (!bongo_cat_face_capture_start(capture,
            (uint32_t)app->settings.model.face_capture_camera, start_err,
            sizeof(start_err))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 摄像头启动失败: %s", start_err);
        bongo_cat_face_capture_destroy(capture);
        return false;
    }
    app->face_capture = capture;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 面捕已开启 (%s)", root);
    return true;
}
