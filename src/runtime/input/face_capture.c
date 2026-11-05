/*
 * 面捕运行时: 组件装载 + 摄像头线程 + 快照。
 *
 * 线程模型 (与 windows 的 raw input 接收器同一套路):
 *   采集线程 (20Hz): SDL_AcquireCameraFrame -> 组件 feed() -> 写快照 (加锁)
 *   主线程 (每帧):   读快照 -> 映射成 Cubism 参数
 * 主线程永远只做一次加锁拷贝, 不碰摄像头也不等推理结果。
 */
#include "runtime.h"
#include "face_capture.h"

#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_camera.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct BongoCatFaceCapture {
    /* 组件 (可选): 句柄与符号 */
    SDL_SharedObject *module;
    uint32_t (*abi)(void);
    int (*open)(const char *, char *, size_t);
    int (*feed)(const unsigned char *, int, int, int, BongoCatFaceFrame *);
    void (*close)(void);

    SDL_Thread *thread;
    SDL_Mutex *lock;
    atomic_bool running;
    atomic_bool stop;
    atomic_bool camera_open;
    atomic_uint camera_id;
    /* 快照: 写者持锁, 读者持锁拷贝 */
    BongoCatFaceSnapshot snapshot;
    char status[160];
};

static int capture_thread(void *userdata) {
    BongoCatFaceCapture *capture = userdata;
    /* 摄像头是独立的 SDL 子系统: 主程序只初始化了 VIDEO/EVENTS, 这里不显式初始化的话
       SDL_OpenCamera 会直接报 "Camera subsystem is not initialized"。 */
    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        SDL_LockMutex(capture->lock);
        snprintf(capture->snapshot.status, sizeof(capture->snapshot.status),
            "摄像头子系统初始化失败: %s", SDL_GetError());
        SDL_UnlockMutex(capture->lock);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 摄像头子系统初始化失败: %s",
            SDL_GetError());
        atomic_store(&capture->running, false);
        return 0;
    }
    SDL_CameraID wanted = (SDL_CameraID)atomic_load(&capture->camera_id);
    /* 0 是"系统默认"的约定, 但 SDL_OpenCamera 不接受 0 —— 自己挑第一个可用设备。 */
    if (wanted == 0) {
        int count = 0;
        SDL_CameraID *devices = SDL_GetCameras(&count);
        if (devices && count > 0) wanted = devices[0];
        SDL_free(devices);
        if (wanted == 0) {
            SDL_LockMutex(capture->lock);
            snprintf(capture->snapshot.status, sizeof(capture->snapshot.status),
                "没有找到可用摄像头");
            SDL_UnlockMutex(capture->lock);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 没有找到可用摄像头");
            atomic_store(&capture->running, false);
            return 0;
        }
    }
    SDL_CameraSpec spec = {0};
    spec.format = SDL_PIXELFORMAT_RGB24;
    spec.width = 640;
    spec.height = 480;
    spec.framerate_numerator = 20;
    spec.framerate_denominator = 1;
    SDL_Camera *camera = SDL_OpenCamera(wanted, &spec);
    if (!camera) camera = SDL_OpenCamera(wanted, NULL);
    if (!camera) {
        SDL_LockMutex(capture->lock);
        snprintf(capture->snapshot.status, sizeof(capture->snapshot.status),
            "打开摄像头失败: %s", SDL_GetError());
        SDL_UnlockMutex(capture->lock);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 打开摄像头失败: %s", SDL_GetError());
        atomic_store(&capture->running, false);
        return 0;
    }
    atomic_store(&capture->camera_open, true);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 摄像头已打开");

    BongoCatFaceFrame frame = {0};
    while (!atomic_load(&capture->stop)) {
        Uint64 timestamp = 0;
        SDL_Surface *surface = SDL_AcquireCameraFrame(camera, &timestamp);
        if (!surface) {
            /* 没有新帧就睡一会: 摄像头帧率可能低于推理速度, 空转会把一个核吃满 */
            SDL_Delay(5);
            continue;
        }
        const int result = capture->feed((const unsigned char *)surface->pixels,
            surface->w, surface->h, surface->pitch, &frame);
        SDL_ReleaseCameraFrame(camera, surface);
        SDL_LockMutex(capture->lock);
        if (result > 0) {
            capture->snapshot.frame = frame;
            capture->snapshot.frame.timestamp_ns = (uint64_t)timestamp;
            capture->snapshot.has_face = true;
            snprintf(capture->snapshot.status, sizeof(capture->snapshot.status),
                "在跟踪 (fps %.1f, 置信度 %.2f)", (double)frame.fps,
                (double)frame.confidence);
        } else if (result == 0) {
            capture->snapshot.has_face = false;
            capture->snapshot.frame.confidence = 0.0f;
            snprintf(capture->snapshot.status, sizeof(capture->snapshot.status),
                "没检测到人脸");
        } else {
            capture->snapshot.has_face = false;
            snprintf(capture->snapshot.status, sizeof(capture->snapshot.status),
                "推理失败");
        }
        SDL_UnlockMutex(capture->lock);
    }
    SDL_CloseCamera(camera);
    atomic_store(&capture->camera_open, false);
    atomic_store(&capture->running, false);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 摄像头已关闭");
    return 0;
}

/* 装载组件: 任何一步失败都只是"面捕不可用", 不是错误退出。 */
static bool load_module(BongoCatFaceCapture *capture, const char *dll_path,
    char *err, size_t err_cap) {
    /* 让平台层先把组件自带的 onnxruntime.dll 装进来 (Windows 上按 PATH 找同名 DLL 很容易撞到别人的) */
    bongo_cat_platform_preload_facetrack_runtime(dll_path);
    capture->module = SDL_LoadObject(dll_path);
    if (!capture->module) {
        if (err) snprintf(err, err_cap, "找不到面捕组件: %s", dll_path);
        return false;
    }
    capture->abi = (uint32_t (*)(void))(void *)SDL_LoadFunction(capture->module,
        "bongo_cat_facetrack_abi");
    capture->open = (int (*)(const char *, char *, size_t))(void *)
        SDL_LoadFunction(capture->module, "bongo_cat_facetrack_open");
    capture->feed = (int (*)(const unsigned char *, int, int, int,
        BongoCatFaceFrame *))(void *)SDL_LoadFunction(capture->module,
        "bongo_cat_facetrack_feed");
    capture->close = (void (*)(void))(void *)SDL_LoadFunction(capture->module,
        "bongo_cat_facetrack_close");
    if (!capture->abi || !capture->open || !capture->feed || !capture->close) {
        if (err) snprintf(err, err_cap, "组件导出符号不全 (不是 BongoCat 面捕组件)");
        return false;
    }
    const uint32_t version = capture->abi();
    if (version != BONGO_CAT_FACETRACK_ABI) {
        if (err) snprintf(err, err_cap, "组件 ABI %u 与宿主 %u 不匹配, 请重新下载组件",
            version, BONGO_CAT_FACETRACK_ABI);
        return false;
    }
    return true;
}

BongoCatFaceCapture *bongo_cat_face_capture_create(const char *dll_path,
    const char *models_dir, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!dll_path || !dll_path[0] || !models_dir || !models_dir[0]) {
        if (err) snprintf(err, err_cap, "缺少组件或模型路径");
        return NULL;
    }
    BongoCatFaceCapture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        if (err) snprintf(err, err_cap, "内存不足");
        return NULL;
    }
    capture->lock = SDL_CreateMutex();
    if (!capture->lock) {
        free(capture);
        if (err) snprintf(err, err_cap, "创建互斥锁失败");
        return NULL;
    }
    if (!load_module(capture, dll_path, err, err_cap)) {
        bongo_cat_face_capture_destroy(capture);
        return NULL;
    }
    char open_err[256] = {0};
    if (capture->open(models_dir, open_err, sizeof(open_err)) != 0) {
        if (err) snprintf(err, err_cap, "%s", open_err[0] ? open_err : "组件初始化失败");
        bongo_cat_face_capture_destroy(capture);
        return NULL;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 面捕组件就绪 (ABI %u): %s",
        capture->abi(), dll_path);
    return capture;
}

void bongo_cat_face_capture_destroy(BongoCatFaceCapture *capture) {
    if (!capture) return;
    bongo_cat_face_capture_stop(capture);
    if (capture->close) capture->close();
    if (capture->module) SDL_UnloadObject(capture->module);
    if (capture->lock) SDL_DestroyMutex(capture->lock);
    free(capture);
}

bool bongo_cat_face_capture_start(BongoCatFaceCapture *capture, uint32_t camera_id,
    char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!capture || !capture->feed) {
        if (err) snprintf(err, err_cap, "面捕组件不可用");
        return false;
    }
    if (atomic_load(&capture->running)) return true;
    atomic_store(&capture->camera_id, camera_id);
    atomic_store(&capture->stop, false);
    atomic_store(&capture->running, true);
    capture->thread = SDL_CreateThread(capture_thread, "face-capture", capture);
    if (!capture->thread) {
        atomic_store(&capture->running, false);
        if (err) snprintf(err, err_cap, "创建采集线程失败: %s", SDL_GetError());
        return false;
    }
    return true;
}

void bongo_cat_face_capture_stop(BongoCatFaceCapture *capture) {
    if (!capture || !capture->thread) return;
    atomic_store(&capture->stop, true);
    SDL_WaitThread(capture->thread, NULL);
    capture->thread = NULL;
    atomic_store(&capture->running, false);
}

void bongo_cat_face_capture_snapshot(BongoCatFaceCapture *capture,
    BongoCatFaceSnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!capture || !capture->lock) return;
    SDL_LockMutex(capture->lock);
    *out = capture->snapshot;
    out->camera_open = atomic_load(&capture->camera_open);
    SDL_UnlockMutex(capture->lock);
}

bool bongo_cat_face_capture_head_active(BongoCatFaceCapture *capture) {
    if (!capture) return false;
    BongoCatFaceSnapshot snapshot;
    bongo_cat_face_capture_snapshot(capture, &snapshot);
    return snapshot.has_face && snapshot.frame.confidence >= 0.15f;
}

bool bongo_cat_face_capture_available(const BongoCatFaceCapture *capture) {
    return capture && capture->feed != NULL;
}
