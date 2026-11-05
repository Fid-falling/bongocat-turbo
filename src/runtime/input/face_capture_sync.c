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

#include "../update/facetrack_install.h"
#include "../lifecycle/storage_paths.h"

#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 组件根目录: 环境变量优先 (测试/高级用户手动放置), 否则 <storage>/components/facetrack */
bool bongo_cat_face_capture_root(BongoCatApp *app, char *output,
    size_t capacity) {
    const char *override = SDL_getenv("BONGO_CAT_FACETRACK_DIR");
    if (override && override[0]) {
        snprintf(output, capacity, "%s", override);
        return true;
    }
    /* app->storage_root 只有用户显式传 --storage-root 时才有值; 正常安装/便携启动下
       它是空的, 得问 storage_paths 要解析后的根目录 —— 之前只看 storage_root, 于是
       正常启动的用户点"下载"毫无反应 (实测: 开关变蓝, 状态仍是"组件未安装")。 */
    char base[BONGO_CAT_PATH_CAP] = {0};
    if (app && app->storage_root[0])
        snprintf(base, sizeof(base), "%s", app->storage_root);
    else if (!bongo_cat_storage_paths_root(base, sizeof(base)) || !base[0])
        return false;
    return bongo_cat_path_join(output, capacity, base, "components/facetrack");
}

bool bongo_cat_face_capture_sync(BongoCatApp *app) {
    if (!app) return false;
    const bool wanted = app->settings.model.face_capture != 0 ||
        app->force_face_capture;
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
    if (!bongo_cat_face_capture_root(app, root, sizeof(root))) {
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
    BongoCatFaceCapture *capture = bongo_cat_face_capture_create(dll, models,
        app->settings.model.face_capture_gpu != 0, err, sizeof(err));
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

bool bongo_cat_face_capture_download(BongoCatApp *app) {
    if (!app) return false;
    char root[BONGO_CAT_PATH_CAP] = {0};
    if (!bongo_cat_face_capture_root(app, root, sizeof(root))) return false;
    if (!app->facetrack_install) {
        app->facetrack_install = bongo_cat_facetrack_install_create(root);
        if (!app->facetrack_install) return false;
    }
    if (!bongo_cat_facetrack_install_start(app->facetrack_install)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[face] 下载启动失败 (目录不可写?): %s", root);
        return false;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 开始下载组件到 %s", root);
    return true;
}

bool bongo_cat_face_capture_install_blocking(BongoCatApp *app, int timeout_seconds) {
    if (!app) return false;
    char root[BONGO_CAT_PATH_CAP] = {0};
    if (!bongo_cat_face_capture_root(app, root, sizeof(root))) return false;
    if (bongo_cat_facetrack_install_present(root)) {
        printf("[face] 组件已就位: %s\n", root);
        return true;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 开始下载组件: %s", root);
    BongoCatFacetrackInstall *install = bongo_cat_facetrack_install_create(root);
    if (!install) return false;
    if (!bongo_cat_facetrack_install_start(install)) {
        bongo_cat_facetrack_install_destroy(install);
        return false;
    }
    const Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_seconds * 1000u;
    BongoCatFacetrackInstallSnapshot snapshot = {0};
    while (SDL_GetTicks() < deadline) {
        bongo_cat_facetrack_install_snapshot(install, &snapshot);
        if (snapshot.state == BONGO_CAT_FACETRACK_INSTALL_DONE ||
            snapshot.state == BONGO_CAT_FACETRACK_INSTALL_FAILED) break;
        SDL_Delay(100);
    }
    const bool ok = snapshot.state == BONGO_CAT_FACETRACK_INSTALL_DONE;
    printf("[face] 下载%s%s%s\n", ok ? "完成" : "失败",
        snapshot.error[0] ? ": " : "", snapshot.error);
    bongo_cat_facetrack_install_destroy(install);
    return ok;
}

/* TEMP-REPRO: 复现"面捕运行中关闭会崩"。阶段:
   0 = 关, 1 = 已开启等待, 2 = 关闭并退出。由 update_model 每帧推进 (主线程 + 设置窗口
   正在绘制的真实时序)。 */
static int g_selftest_stage = -1;
static Uint64 g_selftest_deadline = 0;

void bongo_cat_face_capture_toggle_selftest(BongoCatApp *app) {
    if (!SDL_getenv("BONGO_CAT_FACE_TOGGLE_TEST")) return;
    if (g_selftest_stage >= 0 || !app) return;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face][selftest] 打开");
    app->settings.model.face_capture = 1;
    bongo_cat_face_capture_sync(app);
    g_selftest_stage = 1;
    g_selftest_deadline = SDL_GetTicks() + 3000;
}

void bongo_cat_face_capture_toggle_selftest_tick(BongoCatApp *app) {
    if (g_selftest_stage != 1 || !app) return;
    if (SDL_GetTicks() < g_selftest_deadline) return;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face][selftest] 关闭 (主循环第 %d 帧后)",
        g_selftest_stage);
    app->settings.model.face_capture = 0;
    bongo_cat_face_capture_sync(app);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face][selftest] 关闭完成");
    g_selftest_stage = 2;
    app->running = false;
}
