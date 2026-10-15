#ifndef BONGO_CAT_VK_BACKEND_H
#define BONGO_CAT_VK_BACKEND_H

/*
 * 这个头文件同时被 C 和 C++ 使用 (C++ 侧是 Cubism 的 Vulkan 桥), 所以
 * 必须显式给出 C 链接: 否则 C++ 调用方按名字修饰去找符号, 链接期会报
 * "无法解析的外部符号 bongo_cat_vk_native_info" 这类错误。
 */
#ifdef __cplusplus
extern "C" {
#endif

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
/*
 * 在已有上下文 (主窗口) 的 VkInstance/VkDevice/queue 上再建一个上下文, 用于第二个
 * 窗口 (设置窗口): 交换链、命令池、描述符池、管线都是自己的一份, 但设备与队列是
 * 借来的, 销毁时不会动它们。
 *
 * 一个进程只保留一套 VkDevice 是刻意的: 注入式图形钩子 (RTSS / OBS /
 * NVIDIA 叠加层) 普遍假定单设备, 第二套设备会让它们在呈现路径上越界 (实测
 * nvoglv64 内部崩在 vkQueuePresentKHR)。
 * owner 为 NULL 时等价于 bongo_cat_vk_create()。owner 必须比这个上下文活得久。
 */
BongoCatVk *bongo_cat_vk_create_shared(SDL_Window *window, const BongoCatVk *owner,
    BongoCatError *error);
void bongo_cat_vk_destroy(BongoCatVk *vk);

bool bongo_cat_vk_info(const BongoCatVk *vk, BongoCatVkInfo *info);

/*
 * 连续呈现失败的次数 (成功一次即清零) 与累计失败次数。
 *
 * 上层用它做防御性回退: Vulkan 的初始化成功并不代表这一帧真的能端出去 —— 交换链
 * 建好了却每次 vkQueuePresentKHR 都失败时, 窗口会一直停在空白画布上。设置窗口与
 * 轮盘据此在连续 N 次失败后改用 OpenGL 重建自己, 避免把用户困在一个画不出来的
 * 界面里 (见 preferences_gl.c / dial_window.c)。
 */
uint32_t bongo_cat_vk_consecutive_present_failures(const BongoCatVk *vk);
uint64_t bongo_cat_vk_present_failures(const BongoCatVk *vk);
/* 已经成功提交并呈现出去的帧数 (用于判断"画了却从没提交") */
uint64_t bongo_cat_vk_presented_frames(const BongoCatVk *vk);

/* 交换链尺寸变化, 由 WINDOW_RESIZED/PIXEL_SIZE_CHANGED 之后调用 */
bool bongo_cat_vk_resize(BongoCatVk *vk, int width, int height);

/*
 * 垂直同步开关 (对应界面里的最大帧率设置: "无限" -> false)。
 * 呈现模式是创建交换链时定下的, 值变化时这里会重建交换链;
 * 页面尺寸没变也会重建, 所以不要用 bongo_cat_vk_resize 代替。
 */
void bongo_cat_vk_set_vsync(BongoCatVk *vk, bool vsync);

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
/* 单通道 (R8) 纹理: 设置界面的字体图集与图标缓存用, 着色器把 r 当 alpha */
BongoCatVkTexture *bongo_cat_vk_texture_from_alpha(BongoCatVk *vk,
    const unsigned char *alpha, int width, int height, const char *label,
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
 *    premultiply 描述贴图本身的像素格式: true = 直通 alpha (叠加层贴图在 Vulkan
 *    侧就是这样上传的), 片元里会乘一次 alpha; false = 贴图已经是预乘值 (Live2D
 *    的离屏结果), 不能再乘 —— 多乘一次就是半透明区域偏暗的 a^2 bug。
 *  - draw_triangles: 通用顶点色/纹理三角形列表 (三角形扇/带已由调用方展开)。
 */
bool bongo_cat_vk_draw_overlay_quad(BongoCatVk *vk, BongoCatVkTexture *texture,
    bool mirror, int reference_width, int reference_height, bool erase_left,
    bool erase_right, bool premultiply);
bool bongo_cat_vk_draw_triangles(BongoCatVk *vk, BongoCatVkTexture *texture,
    const float *vertices, unsigned int vertex_count, bool textured);
/*
 * 径向菜单的三角形列表。顶点格式与 draw_triangles 不同 (位置 2f + uv 2f +
 * 颜色 RGBA8, 共 20 字节, 与设置界面的界面顶点一致), 而且菜单一帧有数万个顶点,
 * 所以走后端内部单独的大顶点缓冲。顶点坐标是窗口逻辑像素, 原点在左上,
 * 由 width/height 映射到裁剪空间。
 */
bool bongo_cat_vk_draw_dial(BongoCatVk *vk, BongoCatVkTexture *texture,
    const void *vertices, unsigned int vertex_count, int width, int height);

/*
 * 设置视口 (x/y 为左上角像素坐标, y 轴向下)。
 *
 * 画叠加层 (背景图/按键/特效/指针) 时必须用模型的内容视口, 与 OpenGL 路径的
 * glViewport(content_x, content_y, content_w, content_h) 对齐: 那些图元是按
 * NDC 画的整屏四边形, 只有视口一致, 它们才会和叠加层落在同一块内容区域里。
 * 画 Live2D 合成与圆角遮罩之前记得恢复整窗视口。
 */
void bongo_cat_vk_set_viewport(BongoCatVk *vk, int x, int y, int width,
    int height);

/* 圆角遮罩: 与 OpenGL 路径一样把 RGB 与 alpha 一起乘上覆盖率 */
bool bongo_cat_vk_mask_corners(BongoCatVk *vk, int width, int height,
    float radius_percent);

/*
 * 设置界面 (Nuklear) 的绘制支持。
 *  - set_scissor: 像素坐标, 原点在左上 (Vulkan 约定), 超出范围自动裁剪;
 *  - ui_upload_vertices: 一帧开始时把整块顶点 (pos 2f + uv 2f + color RGBA8,
 *    共 20 字节) 放进显存, 索引里引用的是这块顶点的绝对下标;
 *  - ui_draw: 逐条绘制命令只提交索引 (16 位) 与裁剪矩形, scale_x/scale_y 是
 *    2/宽 与 2/高 的投影缩放。
 */
void bongo_cat_vk_set_scissor(BongoCatVk *vk, int x, int y, int width, int height);
bool bongo_cat_vk_ui_upload_vertices(BongoCatVk *vk, const void *vertices,
    unsigned int vertex_count);
bool bongo_cat_vk_ui_draw(BongoCatVk *vk, BongoCatVkTexture *texture,
    const void *indices, unsigned int index_count, float scale_x, float scale_y);
/* 设备允许的最大 2D 图像边长 (界面用它判断字体图集是否过大) */
int bongo_cat_vk_max_texture_dimension(const BongoCatVk *vk);

/*
 * 把 Vulkan 的原始句柄交给别的渲染器 (目前是 Cubism 的 Live2D 渲染器)。
 * 字段都是不透明的 void* / 整数: 这个头文件是 C 的, 调用方 (C++) 自行转成
 * VkDevice / VkPhysicalDevice / VkCommandPool / VkQueue / VkFormat 等。
 * 返回 false 表示后端没就绪, 调用方应当回退 OpenGL。
 */
typedef struct BongoCatVkNativeInfo {
    void *instance;
    void *device;
    void *physical_device;
    void *command_pool;
    void *queue;
    /* 队列族与深度格式: 组件要自己建渲染目标/管线, 光有 queue 句柄不够。 */
    unsigned int queue_family;
    int depth_format;   /* VkFormat */
    /* 动态解析入口: 组件不链接 volk/loader 的表, 需要新函数时从这里取。
       两个函数指针由宿主填(它们是 loader 提供的、进程级的), 组件只调用。 */
    void *(*get_instance_proc)(const char *name);
    void *(*get_device_proc)(const char *name);
    /* 交换链第 0 张图像的视图: SDK 的 SetConstantSettings 需要一个默认目标 */
    void *image_view;
    int width;
    int height;
    int image_format;   /* VkFormat */
    int image_count;    /* 交换链图像数 */
} BongoCatVkNativeInfo;

bool bongo_cat_vk_native_info(const BongoCatVk *vk, BongoCatVkNativeInfo *info);

/* 包装外部拥有的 VkImage (只借 view, 不接管所有权) */
BongoCatVkTexture *bongo_cat_vk_texture_from_external(BongoCatVk *vk,
    void *image, void *view, int width, int height, const char *label,
    BongoCatError *error);

/*
 * 命中测试回读。请求发生在帧内 (end_frame 之前), 拷贝与主机映射在下一次使用
 * 同一个帧槽时完成, 所以结果有一帧延迟 —— 与 OpenGL 路径读前/后台缓冲的时序
 * 相当, 远快于命中测试的调度间隔 (100 ms)。
 */
bool bongo_cat_vk_queue_hit_sample(BongoCatVk *vk, int x, int y);
/* 整帧 RGBA 回读 (封面 / 诊断 / 审计): 一帧延迟, take 一次性取走 */
bool bongo_cat_vk_queue_frame_sample(BongoCatVk *vk, int x, int y, int w, int h);
bool bongo_cat_vk_take_frame_sample(BongoCatVk *vk, int *w, int *h,
    const unsigned char **pixels);
/* 内存清理时释放可再生的回读缓冲 (下次用到会重新分配) */
void bongo_cat_vk_release_cached_frames(BongoCatVk *vk);
/* 查询已缓存的回读结果; 目标点不在缓存块内时返回 false */
bool bongo_cat_vk_hit_alpha(const BongoCatVk *vk, int x, int y,
    unsigned char *alpha);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
