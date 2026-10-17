/*
 * Vulkan 侧的"渲染精度": N 倍离屏渲染, 再用 vkCmdBlitImage 线性缩回交换链尺寸。
 *
 * 为什么用 blit 而不是"再画一个全屏四边形": blit 自带线性过滤, 不需要新的管线/
 * 描述符/采样器, 也不会碰叠加层着色器里的 erase/premultiply 语义; 颜色是预乘 alpha,
 * 线性过滤对预乘值本来就是正确的。
 *
 * 为什么所有诊断/命中回读都不用改: 它们读的是**交换链图像**(窗口尺寸), 而这一帧在
 * end_frame 里已经先被 blit 缩回交换链了 —— 于是模型封面、帧诊断、逐像素命中测试
 * 看到的仍然是"窗口尺寸的那一帧", 与 1.17.0 完全一致。
 *
 * 降级策略: 请求倍率超过 GPU 单边上限 (maxImageDimension2D) 或总像素预算, 或交换链
 * 不支持 TRANSFER_DST (blit 目标) 时, 自动退回 1.0 (直接画到交换链) 并记一条 WARN。
 */
#include "vk_internal.h"

#include "bongo_cat/config.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#include <math.h>

#ifdef BONGO_CAT_HAS_VULKAN

/* 离屏目标总像素上限: 4×4K 量级 (~33M 像素), 再往上只是把显存换成看不出的锐度。 */
#define BONGO_CAT_VK_SCALE_MAX_PIXELS (4096ull * 2160ull * 4ull)

/* 设置里请求的倍率 (由 bongo_cat_vk_set_render_scale 写入, 启动与改设置时各一次)。 */
static float requested_scale = BONGO_CAT_RENDER_SCALE_DEFAULT;
/* 上一次已经告警过的降级结论, 避免每帧刷一条。 */
static float warned_scale;

void bongo_cat_vk_set_render_scale(float scale) {
    if (!isfinite(scale)) scale = BONGO_CAT_RENDER_SCALE_DEFAULT;
    if (scale < BONGO_CAT_RENDER_SCALE_MIN) scale = BONGO_CAT_RENDER_SCALE_MIN;
    if (scale > BONGO_CAT_RENDER_SCALE_MAX) scale = BONGO_CAT_RENDER_SCALE_MAX;
    requested_scale = roundf(scale * 10.0f) / 10.0f;
}

static void destroy_target(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->scale_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(vk->device, vk->scale_framebuffer, NULL);
        vk->scale_framebuffer = VK_NULL_HANDLE;
    }
    if (vk->scale_view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, vk->scale_view, NULL);
        vk->scale_view = VK_NULL_HANDLE;
    }
    if (vk->scale_image != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, vk->scale_image, NULL);
        vk->scale_image = VK_NULL_HANDLE;
    }
    if (vk->scale_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, vk->scale_memory, NULL);
        vk->scale_memory = VK_NULL_HANDLE;
    }
    vk->scale_extent.width = vk->scale_extent.height = 0;
    vk->scale_active = false;
}

void bongo_cat_vk_scale_destroy(BongoCatVk *vk) { destroy_target(vk); }

/* 建一张离屏颜色图 + 视图 + framebuffer (帧缓冲复用交换链的 render pass)。 */
static bool create_target(BongoCatVk *vk, VkExtent2D extent) {
    VkImageCreateInfo image = {0};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = vk->format;
    image.extent.width = extent.width;
    image.extent.height = extent.height;
    image.extent.depth = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk->device, &image, NULL, &vk->scale_image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk->device, vk->scale_image, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(vk->device, &allocate, NULL, &vk->scale_memory) !=
        VK_SUCCESS) return false;
    if (vkBindImageMemory(vk->device, vk->scale_image, vk->scale_memory, 0) !=
        VK_SUCCESS) return false;
    VkImageViewCreateInfo view = {0};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = vk->scale_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = vk->format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk->device, &view, NULL, &vk->scale_view) != VK_SUCCESS)
        return false;
    VkFramebufferCreateInfo framebuffer = {0};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = vk->render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &vk->scale_view;
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    if (vkCreateFramebuffer(vk->device, &framebuffer, NULL,
            &vk->scale_framebuffer) != VK_SUCCESS) return false;
    vk->scale_extent = extent;
    return true;
}

/*
 * 把请求倍率收成这台机器能做的倍率 (单边 maxImageDimension2D + 总像素预算)。
 * 降级只告警一次 (按结果值去重), 免得每帧一条。
 */
static float fit_scale(BongoCatVk *vk, int width, int height) {
    float factor = requested_scale;
    if (!isfinite(factor) || fabsf(factor - 1.0f) <= 0.001f) return 1.0f;
    if (factor < BONGO_CAT_RENDER_SCALE_MIN) factor = BONGO_CAT_RENDER_SCALE_MIN;
    if (factor > BONGO_CAT_RENDER_SCALE_MAX) factor = BONGO_CAT_RENDER_SCALE_MAX;
    float maximum = 16384.0f;
    VkPhysicalDeviceProperties properties;
    if (vk->physical != VK_NULL_HANDLE) {
        vkGetPhysicalDeviceProperties(vk->physical, &properties);
        if (properties.limits.maxImageDimension2D > 0)
            maximum = (float)properties.limits.maxImageDimension2D;
    }
    const int longest = width > height ? width : height;
    const float side_limit = (float)maximum / (float)(longest > 0 ? longest : 1);
    if (factor > side_limit) factor = side_limit;
    const double pixels = (double)width * factor * (double)height * factor;
    if (pixels > (double)BONGO_CAT_VK_SCALE_MAX_PIXELS) {
        const double shrink = sqrt((double)BONGO_CAT_VK_SCALE_MAX_PIXELS /
            ((double)width * (double)height));
        if (shrink < (double)factor) factor = (float)shrink;
    }
    if (!(fabsf(factor - requested_scale) <= 0.001f) &&
        !(fabsf(factor - warned_scale) <= 0.001f)) {
        warned_scale = factor;
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 渲染精度 %.2f 超出这台机器的上限, 本帧改用 %.2f "
            "(窗口 %dx%d, maxImageDimension2D=%.0f, 像素上限 %llu)",
            (double)requested_scale, (double)factor, width, height,
            (double)maximum, (unsigned long long)BONGO_CAT_VK_SCALE_MAX_PIXELS);
    }
    return factor;
}

/*
 * 供 begin_frame 调用: 需要离屏时准备好目标并返回它的 extent (通过 out_extent)。
 * 返回 true = 本帧要画到离屏上 (此时 framebuffer = vk->scale_framebuffer);
 * false = 直接画到交换链 (倍率 1.0 / 不支持 / 创建失败), out_extent 原样不动。
 */
bool bongo_cat_vk_scale_prepare(BongoCatVk *vk, int width, int height,
    VkExtent2D *out_extent) {
    if (!vk || !out_extent || width <= 0 || height <= 0) return false;
    vk->scale_active = false;
    /* 交换链不支持 TRANSFER_DST 时 blit 无处落笔: 直接退回 1.0, 只提示一次。 */
    if (!vk->swapchain_transfer_dst) {
        if (!(fabsf(requested_scale - 1.0f) <= 0.001f) &&
            !(fabsf(warned_scale - 1.0f) <= 0.001f)) {
            warned_scale = 1.0f;
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[vulkan] 渲染精度不可用: 交换链不支持 TRANSFER_DST (无法缩回窗口尺寸), "
                "已退回按窗口尺寸直接渲染");
        }
        return false;
    }
    const float factor = fit_scale(vk, width, height);
    const VkExtent2D extent = {(uint32_t)lroundf((float)width * factor),
        (uint32_t)lroundf((float)height * factor)};
    if (extent.width == 0 || extent.height == 0) return false;
    if (extent.width == vk->extent.width && extent.height == vk->extent.height)
        return false;
    if (vk->scale_framebuffer == VK_NULL_HANDLE ||
        vk->scale_extent.width != extent.width ||
        vk->scale_extent.height != extent.height) {
        destroy_target(vk);
        if (!create_target(vk, extent)) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[vulkan] 渲染精度离屏目标创建失败 (%ux%u), 退回按窗口尺寸直接渲染",
                (unsigned)extent.width, (unsigned)extent.height);
            destroy_target(vk);
            return false;
        }
    }
    vk->scale_active = true;
    *out_extent = extent;
    return true;
}

/* end_frame 里在 end render pass 之后调用: 离屏 -> 交换链的线性缩放 + 布局转换。 */
void bongo_cat_vk_scale_record_blit(BongoCatVk *vk, VkCommandBuffer command) {
    if (!vk || !command || !vk->scale_active ||
        vk->scale_image == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk->scale_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    /*
     * 交换链图像这一帧从没被当作附件用过: 布局按 UNDEFINED 起步 (内容本来就要被整帧
     * 覆盖), 目标是 TRANSFER_DST。
     */
    barrier.image = vk->images[vk->image_index];
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)vk->scale_extent.width;
    blit.srcOffsets[1].y = (int32_t)vk->scale_extent.height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1].x = (int32_t)vk->extent.width;
    blit.dstOffsets[1].y = (int32_t)vk->extent.height;
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(command, vk->scale_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vk->images[vk->image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
        VK_FILTER_LINEAR);
    /* 之后回读路径与呈现都要求交换链图像处在 PRESENT_SRC。 */
    barrier.image = vk->images[vk->image_index];
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

#endif /* BONGO_CAT_HAS_VULKAN */
