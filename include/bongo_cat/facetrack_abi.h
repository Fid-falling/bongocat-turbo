#ifndef BONGO_CAT_FACETRACK_ABI_H
#define BONGO_CAT_FACETRACK_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 面捕组件 (bongo_facetrack.dll) 与宿主之间的 ABI v1。
 *
 * 设计纪律与渲染组件 (renderer_module_abi.h) 一致:
 *   1. 结构体头两个字段是 abi_version / struct_size, 双向校验, 不匹配就拒绝装载;
 *   2. 组件不链宿主的 SDL/日志/Cubism, 一切外部资源由宿主喂进来 (RGB 帧、模型目录);
 *   3. "这一帧没有人脸" 是正常状态 (feed 返回 0), 上层据此把参数回落到中性;
 *   4. 组件自己跑推理与平滑, 宿主只读这些 float, 所以换模型/换运行时 = 换一个 DLL。
 *
 * 组件是**按需下载**的 (约 13 MB: 模型 2.4 MB + ONNX Runtime 11 MB), 主包不带它。
 */
#define BONGO_CAT_FACETRACK_ABI 1u

typedef struct BongoCatFaceFrame {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t timestamp_ns;

    /* 头部姿态, 单位度, 已缩放到 ±30 度区间 (映射时再乘系数) */
    float yaw_deg, pitch_deg, roll_deg;

    /* 0 = 闭 1 = 睁 */
    float eye_left, eye_right;

    /* 0 = 闭 1 = 张 */
    float mouth_open;
    /* -1 = 撇嘴 0 = 平 1 = 笑 */
    float mouth_form;

    /* 0 = 平 1 = 挑 */
    float brow_left, brow_right;
    float cheek;

    /* -1..1 眼珠方向 (当前实现用头姿近似) */
    float gaze_x, gaze_y;

    /* 0 = 没有脸 1 = 很确定 */
    float confidence;
    /* 组件自报的实测帧率, 仅日志/UI 用 */
    float fps;
} BongoCatFaceFrame;

/* Windows 下导出的函数必须带 __declspec(dllexport), 否则 DLL 里没有符号。 */
#if defined(_WIN32) && defined(FACETRACK_DLL)
#define BONGO_CAT_FACETRACK_EXPORT __declspec(dllexport)
#else
#define BONGO_CAT_FACETRACK_EXPORT
#endif

BONGO_CAT_FACETRACK_EXPORT uint32_t bongo_cat_facetrack_abi(void);
/* 加载模型并预热。models_dir 下需要 mnv3_detection_opt.onnx 与 lm_model0_opt.onnx。
   返回 0 = 成功; 失败时把原因写进 err。 */
BONGO_CAT_FACETRACK_EXPORT int bongo_cat_facetrack_open(const char* models_dir,
    char* err, size_t err_cap);
/* 同步跑一帧: 1 = 有新结果, 0 = 没检测到脸, <0 = 错误。stride 是每行字节数。 */
BONGO_CAT_FACETRACK_EXPORT int bongo_cat_facetrack_feed(const unsigned char* rgb,
    int width, int height, int stride, BongoCatFaceFrame* out);
BONGO_CAT_FACETRACK_EXPORT void bongo_cat_facetrack_close(void);

#ifdef __cplusplus
}
#endif

#endif
