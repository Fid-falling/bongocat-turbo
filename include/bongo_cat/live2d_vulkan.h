#ifndef BONGO_CAT_LIVE2D_VULKAN_H
#define BONGO_CAT_LIVE2D_VULKAN_H

/*
 * 这个头文件由 C (运行时窗口代码) 与 C++ (桥的实现) 共用, 必须显式给出
 * C 链接, 否则 C 侧按未修饰名调用、C++ 侧按修饰名定义, 链接期对不上。
 */
#ifdef __cplusplus
extern "C" {
#endif

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
/*
 * 窗口创建时调用: 只记住后端指针与尺寸并打开渲染器工厂开关, 不碰任何 SDK 函数
 * (SDK 的 csmMap 依赖 CubismFramework::StartUp 设置的全局分配器)。
 * 真正的挂载 attach() 推迟到模型创建渲染器时, 那时框架已经启动。
 */
bool bongo_cat_live2d_vulkan_remember(struct BongoCatVk *vk, int width, int height);
/*
 * Cubism 的 Vulkan 渲染器按相对路径 "FrameworkShaders/*.spv" 打开着色器, 所以
 * 进程的工作目录必须是这些文件的父目录。发布包把它们放在 exe 旁边, 但"只复制
 * 一个 exe"的情况很常见 (用户把 exe 拷到桌面就跑), 那时要去解出来的内嵌资源
 * 目录里找。set_shader_root 在窗口创建时把内嵌资源根目录告诉桥。
 */
bool bongo_cat_live2d_vulkan_shaders_available(const char *assets_root);
void bongo_cat_live2d_vulkan_set_shader_root(const char *assets_root);
/* 超采样档位 ([性能] 区): 0=自动 (目标面积大时退回 1x) 1=质量 (2x) 2=省内存 (1x) */
void bongo_cat_live2d_vulkan_set_supersample(int mode);
bool bongo_cat_live2d_vulkan_attach(struct BongoCatVk *vk, int width, int height);
bool bongo_cat_live2d_vulkan_ready(void);
bool bongo_cat_live2d_vulkan_begin_frame(void);
bool bongo_cat_live2d_vulkan_end_frame(void);
void bongo_cat_live2d_vulkan_detach(void);

/* 模型贴图: 返回 SDK 的 CubismImageVulkan 句柄 (void*), 失败返回 NULL */
void *bongo_cat_live2d_vulkan_create_texture(struct BongoCatVk *vk,
    const void *pixels, int width, int height);
void bongo_cat_live2d_vulkan_destroy_texture(void *texture);

/* 同上, 但用 attach() 时记住的后端 (模型侧不用持有 BongoCatVk*) */
void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels,
    int width, int height);

/* 把贴图绑到渲染器上 (两个参数都是不透明句柄) */
bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture);

/* 离屏结果 (纹理与尺寸), 供合成到宠物窗口时使用 */
bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
