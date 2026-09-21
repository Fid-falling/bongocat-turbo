#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

static bool choose_format(const VkSurfaceFormatKHR *formats, uint32_t count,
    VkFormat *format, VkColorSpaceKHR *space) {
    /* 线性 UNORM 与 OpenGL 路径的非 sRGB 默认帧缓冲一致, 避免透明度合成时
       多一次 gamma 编码。SRGB 只作为兜底。 */
    const VkFormat preferred[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB
    };
    for (size_t want = 0; want < sizeof(preferred) / sizeof(preferred[0]); ++want) {
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i].format == preferred[want]) {
                *format = formats[i].format;
                *space = formats[i].colorSpace;
                return true;
            }
        }
    }
    if (count) {
        *format = formats[0].format;
        *space = formats[0].colorSpace;
        return true;
    }
    return false;
}

/*
 * 窗口透明时交换链必须声明 PRE_MULTIPLIED, 否则清屏的 (0,0,0,0) 会被合成为黑底。
 *
 * Windows 上 SDL 用 DwmEnableBlurBehindWindow (空模糊区域) 让 DWM 按窗口重定向
 * 表面的 alpha 逐像素合成 (见 sdl3-src/src/video/windows/SDL_windowswindow.c)。
 * OpenGL 的 GDI 表面天然带 alpha, 所以 GL 后端一直是透明的; 而 Vulkan 的
 * DXGI 交换链默认按不透明合成 (compositeAlpha=OPAQUE), DWM 拿到的 alpha 全是 1,
 * 于是桌面宠物的背景变成黑色方块。声明 PRE_MULTIPLIED 后 DWM 才会尊重
 * 帧缓冲里的 alpha —— 与 OpenGL 路径的观感一致 (两条路径都是非预乘内容,
 * 半透明边缘的处理也就与 GL 完全一致)。
 */
static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(
    VkCompositeAlphaFlagsKHR supported, bool transparent) {
    const VkCompositeAlphaFlagBitsKHR transparent_order[] = {
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
    };
    const VkCompositeAlphaFlagBitsKHR opaque_order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
    };
    const VkCompositeAlphaFlagBitsKHR *order = transparent ? transparent_order
        : opaque_order;
    for (size_t i = 0; i < 4; ++i)
        if (supported & order[i]) return order[i];
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/* 窗口是否要求逐像素透明 (关掉透明后就没必要让合成器去读 alpha)。 */
static bool window_is_transparent(const BongoCatVk *vk) {
    return vk->window &&
        (SDL_GetWindowFlags(vk->window) & SDL_WINDOW_TRANSPARENT) != 0;
}

static bool create_render_pass(BongoCatVk *vk, BongoCatError *error) {
    VkAttachmentDescription color = {0};
    color.format = vk->format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference = {0};
    reference.attachment = 0;
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependencies[2] = {{0}};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].dstAccessMask = 0;
    VkRenderPassCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = 1;
    create.pAttachments = &color;
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    create.dependencyCount = 2;
    create.pDependencies = dependencies;
    VkResult result = vkCreateRenderPass(vk->device, &create, NULL, &vk->render_pass);
    if (result != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 渲染通道创建失败 (VkResult=%d)", (int)result);
        return false;
    }
    return true;
}

void bongo_cat_vk_destroy_swapchain(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < vk->image_count; ++i) {
        if (vk->framebuffers[i] != VK_NULL_HANDLE)
            vkDestroyFramebuffer(vk->device, vk->framebuffers[i], NULL);
        if (vk->views[i] != VK_NULL_HANDLE)
            vkDestroyImageView(vk->device, vk->views[i], NULL);
        vk->framebuffers[i] = VK_NULL_HANDLE;
        vk->views[i] = VK_NULL_HANDLE;
        vk->images[i] = VK_NULL_HANDLE;
    }
    vk->image_count = 0;
    if (vk->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vk->device, vk->render_pass, NULL);
        vk->render_pass = VK_NULL_HANDLE;
    }
    if (vk->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
        vk->swapchain = VK_NULL_HANDLE;
    }
    /* 交换链销毁之后再释放呈现信号量: 它们此前由呈现操作使用 */
    for (uint32_t i = 0; i < BONGO_CAT_VK_MAX_SWAP_IMAGES; ++i) {
        if (vk->present_semaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, vk->present_semaphores[i], NULL);
            vk->present_semaphores[i] = VK_NULL_HANDLE;
        }
    }
    vk->extent_valid = false;
}

bool bongo_cat_vk_create_swapchain(BongoCatVk *vk, int width, int height,
    BongoCatError *error) {
    VkSurfaceCapabilitiesKHR capabilities;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical, vk->surface,
            &capabilities) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "无法读取 Vulkan 表面能力");
        return false;
    }
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical, vk->surface, &format_count, NULL);
    if (!format_count) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "窗口表面没有可用的 Vulkan 交换链格式");
        return false;
    }
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    if (!formats) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "交换链格式列表分配失败");
        return false;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical, vk->surface, &format_count, formats);
    bool format_ok = choose_format(formats, format_count, &vk->format, &vk->color_space);
    free(formats);
    if (!format_ok) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "没有可用的交换链格式");
        return false;
    }

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, vk->surface, &mode_count, NULL);
    VkPresentModeKHR *modes = calloc(mode_count ? mode_count : 1, sizeof(*modes));
    if (!modes) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "呈现模式列表分配失败");
        return false;
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, vk->surface, &mode_count, modes);
    /*
     * 呈现模式: 关掉垂直同步 (无限帧率) 时优先 IMMEDIATE, 其次 MAILBOX, 都没有
     * 才退回 FIFO; 开着同步时固定 FIFO。
     *
     * 之前是无条件 FIFO —— 即使用户把最大帧率设成"无限", 帧率也永远被锁在显示器
     * 刷新率上 (100 Hz 屏 = 100 FPS)。帧节奏本来就由应用侧 (frame_clock.c) 控制,
     * 这里只需要如实反映设置。
     */
    VkPresentModeKHR fallback = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR wanted = vk->vsync ? VK_PRESENT_MODE_FIFO_KHR :
        VK_PRESENT_MODE_IMMEDIATE_KHR;
    vk->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    bool found = false;
    for (uint32_t i = 0; i < mode_count; ++i)
        if (modes[i] == wanted) { vk->present_mode = wanted; found = true; }
    if (!found && !vk->vsync) {
        for (uint32_t i = 0; i < mode_count; ++i)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                vk->present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
                found = true;
            }
    }
    if (!found) vk->present_mode = fallback;
    free(modes);

    VkExtent2D extent = {0};
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = (uint32_t)SDL_clamp(width, (int)capabilities.minImageExtent.width,
            (int)capabilities.maxImageExtent.width);
        extent.height = (uint32_t)SDL_clamp(height, (int)capabilities.minImageExtent.height,
            (int)capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "窗口像素尺寸为 0 (%ux%u), 暂时无法创建交换链", extent.width, extent.height);
        return false;
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    if (image_count > BONGO_CAT_VK_MAX_SWAP_IMAGES) image_count = BONGO_CAT_VK_MAX_SWAP_IMAGES;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    vk->transfer_source = false;
    if (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        /* 命中测试回读依赖把交换链图像当拷贝源 */
        vk->transfer_source = true;
    }

    VkSwapchainCreateInfoKHR create = {0};
    create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create.surface = vk->surface;
    create.minImageCount = image_count;
    create.imageFormat = vk->format;
    create.imageColorSpace = vk->color_space;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = usage;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = capabilities.currentTransform;
    const bool transparent = window_is_transparent(vk);
    create.compositeAlpha = choose_composite_alpha(
        capabilities.supportedCompositeAlpha, transparent);
    /*
     * Windows 10 的 Win32 表面常常只报 OPAQUE (实测 RTX 3060 Ti + 驱动 1.4.303:
     * supported=0x1), 但 NVIDIA 这类驱动实际接受预乘合成。先按预乘试一次, 失败
     * 下面的梯子会退回不透明 —— 否则桌宠背景在 Win10 上永远是黑的。
     */
    if (transparent &&
        create.compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        create.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    create.presentMode = vk->present_mode;
    create.clipped = VK_TRUE;
    VkResult result = vkCreateSwapchainKHR(vk->device, &create, NULL, &vk->swapchain);
    if (result != VK_SUCCESS && create.compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        /*
         * 个别驱动会把 PRE_MULTIPLIED 报进 supportedCompositeAlpha 却拒绝创建
         * (预览版驱动/远程会话)。宁可退回不透明黑底, 也不能让整个 Vulkan 后端
         * 起不来。
         */
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] compositeAlpha=0x%x 创建失败 (VkResult=%d), 退回不透明合成",
            (unsigned)create.compositeAlpha, (int)result);
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        result = vkCreateSwapchainKHR(vk->device, &create, NULL, &vk->swapchain);
    }
    vk->composite_alpha = create.compositeAlpha;
    vk->composite_supported = capabilities.supportedCompositeAlpha;
    if (result != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 交换链创建失败 (VkResult=%d, %ux%u)", (int)result,
            extent.width, extent.height);
        return false;
    }
    vk->extent = extent;
    if (vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->image_count, NULL) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "无法读取交换链图像数量");
        bongo_cat_vk_destroy_swapchain(vk);
        return false;
    }
    if (vk->image_count > BONGO_CAT_VK_MAX_SWAP_IMAGES)
        vk->image_count = BONGO_CAT_VK_MAX_SWAP_IMAGES;
    if (vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->image_count,
            vk->images) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "无法读取交换链图像");
        bongo_cat_vk_destroy_swapchain(vk);
        return false;
    }
    if (!create_render_pass(vk, error)) {
        bongo_cat_vk_destroy_swapchain(vk);
        return false;
    }
    for (uint32_t i = 0; i < vk->image_count; ++i) {
        VkImageViewCreateInfo view = {0};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = vk->images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = vk->format;
        view.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vk->device, &view, NULL, &vk->views[i]) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "交换链图像视图创建失败 (index=%u)", i);
            bongo_cat_vk_destroy_swapchain(vk);
            return false;
        }
        VkFramebufferCreateInfo framebuffer = {0};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = vk->render_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &vk->views[i];
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        if (vkCreateFramebuffer(vk->device, &framebuffer, NULL,
                &vk->framebuffers[i]) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "交换链帧缓冲创建失败 (index=%u)", i);
            bongo_cat_vk_destroy_swapchain(vk);
            return false;
        }
    }
    vk->extent_valid = true;
    vk->info.width = (int)extent.width;
    vk->info.height = (int)extent.height;
    VkSemaphoreCreateInfo semaphore = {0};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < vk->image_count; ++i) {
        if (vkCreateSemaphore(vk->device, &semaphore, NULL,
                &vk->present_semaphores[i]) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "呈现信号量创建失败 (index=%u)", i);
            bongo_cat_vk_destroy_swapchain(vk);
            return false;
        }
    }
    /* 管线绑定渲染通道, 交换链重建后必须跟着重建 */
    bongo_cat_vk_rebuild_pipelines(vk);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[vulkan] 交换链 %ux%u images=%u format=%d present_mode=%d "
        "composite_alpha=0x%x supported=0x%x transparent=%d",
        extent.width, extent.height, vk->image_count, (int)vk->format,
        (int)vk->present_mode, (unsigned)vk->composite_alpha,
        (unsigned)vk->composite_supported, transparent ? 1 : 0);
    return true;
}

#endif /* BONGO_CAT_HAS_VULKAN */
