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

#include <Rendering/Vulkan/CubismDeviceInfo_Vulkan.hpp>
#include <Rendering/Vulkan/CubismRenderTarget_Vulkan.hpp>
#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>

namespace {

bool g_attached = false;
BongoCatVk *g_vk = nullptr;
Csm::Rendering::CubismRenderTarget_Vulkan *g_target = nullptr;
VkDevice g_device = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;

/* 24 位深度 + 模板在桌面 Vulkan 上普遍可用, SDK 的样例也用这一档 */
const VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

} // namespace

bool bongo_cat_live2d_vulkan_attach(struct BongoCatVk *vk, int width, int height) {
    if (g_attached) return true;
    if (!vk || width <= 0 || height <= 0) return false;
    BongoCatVkNativeInfo info = {0};
    if (!bongo_cat_vk_native_info(vk, &info) || !info.device || !info.physical_device ||
        !info.command_pool || !info.queue)
        return false;
    g_device = (VkDevice)info.device;
    g_format = (VkFormat)info.image_format;
    if (!Csm::Rendering::CubismDeviceInfo_Vulkan::GetDeviceInfo(g_device)) return false;

    auto *target = CSM_NEW Csm::Rendering::CubismRenderTarget_Vulkan();
    target->CreateRenderTarget(g_device, (VkPhysicalDevice)info.physical_device,
        (unsigned int)width, (unsigned int)height, g_format, kDepthFormat);
    if (!target->IsValid()) {
        CSM_DELETE(target);
        return false;
    }
    VkExtent2D extent = {(unsigned int)width, (unsigned int)height};
    Csm::Rendering::CubismRenderer_Vulkan::SetConstantSettings(g_device,
        (VkPhysicalDevice)info.physical_device, (VkCommandPool)info.command_pool,
        (VkQueue)info.queue, (unsigned int)info.image_count, extent,
        target->GetTextureView(), g_format, kDepthFormat);
    Csm::Rendering::CubismRenderer_Vulkan::EnableChangeRenderTarget();
    g_target = target;
    g_vk = vk;
    g_attached = true;
    return true;
}

bool bongo_cat_live2d_vulkan_ready(void) {
    return g_attached && g_target && g_target->IsValid();
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
