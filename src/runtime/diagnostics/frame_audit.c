/* 冒烟 (--ci-smoke) 专用的帧内容诊断: 首帧统计一次像素分布, 之后按需把每帧统计追加到
 * frame-series.csv, 并按 50ms 限频把整帧存成 frame.bmp。两条后端共用本文件, 区别只在取像素
 * 的方式: OpenGL 直接读当前后台缓冲, Vulkan 走 bongo_cat_vk_take_frame_sample 的整帧回读。
 * 那个回读是"本帧入队、下一帧才取到"的一次性样本, 所以 Vulkan 分支写出的其实是上一帧,
 * 且首帧必然取不到而需要重试。契约: 非冒烟运行零开销直接返回; 写盘一律落在 app->state_root;
 * 不修改渲染状态也不缓存像素。
 */
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"
#include "model_cover.h"

#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FrameStats {
    unsigned visible;
    unsigned alpha;
} FrameStats;

/* 统计按 4x4 步长抽样, 所以 visible/alpha 是下界估计而不是精确像素数; 阈值 (RGB 和 >60、
 * alpha >8) 只服务于本文件的日志与冒烟断言, 与首帧诊断那套口径是分开的两组。
 */
static FrameStats frame_stats(const unsigned char *pixels,
    int width, int height, size_t pitch) {
    FrameStats stats = {0};
    for (int y = 0; y < height; y += 4) {
        const unsigned char *row = pixels + (size_t)y * pitch;
        for (int x = 0; x < width; x += 4) {
            const unsigned char *pixel = row + (size_t)x * 4;
            if ((unsigned)pixel[0] + pixel[1] + pixel[2] > 60) stats.visible++;
            if (pixel[3] > 8) stats.alpha++;
        }
    }
    return stats;
}

/* 返回 false 表示"这一帧还取不到像素" (Vulkan 整帧回读有一帧延迟), 下次再试 */
static bool log_first_frame(BongoCatApp *app, int width, int height) {
    /* 尺寸异常 (0/负数/超 4096) 或乘法溢出并不算错误, 但也无法诊断: 返回 true 让上层记下"已审计",
     * 不再逐帧重试, 免得每帧都刷同一条警告。
     */
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "First-frame diagnostic skipped for size %dx%d", width, height);
        return true;
    }
    size_t pitch = (size_t)width * 4, bytes = pitch * (size_t)height;
    if (pitch / 4 != (size_t)width || bytes / pitch != (size_t)height) return true;
    unsigned char *pixels = calloc(1, bytes);
    if (!pixels) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "First-frame diagnostic allocation failed for %zu bytes", bytes);
        return false;
    }
    /* Vulkan 没有可读的 GL 前后缓冲: 用整帧回读的结果 (第 0 行在视觉上方) */
    GLenum before = GL_NO_ERROR, after = GL_NO_ERROR;
    const bool vulkan = bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN;
    if (vulkan) {
        int sample_width = 0, sample_height = 0;
        const unsigned char *sample = NULL;
        if (!bongo_cat_vk_take_frame_sample(bongo_cat_vk_current(),
                &sample_width, &sample_height, &sample) ||
            sample_width < width || sample_height < height || !sample) {
            free(pixels);
            SDL_Log("First-frame diagnostic deferred: Vulkan readback not ready");
            return false;
        }
        for (int y = 0; y < height; ++y)
            memcpy(pixels + (size_t)y * pitch,
                sample + (size_t)y * (size_t)sample_width * 4u, pitch);
    } else {
        before = glGetError();
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        after = glGetError();
    }
    unsigned long long alpha = 0, opaque = 0, rgb_colored = 0;
    unsigned long long visible_colored = 0, black = 0;
    unsigned long long red = 0, green = 0, blue = 0, alpha_sum = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const unsigned char *pixel = pixels + (size_t)y * pitch + (size_t)x * 4;
        unsigned sum = (unsigned)pixel[0] + pixel[1] + pixel[2];
        alpha += pixel[3] > 8; opaque += pixel[3] > 247;
        rgb_colored += sum > 30;
        visible_colored += pixel[3] > 8 && sum > 30;
        black += pixel[3] > 8 && sum <= 30;
        red += pixel[0]; green += pixel[1]; blue += pixel[2];
        alpha_sum += pixel[3];
    }
    unsigned long long total = (unsigned long long)width * height;
    int sample_buffers = 0, sample_count = 0;
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &sample_buffers);
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &sample_count);
    SDL_Log("First-frame pixels: size=%dx%d total=%llu alpha=%llu opaque=%llu "
        "rgb_colored=%llu visible_colored=%llu black_with_alpha=%llu "
        "transparent=%llu "
        "avg_rgba=%.1f,%.1f,%.1f,%.1f gl_error_before=0x%x gl_error_after=0x%x "
        "msaa=%d/%d", width, height, total, alpha, opaque, rgb_colored,
        visible_colored, black,
        total - alpha, (double)red / total, (double)green / total,
        (double)blue / total, (double)alpha_sum / total, before, after,
        sample_buffers, sample_count);
    SDL_Log("First-frame ratios: alpha=%.2f%% rgb_colored=%.2f%% "
        "visible_colored=%.2f%% black_with_alpha=%.2f%%",
        100.0 * alpha / total, 100.0 * rgb_colored / total,
        100.0 * visible_colored / total, 100.0 * black / total);
    if (after != GL_NO_ERROR) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "First-frame diagnosis: OpenGL framebuffer readback failed (0x%x)", after);
    else if (!rgb_colored && !alpha) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "First-frame diagnosis: framebuffer is blank and fully transparent");
    else if (!rgb_colored) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "First-frame diagnosis: framebuffer contains only black pixels");
    else if (!visible_colored) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "First-frame diagnosis: framebuffer has RGB content but no visible color alpha");
    else SDL_Log("First-frame diagnosis: %s framebuffer contains visible content",
        vulkan ? "Vulkan" : "OpenGL");
    /* 把同一份像素交给"呈现前"审计: 它记下整帧指纹, 稍后与交换后的前/后台缓冲比对,
     * 用来判断屏幕上的画面到底是不是刚画完的这一帧。
     */
    bongo_cat_frame_presentation_prepare(app, pixels, width, height,
        after == GL_NO_ERROR && visible_colored > 0);
    /* 只有冒烟运行才落盘: frame-alpha.txt 记录四角加中心 5 个采样点的 alpha,
     * 供 CI 脚本断言窗口"确实透明/确实不透明", 不用依赖肉眼。
     */
    if (app->smoke) {
        const int points[][2] = {{0, 0}, {width - 1, 0}, {0, height - 1},
            {width - 1, height - 1}, {width / 2, height / 2}};
        unsigned transparent = 0, opaque_samples = 0;
        for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
            unsigned char value = pixels[(size_t)points[i][1] * pitch +
                (size_t)points[i][0] * 4 + 3];
            transparent += value < 16; opaque_samples += value > 239;
        }
        char path[BONGO_CAT_PATH_CAP];
        bongo_cat_path_join(path, sizeof(path), app->state_root,
            "frame-alpha.txt");
        FILE *file = bongo_cat_file_open(path, "wb");
        if (file) {
            fprintf(file, "samples=5 transparent=%u opaque=%u sample_buffers=%d "
                "sample_count=%d gl_error=%u\n", transparent, opaque_samples,
                sample_buffers, sample_count, (unsigned)after);
            fclose(file);
        }
    }
    free(pixels);
    return true;
}

/* 往 frame-series.csv 追加一行帧统计; 表头只在该文件不存在时写 (追加模式), 所以换列必须先删旧文件。
 * 一行里混放了会话状态 (缩放、不透明度、模型) 与 OS 可见性, 方便回归时横向对比。
 */
static void record_frame(BongoCatApp *app, const unsigned char *pixels,
    int width, int height, size_t pitch) {
    if (!app->smoke_frame_series) return;
    char path[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(path, sizeof(path), app->state_root,
        "frame-series.csv");
    bool header = !bongo_cat_path_is_file(path);
    FILE *file = bongo_cat_file_open(path, "ab");
    if (!file) return;
    if (header) fputs("ticks_ns,width,height,visible_pixels,alpha_pixels,"
        "scale_percent,opacity_percent,window_opacity,model_mode,"
        "model_state_consistent,selection_serial,window_config_visible,"
        "window_os_visible\n", file);
    FrameStats stats = frame_stats(pixels, width, height, pitch);
    bool model_consistent = bongo_cat_live2d_ready(app->live2d) &&
        app->loaded_model[0] &&
        strcmp(app->loaded_model, app->session.active_model_id) == 0;
    /* 两个"可见"不是一回事: session.window.visible 是我们希望窗口可见, SDL_WINDOW_HIDDEN 反映的是
     * 系统层面的实际状态 (最小化、被平台隐藏); 两者不一致通常就是"窗口没出来"的直接线索。
     */
    bool os_visible = (SDL_GetWindowFlags(app->window) & SDL_WINDOW_HIDDEN) == 0;
    fprintf(file, "%llu,%d,%d,%u,%u,%.3f,%.3f,%.5f,%s,%d,%u,%d,%d\n",
        (unsigned long long)SDL_GetTicksNS(), width, height,
        stats.visible, stats.alpha,
        app->session.window.scale_percent, app->session.window.opacity_percent,
        bongo_cat_platform_get_opacity(&app->platform),
        bongo_cat_mode_name(app->loaded_mode), model_consistent,
        app->model_selection_serial, app->session.window.visible, os_visible);
    fclose(file);
}

/* 主循环每呈现一帧调用一次 (OpenGL 在 app_loop.c、Vulkan 在 vk_scene.c), 只在冒烟模式做事,
 * 会写 state_root 下的 frame.bmp 与 frame-series.csv。Vulkan 下取不到回读样本就整帧跳过 ——
 * 样本是上一帧入队的, 取不到属于正常时序而不是失败。
 */
void bongo_cat_frame_audit(BongoCatApp *app, int width, int height) {
    if (!app || !app->smoke || width < 2 || height < 2) return;
    /*
     * 封面捕获与冒烟诊断共用 Vulkan 的同一份**一次性**整帧回读 (见 vk_readback.c)。
     * 诊断在渲染末尾先 take 的话, 主循环随后的封面 capture 永远只能拿到
     * sample=0x0; 而封面任务会一直挂起重试, 于是两边互相饿死 —— 实测 --ci-smoke
     * 下每帧一条 "Model cover readback not ready", 封面一次都写不出来。
     * 封面优先: 只要有封面任务挂起, 本帧整帧诊断整体跳过 (不 take), 写完后的下一帧
     * 自然恢复。非冒烟运行本来就在上一行返回, 零开销。
     */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN &&
        bongo_cat_model_cover_pending(app))
        return;
    /* Vulkan 的整帧回读有一帧延迟: 首帧取不到就保持未审计, 下一帧再试 */
    if (!app->smoke_frame_audited && log_first_frame(app, width, height))
        app->smoke_frame_audited = true;
    char path[BONGO_CAT_PATH_CAP];
    size_t pitch = (size_t)width * 4, bytes = pitch * (size_t)height;
    unsigned char *pixels = malloc(bytes);
    if (!pixels) return;
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        int sample_width = 0, sample_height = 0;
        const unsigned char *sample = NULL;
        if (!bongo_cat_vk_take_frame_sample(bongo_cat_vk_current(),
                &sample_width, &sample_height, &sample) ||
            sample_width < width || sample_height < height || !sample) {
            free(pixels);
            return;
        }
        for (int y = 0; y < height; ++y)
            memcpy(pixels + (size_t)y * pitch,
                sample + (size_t)y * (size_t)sample_width * 4u, pitch);
        record_frame(app, pixels, width, height, pitch);
        uint64_t now_ns = SDL_GetTicksNS();
        /* 限频 50ms: 冒烟运行只有 1.5s, 每帧都存 BMP 会明显拖慢帧率, 让磁盘 I/O 变成被测对象。 */
        if (app->frame_audit_bmp_ns && now_ns >= app->frame_audit_bmp_ns &&
            now_ns - app->frame_audit_bmp_ns < 50000000ull) {
            free(pixels);
            return;
        }
        app->frame_audit_bmp_ns = now_ns;
        char bmp_path[BONGO_CAT_PATH_CAP];
        /* Vulkan 图像本来就是第 0 行在上方, 不需要像 GL 那样翻转 */
        SDL_Surface *bmp = SDL_CreateSurfaceFrom(width, height,
            SDL_PIXELFORMAT_RGBA32, pixels, (int)pitch);
        bongo_cat_path_join(bmp_path, sizeof(bmp_path), app->state_root, "frame.bmp");
        if (bmp) { SDL_SaveBMP(bmp, bmp_path); SDL_DestroySurface(bmp); }
        free(pixels);
        return;
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    record_frame(app, pixels, width, height, pitch);
    uint64_t now = SDL_GetTicksNS();
    if (app->frame_audit_bmp_ns && now >= app->frame_audit_bmp_ns &&
        now - app->frame_audit_bmp_ns < 50000000ull) {
        free(pixels); return;
    }
    app->frame_audit_bmp_ns = now;
    unsigned char *row = malloc(pitch);
    if (!row) { free(pixels); return; }
    /* glReadPixels 的第 0 行在图像下方, 而 BMP 存的是上方, 所以这里要整幅上下翻转;
     * Vulkan 分支的回读本来就是上方优先, 不能共用这段。
     */
    for (int y = 0; y < height / 2; ++y) {
        unsigned char *top = pixels + (size_t)y * pitch;
        unsigned char *bottom = pixels + (size_t)(height - 1 - y) * pitch;
        memcpy(row, top, pitch); memcpy(top, bottom, pitch); memcpy(bottom, row, pitch);
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height,
        SDL_PIXELFORMAT_RGBA32, pixels, (int)pitch);
    bongo_cat_path_join(path, sizeof(path), app->state_root, "frame.bmp");
    if (surface) { SDL_SaveBMP(surface, path); SDL_DestroySurface(surface); }
    free(row); free(pixels);
}
