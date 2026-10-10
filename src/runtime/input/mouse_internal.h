/*
 * input 模块内部的鼠标投影契约, 只给 mouse_bounds.c / mouse_mapping.c / mouse_pipeline.c
 * 三个文件共用 (不属于公开 API, 外部模块不要 include)。
 * 一次指针更新的顺序是固定的: projection -> map_pointer -> apply_mouse_coordinates,
 * 三个函数都不接受 NULL 的投影; 每帧都要重新解析一次投影, 不要跨帧缓存。
 */
#ifndef BONGO_CAT_MOUSE_INTERNAL_H
#define BONGO_CAT_MOUSE_INTERNAL_H

#include "bongo_cat/app.h"

/* 传给下面三个函数的坐标基准。center_x/center_y 是模型锚点的屏幕坐标, 只在 centered 为真时
   有效 (gaze 比例以它为 0.5 的中点)。 */
/* One update uses the same coordinate space for motion limits and model output.
   Resolve it from current display geometry; do not cache across window moves. */
typedef struct BongoCatMouseProjection {
    BongoCatMverPointerBounds bounds;
    double center_x, center_y;
    bool centered;
} BongoCatMouseProjection;

/* 解析当帧基准 (实现在 mouse_bounds.c): 成功返回 true 并填满 projection, 失败表示坐标
   不可信, 调用方应沿用上一帧位置而不是归零。 */
bool bongo_cat_app_mouse_projection(BongoCatApp *app, double screen_x,
    double screen_y, BongoCatMouseProjection *projection);
/* 屏幕坐标 (或相对位移) -> 模型坐标 (实现在 mouse_mapping.c): 越界会被钳进 bounds,
   结果既用于 mver 手臂几何也用于鼠标参数。*changed 表示逻辑位置是否真的变了。 */
bool bongo_cat_app_map_pointer(BongoCatApp *app,
    const BongoCatMouseProjection *projection, bool relative_requested,
    double absolute_x, double absolute_y, double *x, double *y, bool *changed);
/* 把坐标写进模型与覆盖层 (实现在 mouse_mapping.c): 参数不存在会被静默跳过, 总会置脏。 */

void bongo_cat_app_apply_mouse_coordinates(BongoCatApp *app,
    const BongoCatMouseProjection *projection, double hand_x, double hand_y,
    double gaze_x, double gaze_y);

#endif
