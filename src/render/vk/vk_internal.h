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
    bool requested;      /* 这一帧要录制拷贝 */
    bool pending;        /* 已经录制, 等下一次复用该帧槽时读取 */
    int origin_x;
    int origin_y;
    int width;
    int height;
} BongoCatVkReadback;

struct BongoCatVkTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor;
    /* 外部拥有的图像: 只借用, 释放时不动 image/memory/view */
    bool external;
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

/* 设置界面: 逻辑像素 -> 裁剪空间的缩放与偏移 */
typedef struct UiPush {
    float scale[2];
    float offset[2];
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

    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
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
