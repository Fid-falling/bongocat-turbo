#ifndef BONGO_CAT_LIVE2D_VULKAN_INTERNAL_H
#define BONGO_CAT_LIVE2D_VULKAN_INTERNAL_H

#include "bongo_cat/vk_backend.h"

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
