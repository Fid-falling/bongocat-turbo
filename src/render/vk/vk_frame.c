#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#ifdef BONGO_CAT_HAS_VULKAN

static bool create_command_pool(BongoCatVk *vk, BongoCatError *error) {
    VkCommandPoolCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create.queueFamilyIndex = vk->queue_family;
    if (vkCreateCommandPool(vk->device, &create, NULL, &vk->command_pool) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 命令池创建失败");
        return false;
    }
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = vk->command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = BONGO_CAT_VK_FRAMES_IN_FLIGHT;
    VkCommandBuffer buffers[BONGO_CAT_VK_FRAMES_IN_FLIGHT] = {0};
    if (vkAllocateCommandBuffers(vk->device, &allocate, buffers) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 命令缓冲分配失败");
        return false;
    }
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i)
        vk->frames[i].command = buffers[i];
    return true;
}

bool bongo_cat_vk_create_frame_sync(BongoCatVk *vk, BongoCatError *error) {
    if (!create_command_pool(vk, error)) return false;
    VkSemaphoreCreateInfo semaphore = {0};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence = {0};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    /* 第一帧不必等待一个从未提交过的栅栏 */
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkFrame *frame = &vk->frames[i];
        if (vkCreateSemaphore(vk->device, &semaphore, NULL, &frame->image_available) !=
                VK_SUCCESS ||
            vkCreateFence(vk->device, &fence, NULL, &frame->fence) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 同步对象创建失败");
            return false;
        }
    }
    vk->frame_index = 0;
    return true;
}

/*
 * 重建各帧的 image_available 信号量。
 *
 * vkAcquireNextImageKHR 可能已经把信号量置为已触发, 但这一帧最终没有提交
 * (例如返回 OUT_OF_DATE 或拿到的下标失效)。未消费的信号量再次传给 acquire 是
 * 未定义行为, 驱动的表现就是偶发崩溃。调用前必须已经 vkDeviceWaitIdle。
 */
bool bongo_cat_vk_reset_frame_semaphores(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return false;
    VkSemaphoreCreateInfo semaphore = {0};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkFrame *frame = &vk->frames[i];
        if (frame->image_available != VK_NULL_HANDLE)
            vkDestroySemaphore(vk->device, frame->image_available, NULL);
        frame->image_available = VK_NULL_HANDLE;
        if (vkCreateSemaphore(vk->device, &semaphore, NULL,
                &frame->image_available) != VK_SUCCESS) return false;
    }
    /* 所有帧都空闲了, 从第一个帧槽重新开始 */
    vk->frame_index = 0;
    return true;
}

void bongo_cat_vk_destroy_frame_sync(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkFrame *frame = &vk->frames[i];
        if (frame->image_available != VK_NULL_HANDLE)
            vkDestroySemaphore(vk->device, frame->image_available, NULL);
        if (frame->fence != VK_NULL_HANDLE)
            vkDestroyFence(vk->device, frame->fence, NULL);
        frame->image_available = VK_NULL_HANDLE;
        frame->fence = VK_NULL_HANDLE;
        frame->command = VK_NULL_HANDLE;
    }
}

bool bongo_cat_vk_frame_active(const BongoCatVk *vk) {
    return vk && vk->frame_active;
}

/* 上一次用这个帧槽录制的回读已经完成: 把 alpha 取出来放进缓存 */
static void collect_readback(BongoCatVk *vk, BongoCatVkReadback *readback) {
    if (!readback->pending || !readback->mapped) return;
    readback->pending = false;
    if (readback->width <= 0 || readback->height <= 0) return;
    for (int y = 0; y < readback->height; ++y) {
        for (int x = 0; x < readback->width; ++x) {
            vk->hit_alpha[y * readback->width + x] =
                readback->mapped[((size_t)y * (size_t)readback->width +
                    (size_t)x) * 4 + 3];
        }
    }
    vk->hit_x = readback->origin_x;
    vk->hit_y = readback->origin_y;
    vk->hit_width = readback->width;
    vk->hit_height = readback->height;
    if (!vk->hit_valid)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 首次命中测试回读成功: 区域=%dx%d 原点=(%d,%d)",
            readback->width, readback->height, readback->origin_x,
            readback->origin_y);
    vk->hit_valid = true;
}

bool bongo_cat_vk_queue_hit_sample(BongoCatVk *vk, int x, int y) {
    if (!vk || !vk->frame_active || !vk->transfer_source) return false;
    const int tile = BONGO_CAT_VK_HIT_TILE;
    if ((int)vk->extent.width < tile || (int)vk->extent.height < tile) return false;
    if (x < 0 || y < 0 || x >= (int)vk->extent.width || y >= (int)vk->extent.height)
        return false;
    int origin_x = x - tile / 2;
    int origin_y = y - tile / 2;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;
    if (origin_x + tile > (int)vk->extent.width)
        origin_x = (int)vk->extent.width - tile;
    if (origin_y + tile > (int)vk->extent.height)
        origin_y = (int)vk->extent.height - tile;
    BongoCatVkReadback *readback = &vk->readback[vk->frame_index];
    readback->requested = true;
    readback->origin_x = origin_x;
    readback->origin_y = origin_y;
    readback->width = tile;
    readback->height = tile;
    return true;
}

bool bongo_cat_vk_hit_alpha(const BongoCatVk *vk, int x, int y,
    unsigned char *alpha) {
    if (!vk || !vk->hit_valid || !alpha) return false;
    if (x < vk->hit_x || y < vk->hit_y ||
        x >= vk->hit_x + vk->hit_width || y >= vk->hit_y + vk->hit_height)
        return false;
    *alpha = vk->hit_alpha[(size_t)(y - vk->hit_y) * (size_t)vk->hit_width +
        (size_t)(x - vk->hit_x)];
    return true;
}

static void record_readback(BongoCatVk *vk, BongoCatVkReadback *readback) {
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk->images[vk->image_index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkCommandBuffer command = vk->frames[vk->frame_index].command;
    /* 渲染通道结束时图像已是 PRESENT_SRC, 拷贝前后各转一次布局 */
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = readback->origin_x;
    region.imageOffset.y = readback->origin_y;
    region.imageExtent.width = (uint32_t)readback->width;
    region.imageExtent.height = (uint32_t)readback->height;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(command, vk->images[vk->image_index],
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    readback->requested = false;
    readback->pending = true;
}

bool bongo_cat_vk_begin_frame(BongoCatVk *vk, int width, int height) {
    if (!vk || vk->device == VK_NULL_HANDLE) return false;
    if (vk->frame_active) return false;
    if (width <= 0 || height <= 0) return false;
    if (!vk->extent_valid || (uint32_t)width != vk->extent.width ||
        (uint32_t)height != vk->extent.height) {
        if (!bongo_cat_vk_recreate_swapchain(vk, width, height)) return false;
    }
    BongoCatVkFrame *frame = &vk->frames[vk->frame_index];
    if (vkWaitForFences(vk->device, 1, &frame->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    /* 栅栏已过: 上一轮用这个帧槽录制的回读数据现在是安全的 */
    collect_readback(vk, &vk->readback[vk->frame_index]);
    bongo_cat_vk_collect_retired(vk);
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX,
        frame->image_available, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        /*
         * 尺寸/表面变了。此时 image_available 的状态是未定义的, 不能拿它继续提交,
         * 所以重建交换链 (重建时会一并重建各帧的信号量) 并放弃这一帧。
         */
        bongo_cat_vk_recreate_swapchain(vk, width, height);
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 获取交换链图像失败 (VkResult=%d)", (int)acquired);
        return false;
    }
    if (index >= vk->image_count) {
        /* 刚拿到的下标不属于当前交换链: 同样重建后跳过这一帧 */
        bongo_cat_vk_recreate_swapchain(vk, width, height);
        return false;
    }
    if (vkResetFences(vk->device, 1, &frame->fence) != VK_SUCCESS) return false;
    if (vkResetCommandBuffer(frame->command, 0) != VK_SUCCESS) return false;

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame->command, &begin) != VK_SUCCESS) return false;

    VkClearValue clear = {0};
    clear.color = vk->clear;
    VkRenderPassBeginInfo pass = {0};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = vk->render_pass;
    pass.framebuffer = vk->framebuffers[index];
    pass.renderArea.extent = vk->extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(frame->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {0};
    viewport.width = (float)vk->extent.width;
    viewport.height = (float)vk->extent.height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame->command, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {vk->extent.width, vk->extent.height}};
    vkCmdSetScissor(frame->command, 0, 1, &scissor);

    vk->image_index = index;
    /* 每帧的顶点/索引数据从零开始追加 */
    vk->arenas[vk->frame_index].offset = 0;
    vk->index_arenas[vk->frame_index].offset = 0;
    vk->dial_arenas[vk->frame_index].offset = 0;
    vk->frame_active = true;
    return true;
}

bool bongo_cat_vk_end_frame(BongoCatVk *vk) {
    if (!vk || !vk->frame_active) return false;
    BongoCatVkFrame *frame = &vk->frames[vk->frame_index];
    vkCmdEndRenderPass(frame->command);
    /* 命中测试需要这一帧的 alpha: 在提交前把指针附近的小块拷回主机 */
    if (vk->readback[vk->frame_index].requested)
        record_readback(vk, &vk->readback[vk->frame_index]);
    if (vkEndCommandBuffer(frame->command) != VK_SUCCESS) {
        vk->frame_active = false;
        return false;
    }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    /* 呈现信号量按图像索引取: 同一张图像只有在上一次呈现完成后才会被重新获取 */
    VkSemaphore present_semaphore = vk->present_semaphores[vk->image_index];
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame->image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame->command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &present_semaphore;
    vk->frame_active = false;
    if (vkQueueSubmit(vk->queue, 1, &submit, frame->fence) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 命令提交失败");
        return false;
    }
    VkPresentInfoKHR present = {0};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &present_semaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &vk->swapchain;
    present.pImageIndices = &vk->image_index;
    VkResult result = vkQueuePresentKHR(vk->queue, &present);
    vk->frame_index = (vk->frame_index + 1) % BONGO_CAT_VK_FRAMES_IN_FLIGHT;
    vk->frame_counter++;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        /* 尺寸变化或驱动要求重建: 下一帧 begin 时会按当前窗口尺寸重建。 */
        vk->extent_valid = false;
        return result == VK_SUBOPTIMAL_KHR;
    }
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 呈现失败 (VkResult=%d)", (int)result);
        return false;
    }
    return true;
}

#endif /* BONGO_CAT_HAS_VULKAN */
