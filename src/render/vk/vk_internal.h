#ifndef BONGO_CAT_VK_INTERNAL_H
#define BONGO_CAT_VK_INTERNAL_H

#include "bongo_cat/vk_backend.h"

#ifdef BONGO_CAT_HAS_VULKAN

#include <vulkan/vulkan.h>

/* 同时最多两帧在飞: 够用且把延迟压在 1 帧以内 */
#define BONGO_CAT_VK_FRAMES_IN_FLIGHT 2
#define BONGO_CAT_VK_MAX_SWAP_IMAGES 8

typedef struct BongoCatVkFrame {
    VkCommandBuffer command;
    VkSemaphore image_available;
    VkFence fence;
} BongoCatVkFrame;

/* 每帧一份的主机可见顶点缓冲: 文本/图元在 begin/end frame 之间顺序追加。 */
typedef struct BongoCatVkArena {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char *mapped;
    size_t capacity;
    size_t offset;
} BongoCatVkArena;

/* 命中测试回读: 指针附近的一小块 alpha 从交换链图像拷回主机 */
#define BONGO_CAT_VK_HIT_TILE 64

/*
 * 已经交给 GPU 的贴图不能立刻销毁: 可能还有命令缓冲在引用它。销毁请求先记在
 * 这里, 等对应的帧栅栏过去之后再真正释放。
 */
#define BONGO_CAT_VK_RETIRE_CAP 32

typedef struct BongoCatVkRetired {
    BongoCatVkTexture *texture;
    uint64_t frame;
} BongoCatVkRetired;

typedef struct BongoCatVkReadback {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char *mapped;
    VkDeviceSize capacity;  /* buffer 字节数; 整帧回读按 w*h*4 扩容 */
    bool requested;      /* 这一帧要录制拷贝 */
    bool pending;        /* 已经录制, 等下一次复用该帧槽时读取 */
    bool whole_frame;    /* 回读整帧 RGBA, 而不是命中点附近的 alpha 小块 */
    int origin_x;
    int origin_y;
    int width;
    int height;
} BongoCatVkReadback;

/* vk_readback.c: 回读缓冲与采集 (对外 API 见 bongo_cat/vk_backend.h) */
bool bongo_cat_vk_create_readbacks(BongoCatVk *vk, BongoCatError *error);
void bongo_cat_vk_destroy_readbacks(BongoCatVk *vk);
void bongo_cat_vk_collect_readback(BongoCatVk *vk);

struct BongoCatVkTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor;
    /* 外部拥有的图像: 只借用, 释放时不动 image/memory/view */
    bool external;
    /*
     * 单通道遮罩纹理 (字体图集、设置界面的 paint cache): 界面着色器对它取 R 通道
     * 当覆盖率、RGB 当 1。普通 RGBA 图 (图标图集 ui-symbols.png / logo) 整体采样,
     * 形状在 alpha 里。以前不区分, 于是"白色 RGB + alpha 遮罩"的图标图集被整块
     * 涂成纯色方块。
     */
    bool alpha_mask;
    int width;
    int height;
    char label[64];
};

/* 推送常量布局, 必须与 src/render/vk/shaders/*.vert|frag 的 std430 声明一致 */
typedef struct OverlayPush {
    float reference[2];
    float image_size[2];
    int mirror;
    int erase_left;
    int erase_right;
} OverlayPush;

typedef struct PointerPush {
    int textured;
} PointerPush;

typedef struct CornerPush {
    int width;
    int height;
    int radius_milli;
} CornerPush;

/* 设置界面: 逻辑像素 -> 裁剪空间的缩放与偏移, 以及纹理的取样方式 */
typedef struct UiPush {
    float scale[2];
    float offset[2];
    /* 1 = 普通 RGBA 纹理 (整体采样, 形状在 alpha); 0 = 单通道遮罩 (R 当覆盖率) */
    int color_texture;
} UiPush;

/* 混合模式, 与 OpenGL 路径逐条对应 */
typedef enum BongoCatVkBlend {
    BONGO_CAT_VK_BLEND_PREMULTIPLIED,  /* ONE, ONE_MINUS_SRC_ALPHA */
    BONGO_CAT_VK_BLEND_MULTIPLY,       /* ZERO, SRC_ALPHA (圆角遮罩) */
    BONGO_CAT_VK_BLEND_ALPHA           /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA (界面) */
} BongoCatVkBlend;

struct BongoCatVk {
    SDL_Window *window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkCommandPool command_pool;
    bool validation;
    /*
     * 这个上下文是否自己拥有 instance/device (以及 queue/物理设备)。
     *
     * 第二个窗口 (设置窗口) 通过 bongo_cat_vk_create_shared() 复用主窗口的
     * instance/device/queue, 只建自己的交换链与设备级资源 (命令池/描述符池/
     * 管线都是一次性对象, 同一个设备上再来一套没问题); 销毁时不能去销毁借来的
     * instance/device。一个进程只保留一套 VkDevice 也让注入式图形钩子
     * (RTSS / OBS / NVIDIA 叠加层) 的单设备假设成立。
     */
    bool owns_device;

    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
    /*
     * 逐像素合成: 窗口透明时必须是 PRE_MULTIPLIED, 否则 DWM 把清屏 alpha 当成
     * 不透明, 桌面宠物的背景就是黑的 (见 vk_swapchain.c 的 choose_composite_alpha)。
     * 两个字段只用于日志与诊断, 便于测试断言 "透明窗口真的拿到了 alpha 合成"。
     */
    VkCompositeAlphaFlagBitsKHR composite_alpha;
    VkCompositeAlphaFlagsKHR composite_supported;
    /* 垂直同步开关 (由最大帧率设置决定: "无限" -> false) */
    bool vsync;
    VkExtent2D extent;
    bool extent_valid;
    uint32_t image_count;
    VkImage images[BONGO_CAT_VK_MAX_SWAP_IMAGES];
    VkImageView views[BONGO_CAT_VK_MAX_SWAP_IMAGES];
    VkFramebuffer framebuffers[BONGO_CAT_VK_MAX_SWAP_IMAGES];
    /*
     * 呈现完成信号量按交换链图像索引分配, 而不是按帧槽: 一张图像只会在它上一次
     * 呈现完成之后才会被重新获取, 所以这样复用信号量是安全的 (VUID 00067)。
     */
    VkSemaphore present_semaphores[BONGO_CAT_VK_MAX_SWAP_IMAGES];
    VkRenderPass render_pass;

    BongoCatVkFrame frames[BONGO_CAT_VK_FRAMES_IN_FLIGHT];
    uint32_t frame_index;
    uint32_t image_index;
    bool frame_active;
    VkClearColorValue clear;

    VkDescriptorSetLayout texture_layout;
    VkDescriptorPool descriptor_pool;
    VkSampler sampler;
    /* 未贴图的顶点色图元仍须绑定一个描述符集, 这里放一张 1x1 白色纹理 */
    BongoCatVkTexture *fallback_texture;
    VkPipelineLayout overlay_layout;
    VkPipelineLayout pointer_layout;
    VkPipelineLayout corner_layout;
    VkPipelineLayout ui_layout;
    VkPipeline overlay_pipeline;
    VkPipeline pointer_pipeline;
    VkPipeline corner_pipeline;
    VkPipeline ui_pipeline;
    /* 径向菜单: 与设置界面同一套顶点/描述符布局, 但片元是普通 RGBA 采样 */
    VkPipeline dial_pipeline;
    BongoCatVkArena arenas[BONGO_CAT_VK_FRAMES_IN_FLIGHT];
    /* 设置界面的索引缓冲 (16 位), 与顶点一样每帧从零开始追加 */
    BongoCatVkArena index_arenas[BONGO_CAT_VK_FRAMES_IN_FLIGHT];
    /*
     * 径向菜单的顶点缓冲。菜单一帧就有几万个图元顶点 (远超上面 256 KB 的
     * 通用缓冲), 而且顶点格式与应用内其它图元不同, 所以单独一份。
     */
    BongoCatVkArena dial_arenas[BONGO_CAT_VK_FRAMES_IN_FLIGHT];
    BongoCatVkReadback readback[BONGO_CAT_VK_FRAMES_IN_FLIGHT];
    /* 最近一次整帧回读 (RGBA8, 第 0 行在视觉上方), take_frame_sample 一次性取走 */
    unsigned char *frame_pixels;
    int frame_width;
    int frame_height;
    bool frame_ready;
    /* 设置界面这一帧的顶点在顶点缓冲里的起点 (ui_upload_vertices 记录) */
    VkDeviceSize ui_vertex_offset;
    bool ui_vertices_ready;
    /* 最近一次成功回读的 alpha 块 */
    unsigned char hit_alpha[BONGO_CAT_VK_HIT_TILE * BONGO_CAT_VK_HIT_TILE];
    int hit_x;
    int hit_y;
    int hit_width;
    int hit_height;
    bool hit_valid;
    bool transfer_source;   /* 交换链图像能否作为拷贝源 */
    BongoCatVkRetired retired[BONGO_CAT_VK_RETIRE_CAP];
    uint32_t retired_count;
    uint64_t frame_counter;

    BongoCatVkInfo info;
};

/* vk_core.c */
bool bongo_cat_vk_create_instance(BongoCatVk *vk, BongoCatError *error);
bool bongo_cat_vk_create_device(BongoCatVk *vk, BongoCatError *error);
void bongo_cat_vk_destroy_device(BongoCatVk *vk);

/* vk_swapchain.c */
bool bongo_cat_vk_create_swapchain(BongoCatVk *vk, int width, int height,
    BongoCatError *error);
bool bongo_cat_vk_recreate_swapchain(BongoCatVk *vk, int width, int height);
void bongo_cat_vk_destroy_swapchain(BongoCatVk *vk);

/* vk_frame.c */
bool bongo_cat_vk_create_frame_sync(BongoCatVk *vk, BongoCatError *error);
bool bongo_cat_vk_reset_frame_semaphores(BongoCatVk *vk);
void bongo_cat_vk_destroy_frame_sync(BongoCatVk *vk);

/* vk_texture.c */
bool bongo_cat_vk_create_texture_resources(BongoCatVk *vk, BongoCatError *error);
void bongo_cat_vk_destroy_texture_resources(BongoCatVk *vk);
/* 释放那些引用它们的帧栅栏已经过去的贴图 */
void bongo_cat_vk_collect_retired(BongoCatVk *vk);
void bongo_cat_vk_destroy_retired(BongoCatVk *vk);

/* vk_draw.c */
bool bongo_cat_vk_create_draw_resources(BongoCatVk *vk, BongoCatError *error);
void bongo_cat_vk_destroy_draw_resources(BongoCatVk *vk);
/* 交换链重建后, 管线需要跟着新的渲染通道/格式重建 */
void bongo_cat_vk_rebuild_pipelines(BongoCatVk *vk);
void bongo_cat_vk_destroy_pipelines(BongoCatVk *vk);

#endif /* BONGO_CAT_HAS_VULKAN */

#endif
