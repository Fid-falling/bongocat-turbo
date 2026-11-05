/*
 * 叠加层(气泡 + 手/按键)的旋转矩阵。
 *
 * 模型本体是在 Live2D 的 MVP 里旋转的 (见 cubism_model_update.cpp 的 build_projection),
 * 而气泡与手是宿主自己画的四边形, 走另一条管线 —— 所以这两个地方必须用**同一个**变换,
 * 否则会出现"模型转了、气泡还立着"(用户报过)。
 *
 * 变换在像素空间里定义, 避免非方形窗口把旋转拉斜:
 *   p_px = p_ndc * (w/2, h/2)   ->   R(θ) * inv_scale  ->   p_ndc = p_px / (w/2, h/2)
 * 其中 inv_scale = 1/(|cos|+|sin|): 旋转时窗口按这个系数放大, 模型投影按它的倒数缩小,
 * 叠加层跟着缩, 三者才对得齐。
 *
 * 输出是行主序的 4x4 (与 glUniformMatrix4fv(transpose=GL_FALSE) 的列主序数组不同 ——
 * 这里直接按 OpenGL 使用的列主序填, 见 bongo_cat_overlay_rotation_matrix 的注释)。
 */
#include "bongo_cat/overlay_transform.h"

#include <math.h>

void bongo_cat_overlay_rotation_matrix(float degrees, float inverse_scale,
    int width, int height, float out[16]) {
    if (!out) return;
    /* 单位矩阵: 没有旋转时叠加层与旧版逐像素一致 */
    for (int i = 0; i < 16; ++i) out[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    if (width <= 0 || height <= 0) return;
    if (fabsf(degrees) < 0.01f && fabsf(inverse_scale - 1.0f) < 0.0005f) return;
    const float rad = degrees * 3.14159265358979f / 180.0f;
    const float c = cosf(rad), s = sinf(rad);
    const float k = inverse_scale > 0.0f && inverse_scale < 1000.0f
        ? inverse_scale : 1.0f;
    const float sx = 2.0f / (float)width;   /* 像素 -> NDC */
    const float sy = 2.0f / (float)height;
    /* 列主序: 第 j 列是基向量 j 的像。变换 = diag(sx,sy) * R * k * diag(w/2,h/2)。
       逐列算: (m00,m10) 与 (m01,m11) 见下。 */
    const float half_w = (float)width * 0.5f;
    const float half_h = (float)height * 0.5f;
    out[0] = sx * c * k * half_w;
    out[1] = sy * s * k * half_w;
    out[4] = sx * -s * k * half_h;
    out[5] = sy * c * k * half_h;
}
