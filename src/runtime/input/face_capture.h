#ifndef BONGO_CAT_FACE_CAPTURE_H
#define BONGO_CAT_FACE_CAPTURE_H

#include "bongo_cat/config.h"
#include "bongo_cat/facetrack_abi.h"

#include <stdbool.h>

/*
 * 面捕运行时: 装载 bongo_facetrack 组件, 在**独立线程**里按 20Hz 取摄像头帧并推理,
 * 主线程只读最近一帧的快照 (原子), 绝不阻塞渲染。
 *
 * 为什么线程化: 单帧推理在 i5-12400F 上约 30 ms (检测 12 + 关键点 22), 放在主循环里
 * 会让 60fps 掉到 30fps 以下; 组件内部还有平滑与中性标定, 必须按真实帧间隔推进。
 *
 * 组件是**可选**的: 没下载/ABI 不匹配/摄像头打不开时, 这里全部安静地返回 false,
 * 上层把「面捕」当成没开启即可 —— 缺组件不能是启动失败。
 */
typedef struct BongoCatFaceCapture BongoCatFaceCapture;

/* 快照: 主线程每帧读一次, 与组件返回的结构体逐字段一致 (便于比对/日志)。 */
typedef struct BongoCatFaceSnapshot {
    BongoCatFaceFrame frame;
    /* 0 = 这一帧没有脸 (confidence 也为 0) */
    bool has_face;
    /* 相机是否已打开 (区分"没开"和"开了但没人") */
    bool camera_open;
    /* 组件自报的信息, 仅日志用 */
    char status[160];
} BongoCatFaceSnapshot;

/* 创建运行时 (不打开摄像头)。dll_path 为空时按默认目录找组件。 */
BongoCatFaceCapture *bongo_cat_face_capture_create(const char *dll_path,
    const char *models_dir, char *err, size_t err_cap);
void bongo_cat_face_capture_destroy(BongoCatFaceCapture *capture);

/* 打开/关闭摄像头与推理线程 (幂等)。camera_id = 0 表示系统默认摄像头。 */
bool bongo_cat_face_capture_start(BongoCatFaceCapture *capture, uint32_t camera_id,
    char *err, size_t err_cap);
void bongo_cat_face_capture_stop(BongoCatFaceCapture *capture);

/* 读最近一帧快照 (线程安全, 不阻塞)。 */
void bongo_cat_face_capture_snapshot(BongoCatFaceCapture *capture,
    BongoCatFaceSnapshot *out);

/* 这一帧是否真的有人在驱动头部 (有脸且置信度够)。鼠标驱动头部据此让位。 */
bool bongo_cat_face_capture_head_active(BongoCatFaceCapture *capture);

/* 组件根目录 (<storage>/components/facetrack, 环境变量优先)。 */
bool bongo_cat_face_capture_root(BongoCatApp *app, char *output, size_t capacity);

/* 开始下载组件 (幂等)。返回 false = 起不来 (写不了目录 / 已在下载)。 */

/* 阻塞式安装 (只给 --install-facetrack 用): 下载、校验、打印结果。 */
bool bongo_cat_face_capture_install_blocking(BongoCatApp *app, int timeout_seconds);
bool bongo_cat_face_capture_download(BongoCatApp *app);

/* 按设置里的开关创建/销毁面捕运行时 (幂等)。缺组件时只记日志并返回 false。 */
bool bongo_cat_face_capture_sync(BongoCatApp *app);

/* 重新做中性基线标定 (组件 v1.1.0 起的可选能力; 老组件上是空操作)。 */
void bongo_cat_face_capture_recalibrate(BongoCatFaceCapture *capture);
bool bongo_cat_face_capture_has_recalibrate(const BongoCatFaceCapture *capture);

/* 枚举摄像头: 返回个数 (<= capacity), ids/names 可为 NULL。names 每条 name_stride 字节。 */
int bongo_cat_face_capture_cameras(uint32_t *ids, char *names, int name_stride,
    int capacity);

/* 组件是否已装载且 ABI 匹配。 */
bool bongo_cat_face_capture_available(const BongoCatFaceCapture *capture);

/* 把快照映射成 Cubism 参数并写入模型 (查参数是否存在, 不存在就跳过)。
   返回 true = 这一帧真的写了参数 (caller 据此决定是否要跳过鼠标驱动头部)。 */
bool bongo_cat_face_capture_apply(BongoCatFaceCapture *capture,
    BongoCatLive2D *live2d, const BongoCatModelPreferences *prefs);

#endif
