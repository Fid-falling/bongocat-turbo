/*
 * UI 绘制缓存: 渐变 / 圆角外壳 / 发光 / 阴影这类"整块"效果, CPU 逐像素算一次很贵
 * (900x680 的 sidebar glow 一项就 15ms), 所以烘焙成纹理存下来, 之后每帧只画一个四边形。
 *
 * 过期规则踩过坑, 别改回去: begin_frame() 释放的是"连续两帧都没用到"的条目, 判定基准
 * 必须是"上一帧开始时的 use_counter"。曾经写成 use_counter + 1, 于是上一帧刚用过的条目
 * 也满足 used < marker —— 等价于每帧清空整个缓存重新烘焙, 浅色主题的设置界面直接掉到
 * 20 FPS。命中时也要更新 used, 否则命中一次就会被误判过期。
 *
 * 容量: 全屏项每个 2MB 级, 上限要给到能装下整个界面, 不然每帧都有条目被挤掉重建;
 * 条目按 backend 分开统计 (宠物窗口 / 设置窗口各算各的)。
 *
 * 纹理 id 走 UI 的纹理表 (BONGO_CAT_UI_VK_TEXTURES): 表满时注册会失败, item->texture
 * 变成 0, ready() 永远为假 —— 表现同样是"每帧重新烘焙", 所以那张表别调小。
 */

#include "ui_paint_cache.h"
#include "ui_backend.h"

#include <SDL3/SDL_opengl.h>
#include <stddef.h>
#include <string.h>

struct BongoCatUIPaintTexture {
    BongoCatUIBackend *backend;
    BongoCatUIPaintKey key;
    GLuint texture;
    uint64_t used;
    size_t bytes;
};

#define PAINT_CACHE_COUNT 48
/*
 * 缓存上限。别调小: 设置界面的几个全屏项 (sidebar glow、圆角外壳、阴影) 每个就是
 * 2 MB 级, 浅色主题下合计二十多 MB —— 16 MB 时每帧都有条目被挤出, 于是每帧重新
 * CPU 烘焙 (900x680 的 glow 一项 15 ms) 并重新上传纹理, 界面掉到 20 FPS。
 * 现在按"能装下整个界面"给, 只在尺寸/主题变化时重建。
 */
#define PAINT_CACHE_BYTE_LIMIT (64u * 1024u * 1024u)

static BongoCatUIPaintTexture textures[PAINT_CACHE_COUNT];
static uint64_t use_counter;

static void release_texture(BongoCatUIPaintTexture *item) {
    if (item->texture) {
        if (bongo_cat_ui_uses_vulkan(item->backend))
            bongo_cat_ui_texture_unregister(item->backend, (int)item->texture);
        else
            glDeleteTextures(1, &item->texture);
    }
    memset(item, 0, sizeof(*item));
}

static bool reserve_bytes(BongoCatUIBackend *backend,
    BongoCatUIPaintTexture *keep, size_t required) {
    if (required > PAINT_CACHE_BYTE_LIMIT) return false;
    for (;;) {
        size_t used = 0;
        BongoCatUIPaintTexture *oldest = NULL;
        for (size_t i = 0; i < PAINT_CACHE_COUNT; ++i) {
            BongoCatUIPaintTexture *item = &textures[i];
            if (item == keep || item->backend != backend || !item->texture)
                continue;
            used += item->bytes;
            bool stale = !backend->paint_frame_marker ||
                item->used < backend->paint_frame_marker;
            if (stale && (!oldest || item->used < oldest->used)) oldest = item;
        }
        if (SDL_getenv("BONGO_CAT_TEST_PREFS_STAGES"))
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[paint] 挤出 used=%zuKB required=%zuKB limit=%uKB", used / 1024,
                required / 1024, (unsigned)(PAINT_CACHE_BYTE_LIMIT / 1024));
        if (used <= PAINT_CACHE_BYTE_LIMIT &&
            required <= PAINT_CACHE_BYTE_LIMIT - used) return true;
        if (!oldest) return false;
        release_texture(oldest);
    }
}

BongoCatUIPaintTexture *bongo_cat_ui_paint_cache_get(
    BongoCatUIBackend *backend, const BongoCatUIPaintKey *key) {
    BongoCatUIPaintTexture *empty = NULL, *oldest = NULL;
    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); ++i) {
        BongoCatUIPaintTexture *item = &textures[i];
        if (item->texture && item->backend == backend &&
            memcmp(&item->key, key, sizeof(*key)) == 0) {
            item->used = ++use_counter;
            return item;
        }
        if (!item->texture && !empty) empty = item;
        bool reusable = item->backend == backend &&
            (!backend->paint_frame_marker ||
                item->used < backend->paint_frame_marker);
        if (reusable && (!oldest || item->used < oldest->used)) oldest = item;
    }
    BongoCatUIPaintTexture *item = empty ? empty : oldest;
    if (!item) return NULL;
    release_texture(item);
    item->backend = backend;
    item->key = *key;
    item->used = ++use_counter;
    /* 排查用: 缓存未命中 (会走 CPU 逐像素烘焙 + 上传纹理) */
    if (SDL_getenv("BONGO_CAT_TEST_PREFS_STAGES"))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[paint] 未命中 backend=%p kind=%d %dx%d r=%d p1=%d c=%u marker=%llu",
            (void *)backend, (int)key->kind, key->width, key->height,
            key->radius, key->first_parameter, (unsigned)key->first_color,
            (unsigned long long)backend->paint_frame_marker);
    return item;
}

bool bongo_cat_ui_paint_cache_ready(
    const BongoCatUIPaintTexture *item) {
    return item && item->texture != 0;
}

bool bongo_cat_ui_paint_cache_upload(BongoCatUIPaintTexture *item,
    const unsigned char *pixels, bool single_channel) {
    if (!item || !pixels || item->key.width <= 0 || item->key.height <= 0)
        return false;
    size_t channels = single_channel ? 1u : 4u;
    if ((size_t)item->key.width > SIZE_MAX / (size_t)item->key.height ||
        (size_t)item->key.width * (size_t)item->key.height >
            SIZE_MAX / channels) return false;
    size_t bytes = (size_t)item->key.width * item->key.height * channels;
    if (!reserve_bytes(item->backend, item, bytes)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "UI paint cache limit rejected a %llu-byte texture",
            (unsigned long long)bytes);
        return false;
    }
    if (bongo_cat_ui_uses_vulkan(item->backend)) {
        BongoCatError error = {0};
        BongoCatVkTexture *texture = single_channel
            ? bongo_cat_vk_texture_from_alpha(item->backend->vk, pixels,
                item->key.width, item->key.height, "ui-paint", &error)
            : bongo_cat_vk_texture_from_pixels(item->backend->vk, pixels,
                item->key.width, item->key.height, "ui-paint", &error);
        if (!texture) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "UI paint texture upload failed (vulkan) for %dx%d: %s",
                item->key.width, item->key.height, error.message);
            return false;
        }
        int id = bongo_cat_ui_texture_register(item->backend, texture);
        if (!id) {
            bongo_cat_vk_texture_destroy(item->backend->vk, texture);
            return false;
        }
        item->texture = (GLuint)id;
        item->bytes = bytes;
        return true;
    }
    bongo_cat_gl_clear_errors();
    glGenTextures(1, &item->texture);
    glBindTexture(GL_TEXTURE_2D, item->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (single_channel) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, single_channel ? GL_R8 : GL_RGBA8,
        item->key.width, item->key.height, 0,
        single_channel ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLenum upload_error = glGetError();
    if (item->texture && upload_error == GL_NO_ERROR) {
        item->bytes = bytes;
        return true;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "UI paint texture upload failed for %dx%d (0x%x)",
        item->key.width, item->key.height, (unsigned)upload_error);
    if (item->texture) glDeleteTextures(1, &item->texture);
    item->texture = 0;
    return false;
}

void bongo_cat_ui_paint_cache_draw(struct nk_context *context,
    struct nk_rect bounds, const BongoCatUIPaintTexture *item,
    struct nk_color tint) {
    struct nk_image image = nk_image_id((int)item->texture);
    nk_draw_image(nk_window_get_canvas(context), bounds, &image, tint);
}

void bongo_cat_ui_paint_cache_begin_frame(BongoCatUIBackend *backend) {
    if (!backend) return;
    /*
     * paint_frame_marker 存的是"上一帧开始时的 use_counter":
     * 上一帧用过的条目 used >= 这个值, 不会被释放; 只有连续两帧都没用到的条目
     * (used < marker) 才释放。
     *
     * 以前这里存的是 use_counter + 1, 于是上一帧刚用过的条目也满足
     * used < marker —— 等于每帧把整个缓存清空重烘焙。浅色主题那个全屏的
     * sidebar glow 一项就要 15 ms/帧, 设置界面因此只有 20 FPS 左右。
     */
    const uint64_t previous_frame_start = backend->paint_frame_marker;
    if (previous_frame_start) for (size_t i = 0; i < PAINT_CACHE_COUNT; ++i) {
        BongoCatUIPaintTexture *item = &textures[i];
        if (item->backend == backend && item->texture &&
            item->used < previous_frame_start)
            release_texture(item);
    }
    backend->paint_frame_marker = use_counter;
}

size_t bongo_cat_ui_paint_cache_usage(BongoCatUIBackend *backend,
    size_t *texture_count) {
    size_t bytes = 0, count = 0;
    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); ++i) {
        const BongoCatUIPaintTexture *item = &textures[i];
        if (item->backend != backend || !item->texture) continue;
        bytes += item->bytes;
        count++;
    }
    if (texture_count) *texture_count = count;
    return bytes;
}

void bongo_cat_ui_paint_cache_destroy(BongoCatUIBackend *backend) {
    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); ++i) {
        BongoCatUIPaintTexture *item = &textures[i];
        if (item->backend != backend) continue;
        release_texture(item);
    }
    if (backend) backend->paint_frame_marker = 0;
}

void bongo_cat_ui_paint_cache_abandon(BongoCatUIBackend *backend) {
    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); ++i) {
        BongoCatUIPaintTexture *item = &textures[i];
        if (item->backend == backend) memset(item, 0, sizeof(*item));
    }
    if (backend) backend->paint_frame_marker = 0;
}
