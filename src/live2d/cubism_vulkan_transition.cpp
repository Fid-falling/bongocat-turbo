/*
 * SDK 的 Vulkan 渲染器把模型画进离屏图像后, 图像停在 COLOR_ATTACHMENT_OPTIMAL;
 * 要被我们的合成着色器采样, 必须先转成 SHADER_READ_ONLY_OPTIMAL。
 * 从 cubism_vulkan_bridge.cpp 拆出来: 那个文件已经接近行数上限 (500)。
 */

#include "live2d_vulkan_internal.h"

#ifdef BONGO_CAT_HAS_VULKAN

#include <vulkan/vulkan.h>

bool bongo_cat_live2d_vulkan_transition_sampled(const BongoCatVkNativeInfo *info,
    void *image) {
    if (!info || !info->device || !info->command_pool || !info->queue || !image)
        return false;
    VkDevice device = (VkDevice)info->device;
    VkCommandPool pool = (VkCommandPool)info->command_pool;
    VkQueue queue = (VkQueue)info->queue;
    /* 单独开一个一次性命令缓冲, 提交后等空闲: SDK 的绘制命令是它自己提交的, 我们在这
     * 之后才录制帧。阻塞式等待把"SDK 提交"和"我们采样"完全串行, 避免只靠屏障却仍有
     * 在飞提交的竞态 (桥的 end_frame 里还有一次 vkDeviceWaitIdle 做同样的隔离)。 */
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocate, &command) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, pool, 1, &command);
        return false;
    }
    /* srcAccess/dstAccess 与两个 stage 必须配对: 写发生在颜色附件输出阶段, 读发生在
     * 片元着色阶段。少写一个, 驱动就可能把它重排成"先采样、后写完"。
     * 全程只有一个队列族, 所以 queue family 用 IGNORED, 不需要所有权转移。 */
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = (VkImage)image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vkEndCommandBuffer(command);
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    bool ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    if (ok) vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &command);
    return ok;
}

#else /* !BONGO_CAT_HAS_VULKAN */

bool bongo_cat_live2d_vulkan_transition_sampled(const BongoCatVkNativeInfo *info,
    void *image) {
    (void)info; (void)image;
    return false;
}

#endif
