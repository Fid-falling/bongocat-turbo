/* "首帧到底呈现了什么"的对照审计: 切换缓冲前先对整帧像素做一次指纹 (prepare), 平台真正 present
 * 之后再回读 GL 前/后台缓冲, 用指纹是否相同判断用户看到的是哪一块。它只跑一次 (presented 置位
 * 后不再重跑), 且只由 app_loop.c 的 OpenGL 路径调用 —— render() 在 Vulkan 会分叉到 vk_render_main
 * 并提前返回, 那边只会看到 prepare 的日志。Windows 上如果存在 layered 代理窗口, 可见输出其实是
 * 代理而不是 GL 表面, 此时跳过前台缓冲回读, 改为抓取源窗口与代理的截图诊断信息。
 * 契约: prepare 必须在 swap 之前调用, 传进来的像素是借用指针, 本文件不持有。
 */
#include "runtime.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL_opengl.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include "windows_capture.h"
#include "windows_diagnostics.h"
#include "windows_layered.h"
#include <SDL3/SDL_properties.h>
#endif

#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_SAMPLE_BUFFERS
#define GL_SAMPLE_BUFFERS 0x80A8
#endif
#ifndef GL_SAMPLES
#define GL_SAMPLES 0x80A9
#endif

/* 模块内单例, 生命周期只有"首帧": prepare 覆盖式写入, presented 审计读完置位 presented, 没有任何
 * 重置入口 —— 冒烟只需要一次结论, 多帧重跑反而会拿最后一帧覆盖掉证据。rendered_hash 是 prepare
 * 那一刻的整帧指纹, 也是后面与前/后台缓冲比对的唯一依据。
 */
typedef struct PresentationAudit {
    bool prepared;
    bool presented;
    bool visible;
    int width;
    int height;
    uint64_t rendered_hash;
} PresentationAudit;

static PresentationAudit audit;

/* 64 位字节哈希 (FNV 的偏移基数与乘子), 用途只是"两次读回是否同一幅图", 不承担安全性; 用整帧哈希
 * 而不是抽样, 是因为任何一个字节的差异都会改变结果, 哈希相同基本可以认定是同一份像素。
 */
static uint64_t pixel_hash(const unsigned char *pixels, size_t bytes) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < bytes; ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* 一次性打印 GL 的目标状态: draw/read buffer、绑定的 FBO、位深、MSAA, 以及 SDL 的 double-buffer、
 * 加速视觉、保留后台缓冲属性。排查"画了却看不到"时先看这里是不是画到了非默认 FBO 或读错了缓冲;
 * 函数开头会清空 GL 错误队列, 输出的 before/after 只用于日志。
 */
static void log_gl_presentation_state(BongoCatApp *app) {
    GLint draw_buffer = 0, read_buffer = 0;
    GLint draw_framebuffer = 0, read_framebuffer = 0;
    GLint red_bits = 0, green_bits = 0, blue_bits = 0, alpha_bits = 0;
    GLint depth_bits = 0, stencil_bits = 0, sample_buffers = 0, samples = 0;
    GLboolean double_buffer = GL_FALSE, stereo = GL_FALSE;
    GLenum before = glGetError();
    glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_RED_BITS, &red_bits);
    glGetIntegerv(GL_GREEN_BITS, &green_bits);
    glGetIntegerv(GL_BLUE_BITS, &blue_bits);
    glGetIntegerv(GL_ALPHA_BITS, &alpha_bits);
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sample_buffers);
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetBooleanv(GL_DOUBLEBUFFER, &double_buffer);
    glGetBooleanv(GL_STEREO, &stereo);
    GLenum after = glGetError();
    int sdl_double_buffer = -1, accelerated = -1, retained = -1;
    SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &sdl_double_buffer);
    SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &accelerated);
    SDL_GL_GetAttribute(SDL_GL_RETAINED_BACKING, &retained);
    SDL_WindowFlags flags = app && app->window ?
        SDL_GetWindowFlags(app->window) : 0;
    SDL_Log("OpenGL presentation state: draw_buffer=0x%x read_buffer=0x%x "
        "draw_fbo=%d read_fbo=%d rgba_bits=%d,%d,%d,%d depth_bits=%d "
        "stencil_bits=%d sample_buffers=%d samples=%d double_buffer=%d "
        "stereo=%d sdl_double_buffer=%d accelerated=%d retained=%d "
        "window_flags=0x%llx state_error_before=0x%x state_error_after=0x%x",
        draw_buffer, read_buffer, draw_framebuffer, read_framebuffer,
        red_bits, green_bits, blue_bits, alpha_bits, depth_bits, stencil_bits,
        sample_buffers, samples, double_buffer != GL_FALSE,
        stereo != GL_FALSE, sdl_double_buffer, accelerated, retained,
        (unsigned long long)flags, before, after);
}

/* 记录呈现前的整帧指纹并打印一次 GL 状态; 只由首帧审计调用, 不是每帧钩子。尺寸不合法或乘法溢出
 * 时直接忽略本次调用, 此时后面的 presented 审计会因为 !prepared 而自动跳过。
 */
void bongo_cat_frame_presentation_prepare(BongoCatApp *app,
    const unsigned char *pixels, int width, int height, bool visible) {
    if (!app || !pixels || width < 1 || height < 1) return;
    size_t bytes = (size_t)width * (size_t)height * 4;
    if (bytes / 4 != (size_t)width * (size_t)height) return;
    audit = (PresentationAudit){.prepared = true, .visible = visible,
        .width = width, .height = height,
        .rendered_hash = pixel_hash(pixels, bytes)};
    SDL_Log("Pre-presentation framebuffer: size=%dx%d visible=%d "
        "rgba_hash=0x%016llx", width, height, visible,
        (unsigned long long)audit.rendered_hash);
    log_gl_presentation_state(app);
}

/* 读回指定的前/后台缓冲并统计可见像素, 会临时把 GL_READ_BUFFER 切到目标缓冲再恢复原值 (恢复失败
 * 只记日志)。Vulkan 下没有可读的 GL 缓冲, 直接打印跳过。valid 只表示这次 glReadPixels 没报错 ——
 * 按 GL 规范, 交换之后后台缓冲的内容本来就是未定义的, matches_pre_swap 才算有意义的结论。
 */
static void log_post_present_buffer(const char *name, GLenum buffer) {
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        /* Vulkan 没有可回读的 GL 前/后台缓冲, 这项对比无法进行 */
        SDL_Log("Post-presentation buffer comparison skipped on Vulkan: name=%s", name);
        return;
    }
    size_t total = (size_t)audit.width * (size_t)audit.height;
    size_t bytes = total * 4;
    unsigned char *pixels = calloc(1, bytes);
    if (!pixels) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Post-presentation %s allocation failed for %zu bytes", name, bytes);
        return;
    }
    GLint previous = GL_BACK;
    glGetIntegerv(GL_READ_BUFFER, &previous);
    GLenum before = glGetError();
    glReadBuffer(buffer);
    glReadPixels(0, 0, audit.width, audit.height,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum after = glGetError();
    glReadBuffer((GLenum)previous);
    GLenum restore_error = glGetError();
    unsigned long long alpha = 0, visible = 0, nonblack = 0;
    for (size_t index = 0; index < bytes; index += 4) {
        unsigned sum = (unsigned)pixels[index] + pixels[index + 1] +
            pixels[index + 2];
        alpha += pixels[index + 3] > 8;
        nonblack += sum > 30;
        visible += pixels[index + 3] > 8 && sum > 30;
    }
    uint64_t hash = pixel_hash(pixels, bytes);
    bool valid = after == GL_NO_ERROR;
    SDL_Log("Post-presentation OpenGL buffer: name=%s enum=0x%x size=%dx%d "
        "valid=%d alpha=%.2f%% nonblack=%.2f%% visible=%.2f%% "
        "rgba_hash=0x%016llx matches_pre_swap=%d error_before=0x%x "
        "error_after=0x%x restore_error=0x%x",
        name, buffer, audit.width, audit.height, valid,
        100.0 * alpha / total, 100.0 * nonblack / total,
        100.0 * visible / total, (unsigned long long)hash,
        valid && hash == audit.rendered_hash, before, after, restore_error);
    free(pixels);
}

/* present 成功后由主循环调用一次; 非冒烟、没 prepare 过、已经审计过都会直接返回, 所以首帧呈现失败
 * 不会自动重试。Windows 下还会记录源窗口与 layered 代理各自的可见性, 并抓取两边的截图诊断,
 * 用来区分"GL 没画出来"和"合成/采集路径没拿到"。
 */
void bongo_cat_frame_presented_audit(BongoCatApp *app) {
    if (!app || !app->smoke || audit.presented || !audit.prepared) return;
    audit.presented = true;
    SDL_Log("First-frame presentation: success=1 framebuffer_visible=%d "
        "window_opacity=%.3f obs_background=%d obs_background_color=%s",
        audit.visible, bongo_cat_platform_get_opacity(&app->platform),
        app->settings.window.obs_background,
        bongo_cat_obs_background_color_name(
            app->settings.window.obs_background_color));
#ifdef _WIN32
    /* HWND 是从 SDL 窗口属性借出的句柄, 生命周期由 SDL 管理, 这里只读不持有; 拿不到说明窗口不是
     * Win32 实现, 后面的抓取诊断全部跳过。
     */
    HWND source = app->window ? (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(app->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
    if (!source) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Capture diagnostic skipped because the Win32 window is unavailable");
        return;
    }
    HWND proxy = bongo_cat_windows_layered_proxy(source);
    bool proxy_visible = proxy && IsWindow(proxy) && IsWindowVisible(proxy);
    SDL_Log("Windows presented path: source=%p proxy=%p proxy_visible=%d",
        (void *)source, (void *)proxy, proxy_visible);
    /* 只有代理不可见时 GL 表面才是用户看到的输出, 这时回读前/后台缓冲才有意义;
     * 代理可见就说明真正的画面在代理里, 再读 GL 只会得出误导性的结论。
     */
    if (!proxy_visible) {
        log_post_present_buffer("front", GL_FRONT);
        log_post_present_buffer("back", GL_BACK);
    } else {
        SDL_Log("Post-presentation OpenGL front-buffer readback skipped: "
            "the visible output is the layered proxy");
    }
    bongo_cat_windows_capture_log(source, "first-presented-source");
    bongo_cat_windows_diagnostics_probe_capture(source, "opengl-source");
    if (proxy && IsWindow(proxy)) {
        bongo_cat_windows_capture_log(proxy, "first-presented-layered-proxy");
        bongo_cat_windows_diagnostics_probe_capture(proxy, "layered-proxy");
    }
#else
    log_post_present_buffer("front", GL_FRONT);
    log_post_present_buffer("back", GL_BACK);
#endif
}
