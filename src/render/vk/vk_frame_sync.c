/*
 * 帧同步对象的创建/重置/销毁, 从 vk_frame.c 拆出来只为守住单文件 500 行
 * (cmake/CheckLines.cmake)。vk_frame.c 管"一帧怎么走" (begin/draw/end),
 * 这里管"帧与帧之间靠什么同步"。
 *
 * 同步模型 (改之前先读):
 *   - image_available 信号量按帧槽持有, present 信号量按交换链图像索引持有。
 *     混用会撞上"信号量还在被等待"(VUID 00067) —— 同一张图像只会在它上一次呈现
 *     完成之后被重新获取, 所以按图像索引复用 present 信号量是安全的;
 *   - 栅栏 (fence) 按帧槽持有, 第一帧给 SIGNALED 初始值, 免得等一个从未提交过的栅栏;
 *   - 交错重建是这里最危险的操作: acquire 已经触发信号量但这一帧最终没提交时, 那个
 *     信号量处于"未消费"状态, 再次传给 acquire 是未定义行为。所以重建前必须
 *     vkDeviceWaitIdle + 重建所有 image_available 信号量 (见 reset_frame_semaphores)。
 */
#include "vk_internal.h"

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

#endif /* BONGO_CAT_HAS_VULKAN */
