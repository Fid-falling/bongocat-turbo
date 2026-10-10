#ifndef BONGO_CAT_LIVE2D_VULKAN_INTERNAL_H
#define BONGO_CAT_LIVE2D_VULKAN_INTERNAL_H

#include "bongo_cat/vk_backend.h"

/*
 * Live2D 的 Vulkan 离屏目标超采样倍数。
 *
 * SDK 的 Vulkan 渲染器把采样数写死为 VK_SAMPLE_COUNT_1_BIT (开不了 MSAA), 而
 * OpenGL 路径画在 MSAA 默认帧缓冲里。不超采样时模型的描边/轮廓会有明显锯齿,
 * 看起来就是"锐化过头"。2 倍分辨率渲染 + 合成时线性缩小 = 2x2 超采样。
 */
#define BONGO_CAT_LIVE2D_VULKAN_SUPERSAMPLE 2

/*
 * 运行时超采样档位 (设置界面 [性能] 区), 以及按目标尺寸算出的实际倍数:
 *   0 = 自动 (面积超过阈值退回 1x) 1 = 质量 (2x) 2 = 省内存 (1x)
 */
extern int bongo_cat_live2d_vulkan_supersample_mode;
int bongo_cat_live2d_vulkan_supersample_for(int width, int height);

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 把 SDK 渲染器画完的离屏图像从 COLOR_ATTACHMENT_OPTIMAL 转到
 * SHADER_READ_ONLY_OPTIMAL —— 合成着色器采样之前必须做。
 * 用后端自己的命令池提交一次, 与贴图上传同样的做法。
 * 参数用 void* 而不是 VkImage/VkDevice, 这样不带 Vulkan 的构建也能编译。
 */
bool bongo_cat_live2d_vulkan_transition_sampled(const BongoCatVkNativeInfo *info,
    void *image);

#ifdef __cplusplus
}
#endif

#endif
