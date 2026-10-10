#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#ifdef BONGO_CAT_HAS_VULKAN

/*
 * 交换链的"重建"路径: 尺寸变化、垂直同步切换、以及驱动报告的过期交换链。
 * 创建/销毁本身留在 vk_swapchain.c, 拆成两个文件是为了守住单文件 500 行的策略
 * (cmake/CheckLines.cmake)。
 */

bool bongo_cat_vk_recreate_swapchain(BongoCatVk *vk, int width, int height) {
    if (!vk || vk->device == VK_NULL_HANDLE || vk->surface == VK_NULL_HANDLE) return false;
    vkDeviceWaitIdle(vk->device);
    /*
     * 只在没有活动帧时才重建帧信号量。
     *
     * 若这一帧已经 acquire (begin_frame 之后、end_frame 之前), 那个 acquire 信号量
     * 正被本帧的 submit 使用; 销毁它会让我们随后拿已销毁的信号量去 submit/present,
     * 驱动的表现就是在 vkQueuePresentKHR 里崩 (nvoglv64 + 0xc0000005)。
     * 设置窗口恰好命中这个时序: 它在隐藏状态下建窗建交换链, 随后立刻显示/改尺寸,
     * 于是发生了"帧中途重建交换链"。
     */
    if (!vk->frame_active && !bongo_cat_vk_reset_frame_semaphores(vk)) return false;
    /*
     * 作废挂起的回读并把 image_index 归零: 回读是用 image_index 去索引图像数组的
     * (vk_frame.c 的 record_readback), 而它来自上一帧的 acquire。交换链一重建,
     * 旧下标就可能超出新交换链的图像数 -> 越界 -> 驱动崩溃 (nvoglv64 + 0xc0000005)。
     * 设置窗口恰好是"排队回读 + 立刻改尺寸"的组合, 所以只有它会崩。
     */
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        vk->readback[i].requested = false;
        vk->readback[i].pending = false;
        vk->readback[i].whole_frame = false;
    }
    vk->frame_ready = false;
    vk->image_index = 0;
    bongo_cat_vk_destroy_swapchain(vk);
    BongoCatError error = {0};
    if (!bongo_cat_vk_create_swapchain(vk, width, height, &error)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 交换链重建失败: %s",
            error.message[0] ? error.message : "unknown");
        return false;
    }
    return true;
}

bool bongo_cat_vk_resize(BongoCatVk *vk, int width, int height) {
    if (!vk || width <= 0 || height <= 0) return false;
    if (vk->extent_valid && (uint32_t)width == vk->extent.width &&
        (uint32_t)height == vk->extent.height) return true;
    return bongo_cat_vk_recreate_swapchain(vk, width, height);
}

void bongo_cat_vk_set_vsync(BongoCatVk *vk, bool vsync) {
    if (!vk || vk->vsync == vsync) return;
    vk->vsync = vsync;
    /*
     * 呈现模式是创建交换链时定下的, 只能重建才能换。尺寸没变也要重建, 所以
     * 不能走 bongo_cat_vk_resize (它按 extent 提前返回)。
     */
    if (vk->device == VK_NULL_HANDLE || vk->surface == VK_NULL_HANDLE ||
        !vk->extent_valid) return;
    bongo_cat_vk_recreate_swapchain(vk, (int)vk->extent.width,
        (int)vk->extent.height);
    SDL_Log("[vulkan] 垂直同步已%s (present_mode=%d)", vsync ? "开启" : "关闭",
        (int)vk->present_mode);
}

#endif /* BONGO_CAT_HAS_VULKAN */
