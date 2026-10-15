#include "cubism_model.hpp"
#include "bongo_cat/gl_api.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace bongo_cat {

static size_t mask_combination_count(Csm::csmInt32 object_count,
    const Csm::csmInt32 *mask_counts, const Csm::csmInt32 *const *masks) {
    std::set<std::vector<Csm::csmInt32>> combinations;
    for (Csm::csmInt32 i = 0; i < object_count; ++i) {
        if (mask_counts[i] <= 0) continue;
        std::vector<Csm::csmInt32> ids(masks[i], masks[i] + mask_counts[i]);
        // Cubism shares a clipping context for the same IDs in any order.
        std::sort(ids.begin(), ids.end());
        combinations.insert(std::move(ids));
    }
    // Include hidden objects so expressions cannot exceed the allocated capacity.
    return combinations.size();
}

/* 一张遮罩纹理最多容纳 ClippingMaskMaxCountOnDefault 个裁剪上下文, 超出就按
 * MultiRenderTexture 那一档再开缓冲。组合数在 mask_combination_count() 里已经按
 * "排序去重后的 ID 集合"算过 (且把隐藏对象也算进去), 所以这里只做除法就够了。 */
static Csm::csmInt32 mask_buffer_count(size_t count) {
    if (count <= (size_t)Csm::Rendering::ClippingMaskMaxCountOnDefault) return 1;
    const size_t capacity = Csm::Rendering::ClippingMaskMaxCountOnMultiRenderTexture;
    return (Csm::csmInt32)(1 + (count - 1) / capacity);
}

void NativeModel::bind_textures() {
    /* Vulkan: SDK 的 Vulkan 渲染器收 CubismImageVulkan, 类型细节留在桥里 */
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat_cubism_renderer_uses_vulkan()) {
        auto *vulkan = GetRenderer<Csm::Rendering::CubismRenderer_Vulkan>();
        if (!vulkan) return;
        for (void *texture : vulkan_textures_)
            if (texture)
                bongo_cat_live2d_vulkan_bind_texture((void *)vulkan, texture);
        /*
         * Vulkan 分支同样要把"贴图已经预乘"这件事告诉渲染器。SDK 的 Vulkan 渲染器按
         * 这个标志在两组着色器里选 (CubismRenderer_Vulkan.cpp 的 isPremultipliedAlpha
         * -> ShaderNames_*PremultipliedAlpha):
         *   * 未设置 (默认 false): 直通 alpha 的 FragShaderSrc, 片元末尾再做一次
         *     color.rgb * color.a —— 而贴图在 cubism_vulkan_texture.cpp 里已经预乘过,
         *     于是半透明像素变成 color*a^2, 模型边缘整体偏暗;
         *   * 设置之后: FragShaderSrcPremultipliedAlpha, 片元不再乘 alpha, 与 GL 分支
         *     下面那一行完全同款。
         * 漏掉这一行的症状很隐蔽: 覆盖 (alpha) 完全一致, 只有 RGB 在半透明处偏暗,
         * 看起来就像"某个 artmesh 多了一圈暗描边"。
         */
        vulkan->IsPremultipliedAlpha(true);
        return;
    }
#endif
    auto *renderer = GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer) return;
    for (size_t i = 0; i < textures_.size(); ++i)
        if (textures_[i])
            renderer->BindTexture((Csm::csmInt32)i, textures_[i]);
    /* 贴图在 image.c 的 upload() 里已经就地预乘, 这里只是把这一事实告诉 SDK 渲染器,
     * 让它按预乘混合。Vulkan 分支在上面用同一个标志声明同一件事。 */
    renderer->IsPremultipliedAlpha(true);
}

void NativeModel::release_textures() {
    /* 分步加载可能在"位图已解码、掩码还没算"的中间态被打断 (用户又点了一个模型, 或者加载
     * 失败): 那幅 CPU 位图必须在这里一起还掉, 否则每一次失败的切换都会漏一张贴图的内存。 */
    if (pending_alpha_image_.pixels) {
        bongo_cat_image_free(&pending_alpha_image_);
        pending_alpha_image_ = BongoCatImage{};
    }
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat_cubism_renderer_uses_vulkan()) {
        for (void *texture : vulkan_textures_)
            bongo_cat_live2d_vulkan_destroy_texture(texture);
        vulkan_textures_.clear();
        texture_alpha_.clear();
        triangle_alpha_.clear();
        return;
    }
#endif
    if (!textures_.empty())
        glDeleteTextures((GLsizei)textures_.size(), textures_.data());
    textures_.clear();
    texture_alpha_.clear();
    triangle_alpha_.clear();
}

void NativeModel::release_renderer() {
    DeleteRenderer();
#ifdef CSM_TARGET_MAC_GL
    core_buffers_.release();
#endif
    /* 把尺寸与遮罩记账全部清零: 下一次 create_renderer() 会重新探测纹理上限、重算预算
     * 与网格划分。留着旧值会让"换模型/换后端"复用上一套遮罩假设。 */
    renderer_width_ = 0;
    renderer_height_ = 0;
    mask_texture_limit_ = 0;
    mask_buffer_size_ = 0;
    mask_layout_divisions_ = 1;
    mask_last_width_ = mask_last_height_ = 0;
}

void NativeModel::release_render_resources() {
    release_textures();
    release_renderer();
}

/*
 * 只有 OpenGL 路径会用到: Vulkan 分支的遮罩缓冲尺寸在 create_renderer() 里一次性设好
 * (见那里的说明)。这里的记忆值让每帧调用退化成一次比较, 真正的重建发生在窗口尺寸变化时。
 */
void NativeModel::update_mask_buffers() {
    if (width_ == mask_last_width_ && height_ == mask_last_height_) return;
    auto *renderer = GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer || !_model || mask_texture_limit_ <= 0) return;
    mask_last_width_ = width_;
    mask_last_height_ = height_;
    const bool drawable_masks = _model->IsUsingMasking();
    const bool offscreen_masks = _model->IsUsingMaskingForOffscreen();
    if (!drawable_masks && !offscreen_masks) return;

    // Account for the actual atlas subdivision, including hidden masks.
    const int extent = std::min(std::max(width_, height_), mask_texture_limit_);
    const int requested = std::max(512, mask_layout_divisions_ * extent);
    const int size = std::min(mask_texture_limit_, ((requested + 511) / 512) * 512);
    if (size == mask_buffer_size_) return;
    // Grow immediately, but avoid grow/shrink churn near a size boundary.
    if (size < mask_buffer_size_ && requested > mask_buffer_size_ - 768) return;

    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    auto prepare = [size, &previous_texture](
        Csm::Rendering::CubismRenderTarget_OpenGLES2 *buffer) {
        const bool was_bound = previous_texture != 0 &&
            (GLuint)previous_texture == buffer->GetColorBuffer();
        // Allocate now so the SDK cannot replace the filtering on first draw.
        buffer->CreateRenderTarget((Csm::csmUint32)size, (Csm::csmUint32)size);
        if (was_bound) previous_texture = (GLint)buffer->GetColorBuffer();
        glBindTexture(GL_TEXTURE_2D, buffer->GetColorBuffer());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    if (drawable_masks) {
        renderer->SetDrawableClippingMaskBufferSize((float)size, (float)size);
        for (int i = 0; i < renderer->GetDrawableRenderTextureCount(); ++i)
            prepare(renderer->GetDrawableMaskBuffer(i));
    }
    if (offscreen_masks) {
        renderer->SetOffscreenClippingMaskBufferSize((float)size, (float)size);
        for (int i = 0; i < renderer->GetOffscreenRenderTextureCount(); ++i)
            prepare(renderer->GetOffscreenMaskBuffer(i));
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    mask_buffer_size_ = size;
}

bool NativeModel::create_renderer(BongoCatError *error) {
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[live2d/vk] create_renderer: uses_vulkan=%d bridge_ready=%d",
        (int)bongo_cat_cubism_renderer_uses_vulkan(),
        (int)bongo_cat_live2d_vulkan_ready());
    /*
     * Vulkan: 设备与离屏目标由桥在窗口创建时准备好 (attach), 这里只确认它就绪;
     * GL 那一套 context / 错误检查 / 遮罩尺寸查询全部跳过。
     */
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat_cubism_renderer_uses_vulkan()) {
        /* 框架已经启动, 现在才能安全地让 SDK 建它的设备表 */
        if (!bongo_cat_live2d_vulkan_ready())
            bongo_cat_live2d_vulkan_attach(NULL, 0, 0);
        if (!bongo_cat_live2d_vulkan_ready()) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Live2D Vulkan renderer is not attached");
            return false;
        }
        const size_t drawable = mask_combination_count(_model->GetDrawableCount(),
            _model->GetDrawableMaskCounts(), _model->GetDrawableMasks());
        const size_t offscreen = mask_combination_count(_model->GetOffscreenCount(),
            _model->GetOffscreenMaskCounts(), _model->GetOffscreenMasks());
        const Csm::csmInt32 buffers = std::max(mask_buffer_count(drawable),
            mask_buffer_count(offscreen));
        CreateRenderer((Csm::csmUint32)width_, (Csm::csmUint32)height_, buffers);
        auto *vulkan = GetRenderer<Csm::Rendering::CubismRenderer_Vulkan>();
        if (!vulkan) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
                "Cannot create the Live2D Vulkan renderer");
            release_renderer();
            return false;
        }
        /*
         * 遮罩缓冲尺寸必须显式设置。GL 路径在 update_mask_buffers() 里做这件事,
         * 而我的 Vulkan 分支当初整段跳过了它, 于是 SDK 的 Vulkan 裁剪管理器按
         * 未设置的尺寸工作, 在 SetupClippingContext 里越界写 (0xC0000409)。
         * 这里先按模型画布尺寸设置, 与 GL 路径同一个量级。
         */
        /*
         * 尺寸必须与 GL 路径同款: 正方形 + 512 对齐。裁剪管理器会把多个裁剪区域
         * 按网格排布进一张遮罩纹理, 非正方形/未对齐的尺寸会让布局计算越界
         * (之前传 width_ x height_ = 612x354 就是这个毛病)。
         */
        const int mask_extent = std::max(width_, height_);
        const int mask_requested = std::max(512, mask_extent);
        const int mask_size = std::min(2048, ((mask_requested + 511) / 512) * 512);
        vulkan->SetClippingMaskBufferSize((Csm::csmFloat32)mask_size,
            (Csm::csmFloat32)mask_size);
        /*
         * SDK 样例在 CreateRenderer 之后还会单独建这三样 (命令缓冲/顶点/索引)。
         * 我原先漏了, 于是 SDK 内部用于绘制的缓冲是空的 —— 它在
         * CubismRenderTarget_Vulkan::BeginDraw 里按这些缓冲记账时越界
         * (校验层拖慢后稳定复现, cdb 栈已确认)。
         */
        vulkan->CreateCommandBuffer();
        vulkan->CreateVertexBuffer();
        vulkan->CreateIndexBuffer();
        bind_textures();
        return true;
    }
#endif
    if (!SDL_GL_GetCurrentContext()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot create the Live2D renderer without an OpenGL context");
        return false;
    }
    if (!bongo_cat_gl_clear_errors()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Cannot clear the OpenGL error state before creating the Live2D renderer");
        return false;
    }
#ifdef CSM_TARGET_MAC_GL
    CoreProfileBinding binding(core_buffers_);
#endif
    const size_t drawable_combinations = mask_combination_count(
        _model->GetDrawableCount(), _model->GetDrawableMaskCounts(),
        _model->GetDrawableMasks());
    const size_t offscreen_combinations = mask_combination_count(
        _model->GetOffscreenCount(), _model->GetOffscreenMaskCounts(),
        _model->GetOffscreenMasks());
    const Csm::csmInt32 buffer_count = std::max(
        mask_buffer_count(drawable_combinations), mask_buffer_count(offscreen_combinations));
    /*
     * 有些机器上模型创建早于窗口尺寸生效, width_/height_ 会是 0: SDK 用 0 尺寸建
     * 遮罩帧缓冲会直接抛 GL_INVALID_VALUE (0x501), 表现就是"猫不见了"。这里兜底
     * 用窗口像素尺寸, 再不行用 1x1; 窗口尺寸变化后遮罩会按新尺寸重建。
     */
    if (width_ <= 0 || height_ <= 0) {
        int pixel_width = 0, pixel_height = 0;
        SDL_Window *window = SDL_GL_GetCurrentWindow();
        if (window) SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
        width_ = pixel_width > 0 ? pixel_width : 1;
        height_ = pixel_height > 0 ? pixel_height : 1;
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d] 创建渲染器时尺寸无效, 兜底用 %dx%d", width_, height_);
    }
    /*
     * 排查探针: 先把"创建之前"就积压的 GL 错误排空并逐条记下来。glGetError 只返回第一条
     * 并清空队列, 不排空的话下面 CreateRenderer 之后的检查会把更早的陈旧错误误记成
     * "CreateRenderer 出错" —— 0x501 到底出自哪一步, 就靠这两行区分。
     */
    for (GLenum stale = glGetError(); stale != GL_NO_ERROR; stale = glGetError())
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d] CreateRenderer 之前已积压 GL 错误: 0x%x", (unsigned)stale);
    CreateRenderer((Csm::csmUint32)width_, (Csm::csmUint32)height_,
        buffer_count);
    const GLenum create_error = glGetError();
    if (create_error != GL_NO_ERROR)
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d] CreateRenderer(%dx%d buffers=%d) 之后 gl=0x%x",
            width_, height_, (int)buffer_count, (unsigned)create_error);
    auto *renderer = GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    if (renderer) {
        GLint texture_limit = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture_limit);
        mask_texture_limit_ = std::min(4096, (int)texture_limit);
        const size_t per_buffer = (std::max(drawable_combinations,
            offscreen_combinations) + (size_t)buffer_count - 1) / (size_t)buffer_count;
        /* 遮罩图集把多个裁剪区域按网格排进一张纹理, 每张缓冲要装下的裁剪组合数决定
         * 网格分几格: 格子太少时 SDK 会自动多开缓冲, 组合很少时开大格反而浪费。 */
        mask_layout_divisions_ = per_buffer <= 4 ? 1 : (per_buffer <= 16 ? 2 : 3);
        const uint64_t total_buffers = (uint64_t)buffer_count *
            ((drawable_combinations > 0 ? 1u : 0u) +
             (offscreen_combinations > 0 ? 1u : 0u));
        /*
         * 遮罩预算: 原来按 32 MiB RGBA8 给 (两个模型就是 64 MB 级), 遮罩其实只要
         * 一个通道, 8 MiB 足够 —— 遮罩图集放不下时 SDK 会自动多开几个缓冲, 视觉
         * 结果不变。有 BONGO_CAT_MASK_BUDGET_MB 可以临时调回去做对比。
         */
        uint64_t mask_budget = 8ull * 1024 * 1024;
        if (const char *override_value = SDL_getenv("BONGO_CAT_MASK_BUDGET_MB")) {
            long megabytes = strtol(override_value, nullptr, 10);
            if (megabytes > 0 && megabytes <= 512)
                mask_budget = (uint64_t)megabytes * 1024 * 1024;
        }
        while (mask_texture_limit_ > 256 && total_buffers * 4 *
            (uint64_t)mask_texture_limit_ * (uint64_t)mask_texture_limit_ > mask_budget)
            mask_texture_limit_ = std::max(256, mask_texture_limit_ - 256);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 遮罩预算=%lluMB 每张=%dpx 缓冲=%llu 组=%d", 
            (unsigned long long)(mask_budget / (1024 * 1024)), mask_texture_limit_,
            (unsigned long long)total_buffers, mask_layout_divisions_);
        bind_textures();
        update_mask_buffers();
        const GLenum mask_error = glGetError();
        if (mask_error != GL_NO_ERROR)
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[live2d] 遮罩缓冲 (limit=%d divisions=%d buffers=%d) 之后 gl=0x%x",
                mask_texture_limit_, mask_layout_divisions_, (int)buffer_count,
                (unsigned)mask_error);
    }
    GLenum renderer_error = glGetError();
    if (renderer && renderer_error == GL_NO_ERROR) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
        "Cannot create the Live2D renderer (OpenGL 0x%x)",
        (unsigned)renderer_error);
    release_renderer();
    return false;
}

} // namespace bongo_cat
