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
    VkSemaphore render_finished;
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

struct BongoCatVkTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor;
    int width;
    int height;
    char label[64];
};

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
    VkExtent2D extent;
    bool extent_valid;
    uint32_t image_count;
    VkImage images[BONGO_CAT_VK_MAX_SWAP_IMAGES];
    VkImageView views[BONGO_CAT_VK_MAX_SWAP_IMAGES];
    VkFramebuffer framebuffers[BONGO_CAT_VK_MAX_SWAP_IMAGES];
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
    VkPipeline overlay_pipeline;
    VkPipeline pointer_pipeline;
    BongoCatVkArena arenas[BONGO_CAT_VK_FRAMES_IN_FLIGHT];

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
void bongo_cat_vk_destroy_frame_sync(BongoCatVk *vk);

/* vk_texture.c */
bool bongo_cat_vk_create_texture_resources(BongoCatVk *vk, BongoCatError *error);
void bongo_cat_vk_destroy_texture_resources(BongoCatVk *vk);

/* vk_draw.c */
bool bongo_cat_vk_create_draw_resources(BongoCatVk *vk, BongoCatError *error);
void bongo_cat_vk_destroy_draw_resources(BongoCatVk *vk);
/* 交换链重建后, 管线需要跟着新的渲染通道/格式重建 */
void bongo_cat_vk_rebuild_pipelines(BongoCatVk *vk);

#endif /* BONGO_CAT_HAS_VULKAN */

#endif
