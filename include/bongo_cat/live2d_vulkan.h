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

#include "bongo_cat/vk_backend.h"

/* 宿主私有的后端结构体只做前向声明: 阶段 2d 起它不再出现在任何跨界签名里, 只在宿主内部
   被抽成 BongoCatVkNativeInfo(见 vk_backend.h)后才交给组件。 */
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
 * remember() 必须在模型加载之前调用, 而且它是唯一允许在 CubismFramework::StartUp
 * 之前调用的入口 —— 它只记值, 不碰任何 SDK 对象 (SDK 的容器依赖框架装好的全局分配器)。
 * 之后 attach() 不带参数调用时会复用这里记下的句柄与尺寸。
 */
/*
 * 窗口创建时调用: 只记住后端**快照**与尺寸并打开渲染器工厂开关, 不碰任何 SDK 函数
 * (SDK 的 csmMap 依赖 CubismFramework::StartUp 设置的全局分配器)。
 * 真正的挂载 attach() 推迟到模型创建渲染器时, 那时框架已经启动。
 *
 * 参数是宿主私有 BongoCatVk 的只读快照 —— 阶段 2d 起宿主的那个结构体不再跨界(宿主侧在
 * live2d_dispatch_vulkan.c 里把 vk 抽成快照后才交给组件)。
 */
bool bongo_cat_live2d_vulkan_remember(const BongoCatVkNativeInfo *info, int width,
    int height);

/*
 * **宿主侧**的入口(阶段 2d): 窗口代码拿到的是自己的 BongoCatVk, 由这一层负责把它抽成
 * 只读快照再交给组件 —— 于是"宿主私有结构体不跨界"这条纪律只需要在一个地方守住。
 */
bool bongo_cat_live2d_vulkan_remember_backend(struct BongoCatVk *vk, int width,
    int height);
/*
 * Cubism 的 Vulkan 渲染器按相对路径 "FrameworkShaders/*.spv" 打开着色器, 所以
 * 进程的工作目录必须是这些文件的父目录。发布包把它们放在 exe 旁边, 但"只复制
 * 一个 exe"的情况很常见 (用户把 exe 拷到桌面就跑), 那时要去解出来的内嵌资源
 * 目录里找。set_shader_root 在窗口创建时把内嵌资源根目录告诉桥。
 */
bool bongo_cat_live2d_vulkan_shaders_available(const char *assets_root);
void bongo_cat_live2d_vulkan_set_shader_root(const char *assets_root);
/*
 * 档位只决定"下次重建离屏目标时用几倍": 目标已经建好时改档不会立刻生效, 要等窗口尺寸
 * 变化或 release_target()。传入 0/1/2 以外的值一律按 0 (自动) 处理。
 */
/* 超采样档位 ([性能] 区): 0=自动 (目标面积大时退回 1x) 1=质量 (2x) 2=省内存 (1x) */
void bongo_cat_live2d_vulkan_set_supersample(int mode);
/* 内存清理: 释放离屏目标 (下次绘制自动重建) */
void bongo_cat_live2d_vulkan_release_target(void);
/*
 * 借用令牌: 每次重建/释放离屏目标都自增。宿主缓存了 target_info 给的 image/view 时**必须
 * 一起记住它**, 变了就立刻丢弃并重新获取 —— 同尺寸重建时尺寸比较发现不了句柄已换, 继续用
 * 旧包装会采样失效句柄(实测崩在封面回读)。
 */
uint64_t bongo_cat_live2d_vulkan_target_generation(void);
/*
 * 接管 Vulkan 渲染。参数是**宿主私有 BongoCatVk 的只读快照**(POD, 见 vk_backend.h), 而不是
 * 那个结构体本身 —— 组件不该也不能解释宿主的后端状态; 快照里有 device/物理设备/队列/队列族/
 * 命令池/格式/尺寸以及 loader 的动态入口。传 NULL 表示"用 remember() 记下的那一份"。
 * 返回 false 表示这次用不了 VK(宿主会沿用既有回退路径)。
 */
bool bongo_cat_live2d_vulkan_attach(const BongoCatVkNativeInfo *info, int width,
    int height);
bool bongo_cat_live2d_vulkan_ready(void);
bool bongo_cat_live2d_vulkan_begin_frame(void);
bool bongo_cat_live2d_vulkan_end_frame(void);
void bongo_cat_live2d_vulkan_detach(void);

/* 模型贴图: 返回 SDK 的 CubismImageVulkan 句柄 (void*), 失败返回 NULL。
   设备来自 attach() 记下的快照 —— 所以这里不再需要 BongoCatVk*。 */
void *bongo_cat_live2d_vulkan_create_texture(const void *pixels, int width,
    int height);
void bongo_cat_live2d_vulkan_destroy_texture(void *texture);

/* 同上, 但用 attach() 时记住的后端 (模型侧不用持有 BongoCatVk*) */
void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels,
    int width, int height);

/*
 * 图集上限的设备侧夹层: 把"设置档位给的像素上限"夹到设备的 maxImageDimension2D。
 * 单边超过设备上限的纹理创建必然失败, 那时模型加载会整个失败 —— 与其如此, 不如提前夹到
 * 设备允许的最大边长并记一条 WARN。设备还没 attach (或没有物理设备句柄) 时原样返回。
 */
int bongo_cat_live2d_vulkan_clamp_texture_limit(int requested);

/* 把贴图绑到渲染器上 (两个参数都是不透明句柄) */
bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture);

/*
 * 返回的宽高是**超采样之后**的缓冲尺寸 (通常 2 倍窗口), 不是窗口像素尺寸: 合成方按它
 * 建纹理包装、在采样时线性缩小, 这就是 Vulkan 侧替代 MSAA 的做法。尺寸变化即表示离屏
 * 目标被重建过, 合成方应当据此重建自己的纹理包装 (见 vk_scene.c)。
 */
/* 离屏结果 (纹理与尺寸), 供合成到宠物窗口时使用 */
bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
