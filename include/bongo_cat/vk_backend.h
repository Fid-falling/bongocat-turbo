#ifndef BONGO_CAT_VK_BACKEND_H
#define BONGO_CAT_VK_BACKEND_H

#include "bongo_cat/common.h"

/*
 * Vulkan 渲染后端 (阶段 2 起)。
 *
 * 这一层只负责"把窗口画出来"所需的最小 Device/Swapchain/帧同步, 以及后端
 * 自己的纹理与 2D 绘制。它与 OpenGL 后端并存: 由 bongo_cat_renderer_active()
 * 决定这次运行用哪条路, 两条路互不共享 GPU 资源。
 *
 * 设计约束:
 *  - 交换链格式优先选 UNORM, 与 OpenGL 路径的线性帧缓冲保持一致, 避免
 *    透明度合成时出现 gamma 偏移。
 *  - 同步采用 frames-in-flight + 栅栏, 不使用 vkDeviceWaitIdle 逐帧等待。
 *  - 任何一步失败都返回 false 并写入 BongoCatError, 调用方负责回退 OpenGL。
 */

typedef struct SDL_Window SDL_Window;
typedef struct BongoCatVk BongoCatVk;
typedef struct BongoCatVkTexture BongoCatVkTexture;

typedef struct BongoCatVkInfo {
    char device[128];
    char driver[128];
    char api[32];
    int width;
    int height;
} BongoCatVkInfo;

/* 这次构建是否带 Vulkan 支持 (BONGO_CAT_HAS_VULKAN) */
bool bongo_cat_vk_compiled(void);

/* 运行时探测: 能否创建实例并找到可用的图形队列。不会创建窗口表面。 */
bool bongo_cat_vk_probe(BongoCatError *error);

/* 当前进程里活跃的 Vulkan 上下文 (没有则为 NULL)。窗口只有一个, 纹理与绘制
   都发生在主窗口的上下文里, 所以这里沿用与 renderer_active 一致的单例语义。 */
BongoCatVk *bongo_cat_vk_current(void);

/* window 必须带 SDL_WINDOW_VULKAN 标志创建 */
BongoCatVk *bongo_cat_vk_create(SDL_Window *window, BongoCatError *error);
void bongo_cat_vk_destroy(BongoCatVk *vk);

bool bongo_cat_vk_info(const BongoCatVk *vk, BongoCatVkInfo *info);

/* 交换链尺寸变化, 由 WINDOW_RESIZED/PIXEL_SIZE_CHANGED 之后调用 */
bool bongo_cat_vk_resize(BongoCatVk *vk, int width, int height);

/*
 * 帧的生命周期: begin 会获取交换链图像并开始渲染通道 (使用 clear 颜色清屏),
 * end 结束渲染通道 / 提交 / 呈现。begin 与 end 之间才可以录制绘制命令。
 */
bool bongo_cat_vk_begin_frame(BongoCatVk *vk, int width, int height);
bool bongo_cat_vk_end_frame(BongoCatVk *vk);
bool bongo_cat_vk_frame_active(const BongoCatVk *vk);
void bongo_cat_vk_set_clear_color(BongoCatVk *vk, float red, float green,
    float blue, float alpha);

/*
 * 纹理。像素格式固定 RGBA8 (与 OpenGL 后端的 GL_RGBA8 一致), 采样用线性过滤 +
 * clamp to edge, 与 src/media/image.c 里普通图片纹理的参数相同。
 */
BongoCatVkTexture *bongo_cat_vk_texture_from_pixels(BongoCatVk *vk,
    const unsigned char *rgba, int width, int height, const char *label,
    BongoCatError *error);
BongoCatVkTexture *bongo_cat_vk_texture_load(BongoCatVk *vk, const char *path,
    int *width, int *height, BongoCatError *error);
void bongo_cat_vk_texture_destroy(BongoCatVk *vk, BongoCatVkTexture *texture);
int bongo_cat_vk_texture_width(const BongoCatVkTexture *texture);
int bongo_cat_vk_texture_height(const BongoCatVkTexture *texture);

/*
 * 绘制。两者都必须在 begin/end frame 之间调用, 并且都把顶点写进这一帧的
 * 主机可见顶点缓冲 (每帧自动复位)。
 *  - draw_overlay_quad: 与 OpenGL 后端的叠加层四边形着色器语义一致
 *    (镜像、参考尺寸缩放、擦除脚印圆、预乘 alpha)。
 *  - draw_triangles: 通用顶点色/纹理三角形列表 (三角形扇/带已由调用方展开)。
 */
bool bongo_cat_vk_draw_overlay_quad(BongoCatVk *vk, BongoCatVkTexture *texture,
    bool mirror, int reference_width, int reference_height, bool erase_left,
    bool erase_right);
bool bongo_cat_vk_draw_triangles(BongoCatVk *vk, BongoCatVkTexture *texture,
    const float *vertices, unsigned int vertex_count, bool textured);

/* 圆角遮罩: 与 OpenGL 路径一样把 RGB 与 alpha 一起乘上覆盖率 */
bool bongo_cat_vk_mask_corners(BongoCatVk *vk, int width, int height,
    float radius_percent);

/*
 * 命中测试回读。请求发生在帧内 (end_frame 之前), 拷贝与主机映射在下一次使用
 * 同一个帧槽时完成, 所以结果有一帧延迟 —— 与 OpenGL 路径读前/后台缓冲的时序
 * 相当, 远快于命中测试的调度间隔 (100 ms)。
 */
bool bongo_cat_vk_queue_hit_sample(BongoCatVk *vk, int x, int y);
/* 查询已缓存的回读结果; 目标点不在缓存块内时返回 false */
bool bongo_cat_vk_hit_alpha(const BongoCatVk *vk, int x, int y,
    unsigned char *alpha);

#endif
