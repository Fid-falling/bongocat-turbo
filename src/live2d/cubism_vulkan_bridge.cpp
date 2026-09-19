/*
 * Cubism Live2D 渲染器的 Vulkan 桥。
 *
 * 目标: 用主窗口的 VkDevice/queue/command pool, 把模型渲染进一张离屏纹理,
 * 之后由 Vulkan 后端的合成路径画到宠物窗口上。参照 SDK 的
 * Samples/Vulkan/Demo (LAppDelegate / LAppView / LAppLive2DManager) 的调用顺序。
 *
 * 任何一步失败都返回 false, 让上层回退 OpenGL —— 绝不能让 Vulkan 路径把
 * 已经能工作的 GL 路径拖下水。
 */

#include "bongo_cat/live2d_vulkan.h"

#include "bongo_cat/vk_backend.h"
#include "cubism_renderer_select.hpp"

#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>

#ifdef BONGO_CAT_HAS_VULKAN

#include <Rendering/Vulkan/CubismDeviceInfo_Vulkan.hpp>
#include <Rendering/Vulkan/CubismRenderTarget_Vulkan.hpp>
#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>

namespace {

bool g_attached = false;
BongoCatVk *g_vk = nullptr;
/* remember() 记下的待挂载信息 */
BongoCatVk *g_pending_vk = nullptr;
int g_pending_width = 0;
int g_pending_height = 0;
Csm::Rendering::CubismRenderTarget_Vulkan *g_target = nullptr;
VkDevice g_device = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;

/* 24 位深度 + 模板在桌面 Vulkan 上普遍可用, SDK 的样例也用这一档 */
const VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

} // namespace

bool bongo_cat_live2d_vulkan_remember(struct BongoCatVk *vk, int width, int height) {
    if (!vk || width <= 0 || height <= 0) return false;
    g_pending_vk = vk;
    g_pending_width = width;
    g_pending_height = height;
    /* 只切工厂开关: 这一步不调用任何 SDK 函数, 可以在框架启动前做 */
    bongo_cat::bongo_cat_cubism_renderer_use_vulkan(true);
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] remember: vk=%p %dx%d (工厂开关已打开)",
        (void *)vk, width, height);
    return true;
}

bool bongo_cat_live2d_vulkan_attach(struct BongoCatVk *vk, int width, int height) {
    if (g_attached) return true;
    /* 模型侧调用时可以不带参数, 用 remember() 记下的值 */
    if (!vk) { vk = g_pending_vk; width = g_pending_width; height = g_pending_height; }
    if (!vk || width <= 0 || height <= 0) return false;
    BongoCatVkNativeInfo info = {0};
    if (!bongo_cat_vk_native_info(vk, &info)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[live2d/vk] attach: 后端句柄不可用");
        return false;
    }
    if (!info.device || !info.physical_device || !info.command_pool || !info.queue) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d/vk] attach: 句柄不完整 (device=%p physical=%p pool=%p queue=%p)",
            info.device, info.physical_device, info.command_pool, info.queue);
        return false;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: 探针 device=%p view=%p extent=%dx%d format=%d images=%d",
        info.device, info.image_view, info.width, info.height, info.image_format,
        info.image_count);
    g_device = (VkDevice)info.device;
    g_format = (VkFormat)info.image_format;

    VkExtent2D extent = {(unsigned int)width, (unsigned int)height};
    /*
     * 顺序与 SDK 示例一致 (LAppDelegate::initialize 先 SetConstantSettings,
     * LAppView 之后才 CreateRenderTarget): 设备级的管线/描述符是在
     * SetConstantSettings 里建的, 提前建离屏目标会因为拿不到它们而失败。
     * 默认目标先用交换链第 0 张图像 (没有视图时传 NULL)。
     */
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: Step 1 SetConstantSettings (view=%p extent=%dx%d "
        "format=%d depth=%d images=%d)",
        (void *)info.image_view, width, height, (int)g_format,
        (int)kDepthFormat, info.image_count);
    /*
     * SDK 的 Vulkan 渲染器按相对路径 "FrameworkShaders/*.spv" 打开着色器, 而
     * .spv 是随程序分发在 exe 目录下的。进程的工作目录不一定是 exe 目录, 所以
     * 这里显式切过去, 并打一条探针确认第一个着色器真的能打开。
     * 注意: SDL_GetBasePath() 返回 const char * 且由 SDL 自己持有, 不要 free。
     */
    {
        const char *base = SDL_GetBasePath();
        if (base) {
            std::error_code error_code;
            std::filesystem::current_path(base, error_code);
            std::ifstream probe("FrameworkShaders/FragShaderSrc.spv",
                std::ios::binary);
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[live2d/vk] attach: cwd=%s shader=%s",
                std::filesystem::current_path().string().c_str(),
                probe.is_open() ? "ok" : "MISSING");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[live2d/vk] attach: SDL_GetBasePath 失败");
        }
    }
    Csm::Rendering::CubismRenderer_Vulkan::SetConstantSettings(g_device,
        (VkPhysicalDevice)info.physical_device, (VkCommandPool)info.command_pool,
        (VkQueue)info.queue, (unsigned int)info.image_count, extent,
        /*
         * 默认目标视图传 NULL: 我们每帧都用 SetRenderTarget 指向自己的离屏
         * 图像 (配合 EnableChangeRenderTarget), 从不通过 Cubism 渲染到交换链。
         * 之前传的交换链视图在本环境里是无效句柄, SDK 用它建 framebuffer
         * 时会崩。
         */
        VK_NULL_HANDLE, g_format, kDepthFormat);
    Csm::Rendering::CubismRenderer_Vulkan::EnableChangeRenderTarget();
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: Step 2 SetConstantSettings 返回");
    /*
     * GetDeviceInfo 必须放在 SetConstantSettings 之后: SDK 示例的顺序是
     * LAppDelegate 先 SetConstantSettings, LAppManager 之后才 GetDeviceInfo。
     * 它内部会建 CubismPipeline_Vulkan (近 500 条管线 + 加载 .spv),
     * 在静态设置尚未建立时调用会直接崩。
     */
    if (!Csm::Rendering::CubismDeviceInfo_Vulkan::GetDeviceInfo(g_device)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[live2d/vk] attach: GetDeviceInfo 失败");
        return false;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: Step 3 CreateRenderTarget %dx%d", width, height);
    auto *target = CSM_NEW Csm::Rendering::CubismRenderTarget_Vulkan();
    target->CreateRenderTarget(g_device, (VkPhysicalDevice)info.physical_device,
        (unsigned int)width, (unsigned int)height, g_format, kDepthFormat);
    if (!target->IsValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d/vk] attach: 离屏目标创建失败 (%dx%d format=%d depth=%d)",
            width, height, (int)g_format, (int)kDepthFormat);
        CSM_DELETE(target);
        return false;
    }
    g_target = target;
    g_vk = vk;
    g_attached = true;
    return true;
}

bool bongo_cat_live2d_vulkan_ready(void) {
    return g_attached && g_target && g_target->IsValid();
}


/*
 * SDK 的 Vulkan 渲染器把模型画进离屏图像后, 图像停在 COLOR_ATTACHMENT_OPTIMAL;
 * 要被我们的合成着色器采样, 必须先转成 SHADER_READ_ONLY_OPTIMAL。
 * 这里用后端自己的命令池做一次单次提交, 与贴图上传同样的做法。
 */
static void transition_target_to_sampled(const BongoCatVkNativeInfo &info,
    VkImage image) {
    if (!info.device || !info.command_pool || !info.queue || !image) return;
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = (VkCommandPool)info.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers((VkDevice)info.device, &allocate, &command) != VK_SUCCESS)
        return;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
        vkFreeCommandBuffers((VkDevice)info.device, (VkCommandPool)info.command_pool,
            1, &command);
        return;
    }
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
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
    if (vkQueueSubmit((VkQueue)info.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS)
        vkQueueWaitIdle((VkQueue)info.queue);
    vkFreeCommandBuffers((VkDevice)info.device, (VkCommandPool)info.command_pool, 1,
        &command);
}

bool bongo_cat_live2d_vulkan_begin_frame(void) {
    if (!bongo_cat_live2d_vulkan_ready()) return false;
    auto *device_info = Csm::Rendering::CubismDeviceInfo_Vulkan::GetDeviceInfo(g_device);
    if (!device_info || !device_info->GetOffscreenManager()) return false;
    device_info->GetOffscreenManager()->BeginFrameProcess();
    VkExtent2D extent = {g_target->GetBufferWidth(), g_target->GetBufferHeight()};
    Csm::Rendering::CubismRenderer_Vulkan::SetImageExtent(extent);
    Csm::Rendering::CubismRenderer_Vulkan::SetRenderTarget(g_target->GetTextureImage(),
        g_target->GetTextureView(), g_format, extent);
    return true;
}

bool bongo_cat_live2d_vulkan_end_frame(void) {
    if (!bongo_cat_live2d_vulkan_ready()) return false;
    auto *device_info = Csm::Rendering::CubismDeviceInfo_Vulkan::GetDeviceInfo(g_device);
    if (!device_info || !device_info->GetOffscreenManager()) return false;
    device_info->GetOffscreenManager()->EndFrameProcess();
    device_info->GetOffscreenManager()->ReleaseStaleRenderTextures();
    /* 交给合成着色器之前必须换成可采样布局 */
    BongoCatVkNativeInfo native = {0};
    if (g_vk && bongo_cat_vk_native_info(g_vk, &native))
        transition_target_to_sampled(native, g_target->GetTextureImage());
    return true;
}

void bongo_cat_live2d_vulkan_detach(void) {
    if (g_target) {
        g_target->DestroyRenderTarget();
        CSM_DELETE(g_target);
        g_target = nullptr;
    }
    if (g_device != VK_NULL_HANDLE)
        Csm::Rendering::CubismDeviceInfo_Vulkan::ReleaseDeviceInfo(g_device);
    g_device = VK_NULL_HANDLE;
    g_vk = nullptr;
    g_format = VK_FORMAT_UNDEFINED;
    g_attached = false;
}

bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height) {
    if (!bongo_cat_live2d_vulkan_ready()) return false;
    if (image) *image = (void *)g_target->GetTextureImage();
    if (view) *view = (void *)g_target->GetTextureView();
    if (sampler) *sampler = (void *)g_target->GetTextureSampler();
    if (width) *width = (int)g_target->GetBufferWidth();
    if (height) *height = (int)g_target->GetBufferHeight();
    return true;
}

/* 用 attach 时记住的后端建贴图, 省掉把 BongoCatVk* 一路传进模型的管道 */
void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels, int width,
    int height) {
    if (!g_vk) return nullptr;
    return bongo_cat_live2d_vulkan_create_texture(g_vk, pixels, width, height);
}

#else /* !BONGO_CAT_HAS_VULKAN */


/* 没有编译 Vulkan 后端时, 桥一律不可用, 上层据此回退 OpenGL。 */
bool bongo_cat_live2d_vulkan_attach(struct BongoCatVk *vk, int width, int height) {
    (void)vk; (void)width; (void)height;
    return false;
}
bool bongo_cat_live2d_vulkan_ready(void) { return false; }
bool bongo_cat_live2d_vulkan_begin_frame(void) { return false; }
bool bongo_cat_live2d_vulkan_end_frame(void) { return false; }
void bongo_cat_live2d_vulkan_detach(void) {}
bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height) {
    (void)image; (void)view; (void)sampler; (void)width; (void)height;
    return false;
}

#endif
