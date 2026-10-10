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
#include "live2d_vulkan_internal.h"

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
/* 窗口创建时记下的内嵌资源根目录 (assets/), 里面也有一份 FrameworkShaders */
std::string g_assets_root;
Csm::Rendering::CubismRenderTarget_Vulkan *g_target = nullptr;
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;

/* SDK 打开的第一个着色器文件, 用它判断一份 FrameworkShaders 是否可用 */
const char *kShaderProbe = "FrameworkShaders/FragShaderSrc.spv";

bool has_shader_probe(const std::filesystem::path &parent) {
    if (parent.empty()) return false;
    std::error_code error;
    return std::filesystem::exists(parent / kShaderProbe, error);
}

/* 着色器父目录: exe 目录 -> 内嵌资源目录 (只复制 exe 时资源是从 pak 解出来的) */
std::filesystem::path shader_parent_directory() {
    std::filesystem::path base = SDL_GetBasePath() ? SDL_GetBasePath() : "";
    if (has_shader_probe(base)) return base;
    if (!g_assets_root.empty() && has_shader_probe(g_assets_root))
        return std::filesystem::path(g_assets_root);
    return {};
}

/* 24 位深度 + 模板在桌面 Vulkan 上普遍可用, SDK 的样例也用这一档 */
const VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
/* 见 live2d_vulkan_internal.h: SDK 的 Vulkan 渲染器开不了 MSAA, 只能超采样 */
/* 实际倍数见 bongo_cat_live2d_vulkan_supersample_for (运行时档位 + 自动降级) */
/* 目标尺寸 (超采样之后) 与请求尺寸 (窗口/内容尺寸) 的记忆值 */
int g_target_width = 0;
int g_target_height = 0;

VkExtent2D target_extent(int width, int height) {
    const int factor = bongo_cat_live2d_vulkan_supersample_for(width, height);
    VkExtent2D extent = {(unsigned int)(width * factor),
        (unsigned int)(height * factor)};
    return extent;
}

/* 离屏目标跟着窗口尺寸走: 尺寸变了就重建 (重建前等设备空, 免得还有在飞的采样) */
bool ensure_target_size(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (g_target && g_target_width == width && g_target_height == height)
        return true;
    VkExtent2D extent = target_extent(width, height);
    auto *target = CSM_NEW Csm::Rendering::CubismRenderTarget_Vulkan();
    target->CreateRenderTarget(g_device, g_physical, extent.width,
        extent.height, g_format, kDepthFormat);
    if (!target->IsValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d/vk] 离屏目标重建失败 (%dx%d)", width, height);
        CSM_DELETE(target);
        return false;
    }
    if (g_target) {
        if (g_device) vkDeviceWaitIdle(g_device);
        g_target->DestroyRenderTarget();
        CSM_DELETE(g_target);
    }
    g_target = target;
    g_target_width = width;
    g_target_height = height;
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] 离屏目标跟随窗口尺寸: %dx%d (超采样 %dx%d)",
        width, height, (int)extent.width, (int)extent.height);
    return true;
}

} // namespace

int bongo_cat_live2d_vulkan_supersample_mode = 0;

/* 自动档: 2x 是 4 倍面积, 大窗口白吃显存, 超过 1.2M 像素就退回 1x */
int bongo_cat_live2d_vulkan_supersample_for(int width, int height) {
    if (bongo_cat_live2d_vulkan_supersample_mode == 1)
        return BONGO_CAT_LIVE2D_VULKAN_SUPERSAMPLE;
    if (bongo_cat_live2d_vulkan_supersample_mode == 2) return 1;
    return (long)width * (long)height > 1200000L ? 1
        : BONGO_CAT_LIVE2D_VULKAN_SUPERSAMPLE;
}

void bongo_cat_live2d_vulkan_set_supersample(int mode) {
    bongo_cat_live2d_vulkan_supersample_mode =
        mode == 1 || mode == 2 ? mode : 0;
}

bool bongo_cat_live2d_vulkan_shaders_available(const char *assets_root) {
    if (!shader_parent_directory().empty()) return true;
    return assets_root && assets_root[0] && has_shader_probe(assets_root);
}

void bongo_cat_live2d_vulkan_set_shader_root(const char *assets_root) {
    g_assets_root = assets_root ? assets_root : "";
}

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
    g_physical = (VkPhysicalDevice)info.physical_device;
    g_format = (VkFormat)info.image_format;

    VkExtent2D extent = {(unsigned int)width, (unsigned int)height};
    /*
     * 顺序按 SDK 示例: 先 SetConstantSettings (它建管线/描述符), 之后才能建离屏
     * 目标。着色器按相对路径打开, 所以先把工作目录切到能看见 FrameworkShaders 的
     * 地方 (exe 目录或解出来的内嵌资源目录)。
     */
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: Step 1 SetConstantSettings (view=%p extent=%dx%d "
        "format=%d depth=%d images=%d)",
        (void *)info.image_view, width, height, (int)g_format,
        (int)kDepthFormat, info.image_count);
    {
        std::filesystem::path parent = shader_parent_directory();
        if (!parent.empty()) {
            std::error_code error_code;
            std::filesystem::current_path(parent, error_code);
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[live2d/vk] attach: 着色器目录=%s",
                std::filesystem::current_path().string().c_str());
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[live2d/vk] attach: 找不到 FrameworkShaders (exe 目录与内嵌资源里都没有)");
        }
    }
    Csm::Rendering::CubismRenderer_Vulkan::SetConstantSettings(g_device,
        (VkPhysicalDevice)info.physical_device, (VkCommandPool)info.command_pool,
        (VkQueue)info.queue, (unsigned int)info.image_count, extent,
        /*
         * 默认目标视图传 NULL: 我们每帧都用 SetRenderTarget 指向自己的离屏
         * 图像 (配合 EnableChangeRenderTarget), 从不通过 Cubism 渲染到交换链。
         * 之前传的交换链视图在本环境里是无效句柄, SDK 用它建 framebuffer 时会崩。
         */
        VK_NULL_HANDLE, g_format, kDepthFormat);
    Csm::Rendering::CubismRenderer_Vulkan::EnableChangeRenderTarget();
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: Step 2 SetConstantSettings 返回");
    /* GetDeviceInfo 内部会建近 500 条管线并加载 .spv, 必须在静态设置之后调用 */
    if (!Csm::Rendering::CubismDeviceInfo_Vulkan::GetDeviceInfo(g_device)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[live2d/vk] attach: GetDeviceInfo 失败");
        return false;
    }
    VkExtent2D target_size = target_extent(width, height);
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: Step 3 CreateRenderTarget %dx%d (超采样 %dx%d)",
        width, height, (int)target_size.width, (int)target_size.height);
    auto *target = CSM_NEW Csm::Rendering::CubismRenderTarget_Vulkan();
    target->CreateRenderTarget(g_device, (VkPhysicalDevice)info.physical_device,
        target_size.width, target_size.height, g_format, kDepthFormat);
    if (!target->IsValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d/vk] attach: 离屏目标创建失败 (%dx%d format=%d depth=%d)",
            width, height, (int)g_format, (int)kDepthFormat);
        CSM_DELETE(target);
        return false;
    }
    g_target = target;
    g_target_width = width;
    g_target_height = height;
    g_vk = vk;
    g_attached = true;
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] attach: 完成 目标=%dx%d device=%p", width, height,
        (void *)g_device);
    return true;
}

bool bongo_cat_live2d_vulkan_ready(void) {
    return g_attached && g_target && g_target->IsValid();
}

bool bongo_cat_live2d_vulkan_begin_frame(void) {
    if (!bongo_cat_live2d_vulkan_ready()) return false;
    auto *device_info = Csm::Rendering::CubismDeviceInfo_Vulkan::GetDeviceInfo(g_device);
    if (!device_info || !device_info->GetOffscreenManager()) return false;
    BongoCatVkNativeInfo current = {0};
    if (!g_vk || !bongo_cat_vk_native_info(g_vk, &current)) return false;
    /* 窗口尺寸变了就重建离屏目标, 模型才会跟着"缩放"一起变 */
    if (!ensure_target_size(current.width, current.height)) return false;
    device_info->GetOffscreenManager()->BeginFrameProcess();
    VkExtent2D extent = {g_target->GetBufferWidth(), g_target->GetBufferHeight()};
    /*
     * 每帧刷新 SDK 的静态设置。窗口改尺寸会重建我们的交换链 (新的 imageView),
     * 而 SDK 把这些值缓存在静态变量里 —— 只在 attach 时设过一次的话, resize 之后
     * 它仍按旧 extent/旧 imageView 记账, 在遮罩目标 BeginDraw 里越界
     * (校验层拖慢时稳定复现)。SetConstantSettings 只是纯赋值, 每帧调用开销可忽略。
     */
    Csm::Rendering::CubismRenderer_Vulkan::SetConstantSettings(
        (VkDevice)current.device, (VkPhysicalDevice)current.physical_device,
        (VkCommandPool)current.command_pool, (VkQueue)current.queue,
        (unsigned int)current.image_count, extent,
        (VkImageView)current.image_view, (VkFormat)current.image_format,
        kDepthFormat);
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
    if (g_vk && bongo_cat_vk_native_info(g_vk, &native)) {
        bongo_cat_live2d_vulkan_transition_sampled(&native,
            (void *)g_target->GetTextureImage());
        /*
         * SDK 的 Vulkan 渲染器自己提交命令 (DrawModel / 遮罩), 而我们的帧随后要采样
         * 这张离屏图像。开校验层时曾出现 ~2 秒后的崩溃, 像是提交与采样之间的竞态,
         * 所以这里显式把设备等空, 让"SDK 提交"与"我们录制帧"完全串行。
         */
        if (native.device) vkDeviceWaitIdle((VkDevice)native.device);
    }
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
    g_physical = VK_NULL_HANDLE;
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
