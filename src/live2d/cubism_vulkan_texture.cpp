/*
 * 模型贴图的 Vulkan 上传。
 *
 * 参照 SDK 的 Samples/Vulkan/Demo/LAppTextureManager.cpp:
 *   暂存缓冲 -> CubismImageVulkan::CreateImage -> SetImageLayout(TRANSFER_DST)
 *   -> vkCmdCopyBufferToImage -> 屏障到 SHADER_READ_ONLY -> CreateView + CreateSampler
 * 另外按示例的做法把像素预乘 (Cubism 的 Vulkan 管线按预乘 alpha 混合)。
 *
 * 返回的句柄是 CubismImageVulkan*, 由调用方 (模型) 持有并在释放时调 destroy。
 */

#include "bongo_cat/live2d_vulkan.h"

#include "bongo_cat/vk_backend.h"

/* 定义在 cubism_vulkan_bridge.cpp: attach() 时记下的宿主快照(阶段 2d 起组件不再持有
   BongoCatVk*)。 */
BongoCatVkNativeInfo bongo_cat_live2d_vulkan_native_snapshot(void);

#ifdef BONGO_CAT_HAS_VULKAN

#include <Rendering/Vulkan/CubismClass_Vulkan.hpp>
#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>

#include <SDL3/SDL.h>
#include <functional>

namespace {

struct VulkanTexture {
    Live2D::Cubism::Framework::CubismImageVulkan image;
};

/*
 * 为什么不用 SDK 的 VulkanManager: 那套命令缓冲属于它的渲染器实例, 生命周期跟着渲染
 * 目标走, 而贴图上传发生在模型加载期、渲染器还没建出来。改用后端自己的池最省事, 也
 * 保证全程只有一个队列族。上传不在帧内, 所以"提交后直接等空闲"是可接受的简化 ——
 * 换成 fence 只会多一套需要跟踪的状态。
 */
/* 我们自己的单次命令缓冲 (SDK 那边是 VulkanManager 的方法, 这里直接用我们的池) */
bool run_once(VkDevice device, VkCommandPool pool, VkQueue queue,
    const std::function<void(VkCommandBuffer)> &record) {
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocate, &command) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
    record(command);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    bool ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
        vkQueueWaitIdle(queue) == VK_SUCCESS;
    vkFreeCommandBuffers(device, pool, 1, &command);
    return ok;
}

} // namespace

void *bongo_cat_live2d_vulkan_create_texture(const void *pixels, int width,
    int height) {
    if (!pixels || width <= 0 || height <= 0) return nullptr;
    /* 设备信息来自 attach() 记下的**宿主快照**(g_native), 组件不持有 BongoCatVk*。 */
    const BongoCatVkNativeInfo info = bongo_cat_live2d_vulkan_native_snapshot();
    if (!info.device || !info.command_pool || !info.queue) return nullptr;
    VkDevice device = (VkDevice)info.device;
    VkPhysicalDevice physical = (VkPhysicalDevice)info.physical_device;
    VkCommandPool pool = (VkCommandPool)info.command_pool;
    VkQueue queue = (VkQueue)info.queue;

    /* 预乘在 CPU 侧一次做完: Cubism 的 Vulkan 管线固定按预乘 alpha 混合, 而 GL 路径
     * 靠渲染器的 IsPremultipliedAlpha(true) 声明"像素已经预乘" (见
     * cubism_model_renderer.cpp 的 bind_textures)。两条路径对像素格式的假设保持一致,
     * 换后端时不需要重新导出贴图。 */
    const size_t bytes = (size_t)width * (size_t)height * 4u;
    unsigned char *premultiplied = (unsigned char *)SDL_malloc(bytes);
    if (!premultiplied) return nullptr;
    const unsigned char *source = (const unsigned char *)pixels;
    for (size_t i = 0; i < bytes; i += 4) {
        const unsigned alpha = source[i + 3];
        premultiplied[i + 0] = (unsigned char)(source[i + 0] * alpha / 255);
        premultiplied[i + 1] = (unsigned char)(source[i + 1] * alpha / 255);
        premultiplied[i + 2] = (unsigned char)(source[i + 2] * alpha / 255);
        premultiplied[i + 3] = (unsigned char)alpha;
    }

    Live2D::Cubism::Framework::CubismBufferVulkan staging;
    staging.CreateBuffer(device, physical, (VkDeviceSize)bytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.Map(device, (VkDeviceSize)bytes);
    staging.MemCpy(premultiplied, (VkDeviceSize)bytes);
    staging.UnMap(device);
    SDL_free(premultiplied);
    if (staging.GetBuffer() == VK_NULL_HANDLE) return nullptr;

    VulkanTexture *texture = new VulkanTexture();
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    texture->image.CreateImage(device, physical, width, height, 1, format,
        VK_IMAGE_TILING_OPTIMAL, usage);
    bool ok = run_once(device, pool, queue, [&](VkCommandBuffer command) {
        texture->image.SetImageLayout(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region = {0};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        vkCmdCopyBufferToImage(command, staging.GetBuffer(),
            texture->image.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        texture->image.SetImageLayout(command, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            1, VK_IMAGE_ASPECT_COLOR_BIT);
    });
    staging.Destroy(device);
    if (!ok) {
        delete texture;
        return nullptr;
    }
    texture->image.CreateView(device, format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    /* 图集 UV 由美术保证落在 [0,1] 内, 所以 CLAMP_TO_EDGE 就够, 不会采样到邻图。
     * mipLevels=1: 这里不建 mip 链, Vulkan 侧的缩小质量靠离屏超采样补 (见
     * live2d_vulkan_internal.h), 和 GL 路径单独给模型贴图建 mip 是两套不同取舍。 */
    texture->image.CreateSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, 1.0f, 1);
    return texture;
}

/* 释放契约: 真正的生命周期保证在 cubism_bridge_load.cpp 的退休队列 —— 被替换的模型
 * 要等 3 帧才 delete, 就是为了让还在飞的那几帧不再引用这些 image/view/sampler。这里
 * 只按 CubismImageVulkan 的析构把资源还掉, 不做额外的设备同步。 */
void bongo_cat_live2d_vulkan_destroy_texture(void *texture) {
    delete (VulkanTexture *)texture;
}

/*
 * 把贴图绑到渲染器上, 两个参数都是不透明句柄: renderer 是
 * CubismRenderer_Vulkan*, texture 是 create_texture_attached() 的返回值。
 * SDK 的 CubismImageVulkan 类型只在这个文件里出现。
 */
bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture) {
    if (!renderer || !texture) return false;
    ((Live2D::Cubism::Framework::Rendering::CubismRenderer_Vulkan *)renderer)
        ->BindTexture(((VulkanTexture *)texture)->image);
    return true;
}

#else /* !BONGO_CAT_HAS_VULKAN */


void *bongo_cat_live2d_vulkan_create_texture(const void *pixels, int width,
    int height) {
    (void)pixels; (void)width; (void)height;
    return nullptr;
}
void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels, int width,
    int height) {
    (void)pixels; (void)width; (void)height;
    return nullptr;
}
void bongo_cat_live2d_vulkan_destroy_texture(void *texture) { (void)texture; }
bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture) {
    (void)renderer; (void)texture;
    return false;
}

#endif
