/*
 * OpenGL 侧的图像上传与合成。
 *
 * 这里所有返回纹理名的接口都要求调用线程当前有可用的 GL 上下文; Vulkan 后端不适用 (它走
 * vk_backend 的纹理接口)。而纯 CPU 的 load/fit/composite 两条路径都能用 —— 这个分层见
 * include/bongo_cat/image.h 的说明。
 * 模块内部三层: 解码 (image_decode.c) -> CPU 处理 (alpha/mipmap/resize) -> 上传 (本文件)。
 */
#include "bongo_cat/gl_api.h"
#include "bongo_cat/image.h"
#include "image_internal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stb_image.h>
#include <stdlib.h>
#include <string.h>

#define BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT BONGO_CAT_LIVE2D_TEXTURE_LIMIT

/*
 * 上传前 CPU 缓冲的诊断探针 (BONGO_CAT_TEST_DECODE_PROBE=1 才工作, 默认只多一次缓存过的
 * getenv 判断)。统计量选得少而稳: 尺寸 + 滚动字节和 (对任意一个字节差异都敏感) + alpha 直方图
 * 12 桶, 按"直通 alpha"口径算 —— 也就是上传前的原始像素。
 *
 * 存在的理由: GL 与 VK 的解码入口不是同一个函数 (GL 走 bongo_cat_image_texture_model,
 * VK 走 bongo_cat_image_fit), 一旦别处再改解码参数, 两个后端可能在 CPU 侧就分叉, 而症状
 * 会伪装成"渲染差异"。两端各跑一次、比这三行数字, 就能一眼分清是解码还是 GPU 侧。
 * 实测依据: standard 模型三张贴图两端完全一致 (sum 0x6aaac59fe065af03 / 0xb65a647b1b9cecd9
 * / 0x24bf5a7cfa84a836), 所以颜色差异只在 GPU 侧, 与解码无关。
 */
void bongo_cat_image_log_upload_probe(const BongoCatImage *image, const char *where) {
    static int enabled = -1;
    if (enabled < 0) enabled = SDL_getenv("BONGO_CAT_TEST_DECODE_PROBE") ? 1 : 0;
    if (!enabled || !image || !image->pixels || image->width <= 0 || image->height <= 0)
        return;
    unsigned alpha_bins[12] = {0};
    unsigned long long sum = 0, r = 0, g = 0, b = 0, a = 0;
    const size_t count = (size_t)image->width * (size_t)image->height;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *pixel = image->pixels + i * 4;
        for (int c = 0; c < 3; ++c) sum = (sum * 131u + pixel[c]) & 0xffffffffffffffffull;
        sum = (sum * 131u + pixel[3]) & 0xffffffffffffffffull;
        r += pixel[0]; g += pixel[1]; b += pixel[2]; a += pixel[3];
        alpha_bins[pixel[3] * 12u / 256u]++;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "DECODE-PROBE %s: %dx%d sum=0x%016llx mean_rgba=%.2f,%.2f,%.2f,%.2f "
        "alpha_bins(alpha/256*12)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
        where ? where : "?", image->width, image->height, sum,
        (double)r / count, (double)g / count, (double)b / count, (double)a / count,
        alpha_bins[0], alpha_bins[1], alpha_bins[2], alpha_bins[3], alpha_bins[4],
        alpha_bins[5], alpha_bins[6], alpha_bins[7], alpha_bins[8], alpha_bins[9],
        alpha_bins[10], alpha_bins[11]);
}

BongoCatResult bongo_cat_image_load(const char *path, BongoCatImage *image, BongoCatError *error) {
    BongoCatResult result = bongo_cat_image_decode_pixels(path, image, error);
    if (result != BONGO_CAT_OK) return result;
    image->surface = SDL_CreateSurfaceFrom(image->width, image->height,
        SDL_PIXELFORMAT_RGBA32, image->pixels, image->width * 4);
    if (!image->surface) {
        bongo_cat_image_free(image);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Cannot create image surface");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    return BONGO_CAT_OK;
}
/* pixels_stbi 记住该用哪个分配器: stb 有自己的分配器, 和 free() 不能混用 —— 混了会在部分
 * 平台上直接崩 (不是同一个 CRT 堆)。 */
void bongo_cat_image_free(BongoCatImage *image) {
    if (!image) return;
    if (image->surface) SDL_DestroySurface(image->surface);
    if (image->pixels) {
        if (image->pixels_stbi) stbi_image_free(image->pixels);
        else free(image->pixels);
    }
    memset(image, 0, sizeof(*image));
}
/*
 * 模型贴图的两个诊断开关 (默认都保持现状, 只在 A/B 排查时用):
 *
 *   BONGO_CAT_GL_MODEL_MIP=0   跳过 mip 链, 模型贴图退回 GL_LINEAR —— 也就是"只采 level 0"
 *                              的采样口径, 用来复现 Vulkan 侧 (mipLevels=1) 的滤波行为,
 *                              判断 GL/VK 的差异到底是不是"mip+各向异性 vs 单级线性"。
 *   BONGO_CAT_GL_ANISOTROPY=1  强制各向异性上限 = 1, 关掉 GL_EXT_texture_filter_anisotropic
 *                              的收益, 用来单独隔离各向异性的贡献。
 *
 * 两者都不改变默认行为: 不设变量 (或设成别的值) 时走原来的代码路径。
 */
static bool model_mips_enabled(void) {
    const char *value = SDL_getenv("BONGO_CAT_GL_MODEL_MIP");
    return !(value && value[0] == '0' && value[1] == '\0');
}

static bool model_anisotropy_enabled(void) {
    const char *value = SDL_getenv("BONGO_CAT_GL_ANISOTROPY");
    return !(value && value[0] == '1' && value[1] == '\0');
}

static unsigned int upload(const BongoCatImage *image, GLuint texture,
    bool model_texture) {
    /* Premultiply in place before any GPU filtering. Cubism must use its
     * premultiplied shader for these model textures, including fallback. */
    bongo_cat_image_log_upload_probe(image, model_texture ? "gl-model-texture" : "gl-texture");
    if (model_texture) {
        size_t count = (size_t)image->width * image->height;
        for (size_t i = 0; i < count; ++i) {
            unsigned char *pixel = image->pixels + i * 4;
            for (int c = 0; c < 3; ++c)
                pixel[c] = (unsigned char)((pixel[c] * pixel[3] + 127) / 255);
        }
    }
    bool created = texture == 0;
    /* 只用于下面那行诊断日志: 记下"实际生效"的各向异性上限, 免得再查一次扩展枚举
     * (没有 GL_EXT_texture_filter_anisotropic 时那次查询会留下一个 GL_INVALID_ENUM,
     * 污染后面的错误检查)。 */
    GLfloat applied_anisotropy = 1.0f;
    if (created) glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (created) {
        /* Keep direct image/UI textures on the inexpensive linear path. Model
         * textures switch to alpha-safe trilinear filtering after upload. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* 模型图集是 1024px 级别、实际只画几百像素, 缩小倍率很大: 没有 mip 链会闪, 斜视时还会
         * 出摩尔纹。各向异性上限取 8 是收益与开销的折中 (再高对 2D 图集没有明显收益)。 */
        if (model_texture && model_anisotropy_enabled() && SDL_GL_ExtensionSupported(
            "GL_EXT_texture_filter_anisotropic")) {
            GLfloat maximum = 1.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
            if (maximum > 1.0f) {
                applied_anisotropy = SDL_min(maximum, 8.0f);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                    applied_anisotropy);
            }
        }
    }
    /* 行本身是紧打包的 RGBA (宽度*4 天然 4 字节对齐), 设 1 只是为了不受外部遗留状态影响;
     * 结束时恢复成 4, 免得把状态泄漏给同进程里的其它上传者。 */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (created && model_texture && model_mips_enabled()) {
        if (!bongo_cat_image_upload_mipmaps(image)) {
            bongo_cat_gl_clear_errors();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width,
                image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
        }
    } else if (created) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width,
            image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->width, image->height,
            GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    /* 只有显式设了诊断开关才记一行, 正常运行的日志里不会多出噪声。 */
    if (model_texture && (SDL_getenv("BONGO_CAT_GL_MODEL_MIP") ||
        SDL_getenv("BONGO_CAT_GL_ANISOTROPY"))) {
        GLint min_filter = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[gl] 模型贴图 %dx%d mip=%d min_filter=0x%x anisotropy=%.1f",
            image->width, image->height, model_mips_enabled() ? 1 : 0,
            (unsigned)min_filter, (double)applied_anisotropy);
    }
    return texture;
}
unsigned int bongo_cat_image_texture(const char *path, int *width, int *height, BongoCatError *error) {
    BongoCatImage image;
    if (bongo_cat_image_load(path, &image, error) != BONGO_CAT_OK) return 0;
    GLuint texture = upload(&image, 0, false);
    if (width) *width = image.width;
    if (height) *height = image.height;
    bongo_cat_image_free(&image);
    return texture;
}

unsigned int bongo_cat_image_upload_texture(const BongoCatImage *image,
    unsigned int texture, bool premultiply) {
    if (!image || image->width <= 0 || image->height <= 0) return 0;
    return upload(image, (GLuint)texture, premultiply);
}

void bongo_cat_image_release_texture(unsigned int texture) {
    if (!texture) return;
    GLuint name = (GLuint)texture;
    glDeleteTextures(1, &name);
}

/* Windows 上先用 WIC 直接解到目标尺寸 (省掉一次全尺寸解码), 失败再退回 stb + SDL_ScaleSurface。
 * 只有确实超限时才缩放; 缩不出来时原样返回, 这不算失败。 */
BongoCatResult bongo_cat_image_fit(const char *path, int max_width,
    int max_height, BongoCatImage *out, BongoCatError *error) {
    if (!path || !out) return BONGO_CAT_ERROR_ARGUMENT;
    BongoCatImage image = {0};
#ifdef _WIN32
    if (max_width > 0 && max_height > 0 &&
        bongo_cat_image_decode_wic_responsive(path, &image, max_width, max_height,
            NULL, NULL)) {
        *out = image;
        return BONGO_CAT_OK;
    }
#endif
    BongoCatResult result = bongo_cat_image_load(path, &image, error);
    if (result != BONGO_CAT_OK) return result;
    if (max_width > 0 && max_height > 0 &&
        (image.width > max_width || image.height > max_height) && image.surface) {
        float scale = SDL_min((float)max_width / image.width,
            (float)max_height / image.height);
        int target_width = SDL_max(1, (int)(image.width * scale + .5f));
        int target_height = SDL_max(1, (int)(image.height * scale + .5f));
        SDL_Surface *scaled = SDL_ScaleSurface(image.surface, target_width,
            target_height, SDL_SCALEMODE_LINEAR);
        if (scaled) {
            size_t bytes = (size_t)target_width * (size_t)target_height * 4u;
            unsigned char *pixels = malloc(bytes);
            if (pixels) {
                memcpy(pixels, scaled->pixels, bytes);
                SDL_DestroySurface(scaled);
                bongo_cat_image_free(&image);
                memset(out, 0, sizeof(*out));
                out->pixels = pixels;
                out->width = target_width;
                out->height = target_height;
                out->surface = SDL_CreateSurfaceFrom(target_width, target_height,
                    SDL_PIXELFORMAT_RGBA32, pixels, target_width * 4);
                return BONGO_CAT_OK;
            }
            SDL_DestroySurface(scaled);
        }
    }
    *out = image;
    return BONGO_CAT_OK;
}

unsigned int bongo_cat_image_texture_thumbnail(const char *path, int max_width,
    int max_height, int *width, int *height, BongoCatError *error) {
    BongoCatImage image = {0};
    if (bongo_cat_image_fit(path, max_width, max_height, &image, error) !=
        BONGO_CAT_OK) return 0;
    GLuint texture = upload(&image, 0, false);
    if (width) *width = image.width;
    if (height) *height = image.height;
    bongo_cat_image_free(&image);
    return texture;
}
typedef struct ImageProgressStage {
    BongoCatImageProgress progress;
    void *userdata;
    float start;
    float span;
} ImageProgressStage;

static void report_stage_progress(void *userdata, float progress) {
    ImageProgressStage *stage = userdata;
    if (stage && stage->progress)
        stage->progress(stage->userdata, stage->start + stage->span * progress);
}

/*
 * 模型贴图上传 (GL 路径)。
 *
 * 进度区间刻意写死, 而不是按实际走了哪条分支去算: 解码可能从 WIC 退回 stb, 上传也可能因
 * GL_OUT_OF_MEMORY 缩小重试, 各分支耗时不同但区间固定 —— 进度条才不会来回跳。
 */
unsigned int bongo_cat_image_texture_model(const char *path, bool direct_decode,
    int *width, int *height, BongoCatImageAlphaMask *alpha,
    BongoCatImageProgress progress, void *userdata, BongoCatError *error) {
    BongoCatImage image = {0};
    ImageProgressStage stage = {progress, userdata, 0.0f, .30f};
    BongoCatImageProgress staged = progress ? report_stage_progress : NULL;
    GLint hardware_limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &hardware_limit);
    if (hardware_limit < 1) hardware_limit = BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT;
#ifdef _WIN32
    /*
     * A model atlas can advertise 8192px even when the pet is rendered in a
     * few hundred pixels. Decoding that atlas at full size costs hundreds of
     * MB of temporary RAM and makes alpha-mask generation and glTexImage2D
     * dominate model switching. Keep a bounded working size for normal loads;
     * this is still well above the largest bundled 1024px atlas. The hardware
     * limit remains authoritative on low-end GPUs.
     */
    int decode_limit = SDL_min(hardware_limit,
        BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT);
    /* Preset atlases are verified byte-identical in WIC and stb. Keep WIC for
       custom PNG color metadata and for memory-bounded oversized decoding. */
    bool scaled = bongo_cat_image_needs_wic_scaling(path, decode_limit);
    if (!direct_decode || scaled) {
        stage.span = .20f;
        if (!bongo_cat_image_decode_wic_responsive(path, &image,
            decode_limit, decode_limit, staged, &stage)) {
            stage = (ImageProgressStage){progress, userdata, .20f, .10f};
            if (bongo_cat_image_decode_pixels_responsive(path, &image,
                staged, &stage, error) != BONGO_CAT_OK) return 0;
        }
    } else if (bongo_cat_image_decode_pixels_responsive(path, &image,
        staged, &stage, error) != BONGO_CAT_OK) return 0;
    if (scaled) SDL_Log("Live2D texture constrained to %dx%d by a %d pixel "
        "texture limit: %s", image.width, image.height, decode_limit, path);
    else if (image.width > BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT ||
        image.height > BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT)
        SDL_Log("High-resolution Live2D texture preserved at %dx%d: %s",
            image.width, image.height, path);
#else
    (void)direct_decode;
    if (bongo_cat_image_decode_pixels_responsive(path, &image,
        staged, &stage, error) != BONGO_CAT_OK) return 0;
#endif
    if (progress) progress(userdata, .30f);
    stage = (ImageProgressStage){progress, userdata, .30f, .30f};
    bongo_cat_image_make_alpha_mask_progress(&image, alpha, staged, &stage);
    bongo_cat_gl_clear_errors();
    GLuint texture = upload(&image, 0, true);
    GLenum upload_error = glGetError();
#ifdef _WIN32
    if (upload_error == GL_OUT_OF_MEMORY &&
        (image.width > BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT ||
            image.height > BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT)) {
        if (texture) glDeleteTextures(1, &texture);
        texture = 0;
        bongo_cat_image_free(&image);
        int fallback_limit = SDL_min(hardware_limit,
            BONGO_CAT_LIVE2D_EFFICIENT_TEXTURE_LIMIT);
        stage = (ImageProgressStage){progress, userdata, .90f, .04f};
        if (bongo_cat_image_decode_wic_responsive(path, &image,
            fallback_limit, fallback_limit, staged, &stage)) {
            stage = (ImageProgressStage){progress, userdata, .94f, .03f};
            bongo_cat_image_make_alpha_mask_progress(
                &image, alpha, staged, &stage);
            bongo_cat_gl_clear_errors();
            texture = upload(&image, 0, true);
            upload_error = glGetError();
            if (upload_error == GL_NO_ERROR) SDL_LogWarn(
                SDL_LOG_CATEGORY_RENDER,
                "Live2D texture fell back to %dx%d after full-resolution "
                "GPU upload exhausted memory: %s", image.width, image.height, path);
        }
    }
#endif
    if (!texture || upload_error != GL_NO_ERROR) {
        if (texture) glDeleteTextures(1, &texture);
        texture = 0;
        bongo_cat_error_set(error, upload_error == GL_OUT_OF_MEMORY
            ? BONGO_CAT_ERROR_MEMORY : BONGO_CAT_ERROR_PLATFORM,
            "OpenGL texture upload failed (0x%x): %s", (unsigned)upload_error, path);
    }
    if (width) *width = image.width;
    if (height) *height = image.height;
    bongo_cat_image_free(&image);
    if (progress) progress(userdata, 1.0f);
    return texture;
}

/* 把掌心那块"脚印"抹掉的椭圆, 中心和半径是**针对内置猫图手工标定的归一化值**: 换一张底图
 * 必须重新标定, 否则擦出来的形状对不上爪子。只把 alpha 非零的像素刷成白色 (而不是设成
 * 透明), 所以抹掉的是"图案"而不是"那块区域"。 */
static void erase_paw(BongoCatImage *image, bool left) {
    float cx = left ? .700f : .275f, cy = left ? .515f : .397f;
    float rx = left ? .080f : .070f, ry = left ? .170f : .160f;
    for (int y = 0; y < image->height; ++y) for (int x = 0; x < image->width; ++x) {
        float dx = (x / (float)image->width - cx) / rx;
        float dy = (y / (float)image->height - cy) / ry;
        unsigned char *pixel = image->pixels + ((size_t)y * image->width + x) * 4;
        if (dx * dx + dy * dy < 1.0f && pixel[3])
            pixel[0] = pixel[1] = pixel[2] = 255;
    }
}

/* source-over 混合, 且要求两层尺寸完全一致: 尺寸不符就整层放弃并返回 false —— 宁可不出图,
 * 也不要把错位的图层糊上去。公式按"未预乘像素 + 直通 alpha"写, 中间乘 255 放大是为了避免
 * 整数除法提前截断。 */
static bool blend_file(BongoCatImage *base, const char *path, BongoCatError *error) {
    if (!path || !path[0]) return true;
    BongoCatImage layer;
    if (bongo_cat_image_load(path, &layer, error) != BONGO_CAT_OK) return false;
    bool valid = layer.width == base->width && layer.height == base->height;
    if (valid) for (int i = 0; i < base->width * base->height; ++i) {
        unsigned char *dst = base->pixels + i * 4, *src = layer.pixels + i * 4;
        unsigned alpha = src[3], inverse = 255 - alpha, destination_alpha = dst[3];
        unsigned output_alpha = alpha * 255 + destination_alpha * inverse;
        if (!output_alpha) { memset(dst, 0, 4); continue; }
        for (int channel = 0; channel < 3; ++channel) {
            unsigned color = src[channel] * alpha * 255 +
                dst[channel] * destination_alpha * inverse;
            dst[channel] = (unsigned char)((color + output_alpha / 2) / output_alpha);
        }
        dst[3] = (unsigned char)((output_alpha + 127) / 255);
    }
    bongo_cat_image_free(&layer);
    return valid;
}

BongoCatResult bongo_cat_image_composite(const char *base, const char *left,
    const char *right, bool erase_left, bool erase_right, BongoCatImage *out,
    BongoCatError *error) {
    if (!out) return BONGO_CAT_ERROR_ARGUMENT;
    BongoCatImage image;
    BongoCatResult result = bongo_cat_image_load(base, &image, error);
    if (result != BONGO_CAT_OK) return result;
    if (erase_left) erase_paw(&image, true);
    if (erase_right) erase_paw(&image, false);
    if (!blend_file(&image, left, error) || !blend_file(&image, right, error)) {
        bongo_cat_image_free(&image);
        return BONGO_CAT_ERROR_IO;
    }
    *out = image;
    return BONGO_CAT_OK;
}

unsigned int bongo_cat_image_composite_texture(const char *base, const char *left,
    const char *right, unsigned int texture, bool erase_left, bool erase_right,
    BongoCatError *error) {
    BongoCatImage image;
    if (bongo_cat_image_composite(base, left, right, erase_left, erase_right,
            &image, error) != BONGO_CAT_OK) return 0;
    texture = upload(&image, texture, false);
    bongo_cat_image_free(&image);
    return texture;
}
