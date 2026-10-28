/*
 * 一帧的 Vulkan 生命周期: begin (等本帧槽的 fence / 取交换链图像 / 开 render pass)
 * -> 绘制 -> end (记录回读 / 提交 / 呈现)。
 *
 * 不变量与取舍:
 *   - image_available 用帧槽、present 信号量用图像索引: 同一张图像只有在上一次呈现
 *     完成之后才会被重新获取, 混用会撞上"信号量还在被等待" (同步对象的创建/重置/
 *     销毁都在 vk_frame_sync.c);
 *   - end_frame 里刻意不加任何等待: 提交与呈现都是发出去就返回, 该等的在 begin 等。
 *     设置窗口 present 那十几毫秒是 DWM 的取帧节奏 (同一 API 的宠物窗口只要 0.4ms),
 *     在这里加同步只会更慢;
 *   - 回读必须在 end render pass 之后、vkEndCommandBuffer 之前记录。
 */

#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#ifdef BONGO_CAT_HAS_VULKAN

bool bongo_cat_vk_frame_active(const BongoCatVk *vk) {
    return vk && vk->frame_active;
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
    {
        /* 栅栏等待失败以前是静默 return false: 表现就是"一帧都不出, 日志里没线索"。 */
        VkResult waited = vkWaitForFences(vk->device, 1, &frame->fence, VK_TRUE,
            UINT64_MAX);
        if (waited != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 等待帧栅栏失败 (VkResult=%d %s) frame_index=%u device=%p",
                (int)waited, bongo_cat_vk_result_name(waited), vk->frame_index,
                (void *)vk->device);
            return false;
        }
    }
    /* 栅栏已过: 上一轮用这个帧槽录制的回读数据现在是安全的 */
    bongo_cat_vk_collect_readback(vk);
    bongo_cat_vk_collect_retired(vk);
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX,
        frame->image_available, VK_NULL_HANDLE, &index);
    vk->last_acquire_result = (int)acquired;
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        /*
         * 尺寸/表面变了。此时 image_available 的状态是未定义的, 不能拿它继续提交,
         * 所以重建交换链 (重建时会一并重建各帧的信号量) 并放弃这一帧。
         *
         * 这条以前是完全静默的: 交换链过期时用户只会看到"画面不动", 日志里一个字
         * 都没有。现在按"连续第几次"升级为 WARN, 单次只记 INFO —— 拖窗口边缘时会
         * 连续发生很多次, 每次一条 WARN 会把日志冲掉。
         */
        vk->acquire_failures_total++;
        vk->consecutive_acquire_failures++;
        if (vk->consecutive_acquire_failures == 1 ||
            vk->consecutive_acquire_failures % 30u == 0)
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 获取交换链图像失败 (VkResult=%d %s), "
                "连续=%u 累计=%llu: 重建交换链并放弃这一帧",
                (int)acquired, bongo_cat_vk_result_name(acquired),
                vk->consecutive_acquire_failures,
                (unsigned long long)vk->acquire_failures_total);
        else
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[vulkan] 交换链过期 (VkResult=%d %s), 重建后重试 (连续=%u)",
                (int)acquired, bongo_cat_vk_result_name(acquired),
                vk->consecutive_acquire_failures);
        bongo_cat_vk_recreate_swapchain(vk, width, height);
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vk->acquire_failures_total++;
        vk->consecutive_acquire_failures++;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 获取交换链图像失败 (VkResult=%d %s), 连续=%u 累计=%llu "
            "swapchain=%p device=%p",
            (int)acquired, bongo_cat_vk_result_name(acquired),
            vk->consecutive_acquire_failures,
            (unsigned long long)vk->acquire_failures_total,
            (void *)vk->swapchain, (void *)vk->device);
        return false;
    }
    if (acquired == VK_SUBOPTIMAL_KHR)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 交换链已次优 (VK_SUBOPTIMAL_KHR), 本帧照常呈现");
    vk->consecutive_acquire_failures = 0;
    if (index >= vk->image_count) {
        /* 刚拿到的下标不属于当前交换链: 同样重建后跳过这一帧 */
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 获取到的图像下标越界 (index=%u images=%u), 重建交换链",
            index, vk->image_count);
        bongo_cat_vk_recreate_swapchain(vk, width, height);
        return false;
    }
    /* 这三步以前都是静默 return false: 首帧卡死时日志里"进入首帧"之后没有任何线索。
       现在任何非成功都带上枚举名, 与上面那些 VkResult 日志同一口径。 */
    VkResult reset_fences = vkResetFences(vk->device, 1, &frame->fence);
    if (reset_fences != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 重置帧栅栏失败 (VkResult=%d %s) fence=%p",
            (int)reset_fences, bongo_cat_vk_result_name(reset_fences),
            (void *)frame->fence);
        return false;
    }
    VkResult reset_command = vkResetCommandBuffer(frame->command, 0);
    if (reset_command != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 重置命令缓冲失败 (VkResult=%d %s) command=%p",
            (int)reset_command, bongo_cat_vk_result_name(reset_command),
            (void *)frame->command);
        return false;
    }

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult begun = vkBeginCommandBuffer(frame->command, &begin);
    if (begun != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 命令缓冲开始失败 (VkResult=%d %s) command=%p",
            (int)begun, bongo_cat_vk_result_name(begun), (void *)frame->command);
        return false;
    }

    VkClearValue clear = {0};
    clear.color = vk->clear;
    /*
     * 渲染精度: 请求 N 倍 (且离屏目标可用) 时, 这一帧的 render pass 指向 N 倍大小的
     * 离屏帧缓冲, 视口/裁剪也按那个 extent; end_frame 里再线性缩回交换链。
     * 返回 false 表示"直接画到交换链", 与 1.17.0 完全一致 (默认配置就是这条路径)。
     */
    VkExtent2D pass_extent = vk->extent;
    VkFramebuffer pass_framebuffer = vk->framebuffers[index];
    if (bongo_cat_vk_scale_prepare(vk, width, height, &pass_extent))
        pass_framebuffer = vk->scale_framebuffer;
    /* 绘制入口按这个 extent 换算出视口/裁剪 (见 vk_draw.c), 默认等于交换链尺寸 */
    vk->render_extent = pass_extent;
    VkRenderPassBeginInfo pass = {0};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = vk->render_pass;
    pass.framebuffer = pass_framebuffer;
    pass.renderArea.extent = pass_extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(frame->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {0};
    viewport.width = (float)pass_extent.width;
    viewport.height = (float)pass_extent.height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame->command, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {pass_extent.width, pass_extent.height}};
    vkCmdSetScissor(frame->command, 0, 1, &scissor);

    vk->image_index = index;
    /* 每帧的顶点/索引数据从零开始追加 */
    vk->arenas[vk->frame_index].offset = 0;
    vk->index_arenas[vk->frame_index].offset = 0;
    vk->dial_arenas[vk->frame_index].offset = 0;
    vk->frame_active = true;
    /*
     * 首帧关键点 1/3: 进入首帧。用户报告过"日志停在 create_renderer 之后再无输出",
     * 所以这一段必须有锚点 —— 没有这一行, 就分不清"卡在拿交换链图像"还是"卡在画"。
     */
    if (!vk->first_frame_begin_logged) {
        vk->first_frame_begin_logged = true;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 首帧 进入: swapchain=%ux%u images=%u image_index=%u "
            "format=%d present_mode=%d composite_alpha=0x%x device=%s driver=%s "
            "offscreen=%d window=%p",
            (unsigned)vk->extent.width, (unsigned)vk->extent.height,
            vk->image_count, index, (int)vk->format, (int)vk->present_mode,
            (unsigned)vk->composite_alpha, vk->info.device, vk->info.driver,
            vk->scale_active ? 1 : 0, (void *)vk->window);
    }
    return true;
}

bool bongo_cat_vk_end_frame(BongoCatVk *vk) {
    if (!vk || !vk->frame_active) return false;
    BongoCatVkFrame *frame = &vk->frames[vk->frame_index];
    vkCmdEndRenderPass(frame->command);
    /* 渲染精度: 先把 N 倍离屏图线性缩回交换链, 之后的回读读到的就是窗口尺寸那一帧 */
    if (vk->scale_active) bongo_cat_vk_scale_record_blit(vk, frame->command);
    /* 命中测试需要这一帧的 alpha: 在提交前把指针附近的小块拷回主机 */
    if (vk->readback[vk->frame_index].requested)
        record_readback(vk, &vk->readback[vk->frame_index]);
    VkResult ended = vkEndCommandBuffer(frame->command);
    if (ended != VK_SUCCESS) {
        vk->frame_active = false;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 命令缓冲结束失败 (VkResult=%d %s) command=%p",
            (int)ended, bongo_cat_vk_result_name(ended), (void *)frame->command);
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
    const uint64_t submit_start_ns = SDL_GetTicksNS();
    {
        VkResult submitted = vkQueueSubmit(vk->queue, 1, &submit, frame->fence);
        if (submitted != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 命令提交失败 (VkResult=%d %s) queue=%p image_index=%u "
                "extent=%ux%u",
                (int)submitted, bongo_cat_vk_result_name(submitted),
                (void *)vk->queue, vk->image_index,
                (unsigned)vk->extent.width, (unsigned)vk->extent.height);
            return false;
        }
        /* 首帧关键点 2/3: 已提交。走到这里说明命令缓冲录制与队列提交都成功了,
           之后再没有任何输出, 问题就在驱动/呈现侧而不是我们的录制代码。 */
        if (!vk->first_frame_submit_logged) {
            vk->first_frame_submit_logged = true;
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[vulkan] 首帧 提交: VkResult=%d (%s) queue=%p fence=%p "
                "image_index=%u",
                (int)submitted, bongo_cat_vk_result_name(submitted),
                (void *)vk->queue, (void *)frame->fence, vk->image_index);
        }
    }
    const uint64_t submit_end_ns = SDL_GetTicksNS();
    VkPresentInfoKHR present = {0};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &present_semaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &vk->swapchain;
    present.pImageIndices = &vk->image_index;
    VkResult result = vkQueuePresentKHR(vk->queue, &present);
    vk->last_present_result = (int)result;
    /*
     * 首帧关键点 3/3: present 的返回码。**成功也要打** —— 用户看到的"没有输出"
     * 与"present 成功了但画面是黑的"是两种完全不同的故障, 只有这一行能分开它们。
     */
    if (!vk->first_frame_present_logged) {
        vk->first_frame_present_logged = true;
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[vulkan] 首帧 present 返回: VkResult=%d (%s) swapchain=%ux%u "
                "image_index=%u presented_total=%llu",
                (int)result, bongo_cat_vk_result_name(result),
                (unsigned)vk->extent.width, (unsigned)vk->extent.height,
                vk->image_index,
                (unsigned long long)(vk->presented_total + 1));
        else
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 首帧 present 返回: VkResult=%d (%s) swapchain=%ux%u "
                "image_index=%u present_mode=%d composite_alpha=0x%x",
                (int)result, bongo_cat_vk_result_name(result),
                (unsigned)vk->extent.width, (unsigned)vk->extent.height,
                vk->image_index, (int)vk->present_mode,
                (unsigned)vk->composite_alpha);
    }
    if (SDL_getenv("BONGO_CAT_TEST_PREFS_STAGES")) {
        const uint64_t present_end_ns = SDL_GetTicksNS();
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] submit=%.2fms present_wait=%.2fms extent=%ux%u",
            (double)(submit_end_ns - submit_start_ns) / 1000000.0,
            (double)(present_end_ns - submit_end_ns) / 1000000.0,
            vk->extent.width, vk->extent.height);
    }
    vk->frame_index = (vk->frame_index + 1) % BONGO_CAT_VK_FRAMES_IN_FLIGHT;
    vk->frame_counter++;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        /*
         * SUBOPTIMAL 仍然是一帧端出去了, 只是驱动建议重建; OUT_OF_DATE 则是真的
         * 没端出去。两者必须分开计数, 否则"presented=0" 会把次优但当帧成功的情况
         * 也一并算成失败, 掩盖真正的问题。
         */
        if (result == VK_SUBOPTIMAL_KHR) {
            vk->presented_total++;
            vk->consecutive_present_failures = 0;
            if (!vk->present_success_logged) {
                vk->present_success_logged = true;
                SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                    "[vulkan] 首次呈现成功 (VkResult=VK_SUBOPTIMAL_KHR) "
                    "swapchain=%ux%u present_mode=%d window=%p",
                    (unsigned)vk->extent.width, (unsigned)vk->extent.height,
                    (int)vk->present_mode, (void *)vk->window);
            }
        } else {
            vk->present_failures_total++;
            vk->consecutive_present_failures++;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 呈现被驱动判为过期 (VkResult=%d %s), "
                "连续=%u 累计=%llu: 下一帧 begin 时重建交换链",
                (int)result, bongo_cat_vk_result_name(result),
                vk->consecutive_present_failures,
                (unsigned long long)vk->present_failures_total);
        }
        /* 尺寸变化或驱动要求重建: 下一帧 begin 时会按当前窗口尺寸重建。 */
        vk->extent_valid = false;
        return result == VK_SUBOPTIMAL_KHR;
    }
    if (result != VK_SUCCESS) {
        /*
         * 呈现失败以前只记一行 Error 就返回 false, 上层看到的只是"这一帧没成";
         * 连续失败才是"窗口永远不会更新"的真正形态, 所以这里把连续次数与关键句柄
         * 一起打出来, 并每 30 次升级一次提示 (逐帧打会把日志冲垮)。
         */
        vk->present_failures_total++;
        vk->consecutive_present_failures++;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 呈现失败 (VkResult=%d %s) 连续=%u 累计=%llu swapchain=%p "
            "device=%p queue=%p image_index=%u extent=%ux%u present_mode=%d",
            (int)result, bongo_cat_vk_result_name(result),
            vk->consecutive_present_failures,
            (unsigned long long)vk->present_failures_total, (void *)vk->swapchain,
            (void *)vk->device, (void *)vk->queue, vk->image_index,
            (unsigned)vk->extent.width, (unsigned)vk->extent.height,
            (int)vk->present_mode);
        if (vk->consecutive_present_failures % 30u == 0)
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 已连续 %u 次呈现失败, 画面不会再更新 (累计 %llu 次): "
                "上层应回退 OpenGL",
                vk->consecutive_present_failures,
                (unsigned long long)vk->present_failures_total);
        return false;
    }
    vk->presented_total++;
    vk->consecutive_present_failures = 0;
    if (!vk->present_success_logged) {
        vk->present_success_logged = true;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 首次呈现成功 swapchain=%ux%u images=%u present_mode=%d "
            "composite_alpha=0x%x window=%p",
            (unsigned)vk->extent.width, (unsigned)vk->extent.height,
            vk->image_count, (int)vk->present_mode,
            (unsigned)vk->composite_alpha, (void *)vk->window);
    }
    return true;
}

#endif /* BONGO_CAT_HAS_VULKAN */
