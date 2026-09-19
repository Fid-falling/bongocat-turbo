#ifndef BONGO_CAT_LIVE2D_VULKAN_H
#define BONGO_CAT_LIVE2D_VULKAN_H

#include <stdbool.h>

struct BongoCatVk;

/*
 * Cubism 的 Live2D 渲染器走 Vulkan 后端时的桥。
 *
 * SDK 自带 CubismRenderer_Vulkan, 它需要应用提供 device/queue/command pool 与一个
 * 绘制目标 (见 SDK 的 Samples/Vulkan/Demo):
 *   SetConstantSettings(...) -> EnableChangeRenderTarget() -> SetRenderTarget(...)
 * 这里把模型画进一张离屏纹理, 由 Vulkan 后端的合成路径再画到宠物窗口。
 *
 * 所有函数在下面这些情况下都必须能安全失败 (返回 false), 让上层回退 OpenGL:
 * 没有编译 Vulkan 后端、没有编译 Cubism、设备没就绪。
 */
bool bongo_cat_live2d_vulkan_attach(struct BongoCatVk *vk, int width, int height);
bool bongo_cat_live2d_vulkan_ready(void);
bool bongo_cat_live2d_vulkan_begin_frame(void);
bool bongo_cat_live2d_vulkan_end_frame(void);
void bongo_cat_live2d_vulkan_detach(void);

/* 离屏结果 (纹理与尺寸), 供合成到宠物窗口时使用 */
bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height);

#endif
