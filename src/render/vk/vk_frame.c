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
            vkCreateSemaphore(vk->device, &semaphore, NULL, &frame->render_finished) !=
                VK_SUCCESS ||
            vkCreateFence(vk->device, &fence, NULL, &frame->fence) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 同步对象创建失败");
            return false;
        }
    }
    vk->frame_index = 0;
    return true;
}

void bongo_cat_vk_destroy_frame_sync(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkFrame *frame = &vk->frames[i];
        if (frame->render_finished != VK_NULL_HANDLE)
            vkDestroySemaphore(vk->device, frame->render_finished, NULL);
        if (frame->image_available != VK_NULL_HANDLE)
            vkDestroySemaphore(vk->device, frame->image_available, NULL);
        if (frame->fence != VK_NULL_HANDLE)
            vkDestroyFence(vk->device, frame->fence, NULL);
        frame->render_finished = VK_NULL_HANDLE;
        frame->image_available = VK_NULL_HANDLE;
        frame->fence = VK_NULL_HANDLE;
        frame->command = VK_NULL_HANDLE;
    }
}

bool bongo_cat_vk_frame_active(const BongoCatVk *vk) {
    return vk && vk->frame_active;
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
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX,
        frame->image_available, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!bongo_cat_vk_recreate_swapchain(vk, width, height)) return false;
        acquired = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX,
            frame->image_available, VK_NULL_HANDLE, &index);
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 获取交换链图像失败 (VkResult=%d)", (int)acquired);
        return false;
    }
    if (index >= vk->image_count) return false;
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
    /* 每帧的顶点数据从零开始追加 */
    vk->arenas[vk->frame_index].offset = 0;
    vk->frame_active = true;
    return true;
}

bool bongo_cat_vk_end_frame(BongoCatVk *vk) {
    if (!vk || !vk->frame_active) return false;
    BongoCatVkFrame *frame = &vk->frames[vk->frame_index];
    vkCmdEndRenderPass(frame->command);
    if (vkEndCommandBuffer(frame->command) != VK_SUCCESS) {
        vk->frame_active = false;
        return false;
    }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame->image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame->command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame->render_finished;
    vk->frame_active = false;
    if (vkQueueSubmit(vk->queue, 1, &submit, frame->fence) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 命令提交失败");
        return false;
    }
    VkPresentInfoKHR present = {0};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame->render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &vk->swapchain;
    present.pImageIndices = &vk->image_index;
    VkResult result = vkQueuePresentKHR(vk->queue, &present);
    vk->frame_index = (vk->frame_index + 1) % BONGO_CAT_VK_FRAMES_IN_FLIGHT;
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
