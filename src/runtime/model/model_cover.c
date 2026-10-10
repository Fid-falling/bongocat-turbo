/* 模型封面 (设置窗口里每张卡片上的预览图) 的抓取与落盘。
 * 触发链: 模型提交后排程 (model_files.c), 主循环在下一帧走离屏渲染 (present=false) 时收尾,
 * 把这一帧的像素写成 <adapter_directory>/resources/cover.png。两条渲染后端各自回读 ——
 * OpenGL 走 glReadPixels, Vulkan 走整帧回读 (有一帧以上延迟, 没就绪就返回失败让上层退避重试)。
 * 契约: 队列里只有属于 app->loaded_model 的任务会被抓取; 写出成功或主动放弃都要摘链 (finish),
 * 失败则保持挂起, 由 bongo_cat_model_cover_defer 安排下一次。
 * 最容易改错的两处: 两条后端的行序相反 (见 read_cover_pixels), 且 Vulkan 的样本是一次性取走的。
 */
#include "model_cover.h"

#include "preferences_model_cover.h"
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stb_image_write.h>

typedef struct CoverWriter {
    FILE *file;
    bool ok;
} CoverWriter;

/* 切换到另一个模型之前, 为即将离开的模型安排一次封面刷新。
 * 必须在 app->loaded_model 仍指向旧模型时调用 —— 队列按 loaded_model 匹配任务。
 * 这里只排程并尝试立刻抓一帧; 抓不到就交给主循环的退避重试, 只留一条日志。
 */
void bongo_cat_model_cover_capture_before_switch(BongoCatApp *app) {
    if (!app || !app->loaded_model[0]) return;
    const BongoCatModelEntry *entry = bongo_cat_models_find(&app->models,
        app->loaded_model);
    if (!entry) return;
    bongo_cat_model_cover_schedule(app, entry);
    if (!bongo_cat_model_cover_pending(app)) return;
    if (!bongo_cat_app_capture_pending_model_cover(app))
        SDL_Log("Model cover refresh before switch deferred: id=%s",
            entry->id);
}

/* stb_image_write 的写出回调: 任何一次短写只记下失败, 由调用方在 fclose 之后统一判定。 */
static void cover_write(void *context, void *data, int size) {
    CoverWriter *writer = context;
    if (writer->ok && fwrite(data, 1, (size_t)size, writer->file) !=
            (size_t)size)
        writer->ok = false;
}

/* glReadPixels 的第 0 行是图像底部, 而 PNG 第 0 行是顶部, 所以 GL 路径要整体翻行。
 * Vulkan 回读已经是自上而下, 不要在这里重复翻转。
 */
static void flip_cover_pixels(unsigned char *pixels, int height, size_t pitch) {
    for (int y = 0; y < height / 2; ++y) {
        unsigned char *top = pixels + (size_t)y * pitch;
        unsigned char *bottom = pixels + (size_t)(height - y - 1) * pitch;
        for (size_t i = 0; i < pitch; ++i) {
            unsigned char value = top[i];
            top[i] = bottom[i];
            bottom[i] = value;
        }
    }
}

/* 取一帧 RGBA 像素。返回 malloc 的缓冲, 由调用方 free; 失败返回 NULL。
 * 副作用: Vulkan 分支会一次性取走 (take) 整帧样本, 之后再取要等下一次回读。
 */
static unsigned char *read_cover_pixels(const char *path, int width,
    int height, size_t pitch, size_t bytes) {
    /*
     * Vulkan 没有可读的 GL 前后缓冲: 用整帧回读的结果 (vk_readback.c)。
     * Vulkan 图像第 0 行在视觉上方, 与 glReadPixels 相反, 所以这里不翻转。
     */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        int sample_width = 0, sample_height = 0;
        const unsigned char *sample = NULL;
        if (!bongo_cat_vk_take_frame_sample(bongo_cat_vk_current(),
                &sample_width, &sample_height, &sample) ||
            sample_width < width || sample_height < height || !sample) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Model cover readback not ready (Vulkan): path=%s sample=%dx%d "
                "window=%dx%d", path, sample_width, sample_height, width, height);
            return NULL;
        }
        unsigned char *copy = malloc(bytes);
        if (!copy) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Model cover refresh allocation failed for %zu bytes: %s",
                bytes, path);
            return NULL;
        }
        for (int y = 0; y < height; ++y)
            SDL_memcpy(copy + (size_t)y * pitch,
                sample + (size_t)y * (size_t)sample_width * 4u, pitch);
        return copy;
    }
    unsigned char *pixels = malloc(bytes);
    if (!pixels) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Model cover refresh allocation failed for %zu bytes: %s",
            bytes, path);
        return NULL;
    }
    while (glGetError() != GL_NO_ERROR) {}
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Model cover framebuffer readback failed: path=%s gl_error=0x%x",
            path, error);
        free(pixels);
        return NULL;
    }
    flip_cover_pixels(pixels, height, pitch);
    return pixels;
}

/* 先写临时文件, 再原子替换到目标路径: 失败时删掉临时文件并返回 false, 目标文件保持旧内容。
 * durable=false —— 封面是可再生的缓存, 不值得为它做落盘同步。
 */
static bool write_cover_png(const char *temporary, const char *path,
    const unsigned char *pixels, int width, int height, size_t pitch) {
    FILE *file = bongo_cat_file_open(temporary, "wb");
    CoverWriter writer = {file, file != NULL};
    bool ok = file && stbi_write_png_to_func(cover_write, &writer,
        width, height, 4, pixels, (int)pitch) && writer.ok;
    if (file && fclose(file) != 0) ok = false;
    if (!ok) {
        bongo_cat_file_remove(temporary);
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Model cover could not be encoded: %s", path);
        return false;
    }
    if (bongo_cat_file_replace(temporary, path, false)) return true;
    bongo_cat_file_remove(temporary);
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "Model cover temporary file could not replace target: %s", path);
    return false;
}

/* 抓取并写出当前模型的封面。返回值是"这次任务是否算处理完", 而不是"是否成功":
 * true = 已写出, 或因尺寸超出 4096 上限而主动放弃 (两者都会摘链); false = 这次没成,
 * 任务仍挂起, 调用方应当 defer 退避后再来。成功时会顺带让设置窗口重载这张封面。
 */
bool bongo_cat_model_cover_capture(BongoCatApp *app, int width, int height) {
    if (!app || !bongo_cat_model_cover_pending(app)) return false;
    uint64_t started_ns = SDL_GetTicksNS();
    const char *pending_path = bongo_cat_model_cover_pending_path(app);
    if (!pending_path) return false;
    char path[BONGO_CAT_PATH_CAP];
    int path_length = snprintf(path, sizeof(path), "%s", pending_path);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) return false;
    if (width < 2 || height < 2) return false;
    /* Keep the desktop's native pixel size while bounding readback memory. */
    if (width > 4096 || height > 4096) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Model cover refresh abandoned for unsupported size %dx%d: %s",
            width, height, path);
        bongo_cat_model_cover_finish(app);
        return true;
    }
    size_t pitch = (size_t)width * 4;
    size_t bytes = pitch * (size_t)height;
    if (pitch / 4 != (size_t)width || bytes / pitch != (size_t)height) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Model cover refresh skipped due to size overflow: %s", path);
        return false;
    }
    /* 主/副桌宠是两个进程, 都可能刷新同一张封面: 临时名带实例后缀, 避免互相看到半截文件。 */
    char temporary[BONGO_CAT_PATH_CAP];
    int temporary_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%s",
        path, app->secondary_pet ? "secondary" : "primary");
    if (temporary_length < 0 || (size_t)temporary_length >= sizeof(temporary)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Model cover temporary path is too long: %s", path);
        bongo_cat_model_cover_finish(app);
        return true;
    }
    unsigned char *pixels = read_cover_pixels(path, width, height, pitch, bytes);
    if (!pixels) return false;
    bool written = write_cover_png(temporary, path, pixels,
        width, height, pitch);
    free(pixels);
    if (!written) return false;
    bongo_cat_model_cover_finish(app);
    if (app->preferences)
        bongo_cat_preferences_model_cover_reload(app, path);
    SDL_Log("Model cover refreshed: path=%s (%dx%d) elapsed_ms=%.2f", path,
        width, height, (double)(SDL_GetTicksNS() - started_ns) / 1000000.0);
    return true;
}
