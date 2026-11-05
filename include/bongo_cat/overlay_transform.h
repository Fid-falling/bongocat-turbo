#ifndef BONGO_CAT_OVERLAY_TRANSFORM_H
#define BONGO_CAT_OVERLAY_TRANSFORM_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 把"叠加层(气泡 + 手/按键)整体旋转 + 缩放"的变换算成 4x4 矩阵, 供 GL / Vulkan 两条
 * 后端的顶点着色器直接乘。degrees 与模型旋转一致, inverse_scale 传 1/(|cos|+|sin|),
 * width/height 是当前视口像素尺寸 (旋转在像素空间做, 非方形窗口才不会拉斜)。
 *
 * 没有旋转且 inverse_scale=1 时输出单位矩阵 —— 两条后端的行为与旧版逐像素一致。
 */
void bongo_cat_overlay_rotation_matrix(float degrees, float inverse_scale,
    int width, int height, float out[16]);

#ifdef __cplusplus
}
#endif

#endif
