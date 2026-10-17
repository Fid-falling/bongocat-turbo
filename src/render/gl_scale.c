/*
 * OpenGL 侧的"渲染精度": N 倍离屏渲染 + 线性降采样回窗口尺寸。
 *
 * 为什么用 FBO + glBlitFramebuffer 而不是画一个全屏四边形:
 *   - blit 自带线性过滤, 不需要再多一套着色器/VAO/采样器, 也不会碰到叠加层着色器里
 *     那些 erase/premultiply 的语义;
 *   - 颜色已经是预乘 alpha (叠加层着色器乘过一次), 线性过滤对预乘值本来就是正确的,
 *     再画一遍四边形反而要小心"别又乘一次 alpha"。
 *
 * 入口点为什么自己加载: opengl32.dll 只导出 GL 1.1, FBO/blit 是 3.0+ 的入口, 必须
 * 走 SDL_GL_GetProcAddress。这里刻意不塞进 BongoCatGL 那张共享表: 那张表的契约是
 * "任何成员为 NULL 就整体不可用", 而渲染精度是可选功能 —— 老驱动缺这几个入口时应该
 * 退回"直接按窗口尺寸渲染", 不该把整个 GL 后端判死。
 */
#include "bongo_cat/gl_scale.h"

#include "bongo_cat/config.h"
#include "bongo_cat/gl_api.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <math.h>
#include <stddef.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif

/*
 * 离屏目标的总像素上限: 4×4K 量级 (~33M 像素)。再往上只是把显存换成看不出来的锐度,
 * 而 4.0 × 4K 窗口是 132M 像素 (一张 RGBA8 就是 530MB), 必须在这里拦住。
 */
#define BONGO_CAT_GL_SCALE_MAX_PIXELS (4096u * 2160u * 4u)

typedef struct GlScaleApi {
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC framebuffer_texture_2d;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC framebuffer_renderbuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer_status;
    PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers;
    PFNGLGENRENDERBUFFERSPROC gen_renderbuffers;
    PFNGLBINDRENDERBUFFERPROC bind_renderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC renderbuffer_storage;
    PFNGLDELETERENDERBUFFERSPROC delete_renderbuffers;
    PFNGLBLITFRAMEBUFFERPROC blit_framebuffer;
    bool ready;
    bool failed;
} GlScaleApi;

static GlScaleApi api;
static GLuint scale_framebuffer;
static GLuint scale_texture;
static int scale_width;
static int scale_height;
/*
 * 把离屏结果画回默认帧缓冲用的最小程序。为什么不用 glBlitFramebuffer: 窗口是按
 * MSAA 建的 (SDL_GL_MULTISAMPLEBUFFERS, 见 window.c), 而"单采样 -> 多重采样"的
 * blit 是 GL_INVALID_OPERATION —— 实测就是"设了 2.0 之后窗口一片空"。所以这里改成
 * 画一个全屏三角形采样离屏纹理 (线性过滤由纹理参数给), 目标是不是 MSAA 都成立。
 */
static BongoCatGL scale_gl;
static GLuint scale_program;
static GLuint scale_vao;
static GLint scale_sampler_location;
static bool scale_program_failed;
/* 上一次已经告警过的"自动降级"结论, 避免每帧刷屏 */
static float warned_factor;

static bool load_api(void) {
    if (api.ready) return true;
    if (api.failed) return false;
    api.gen_framebuffers = (PFNGLGENFRAMEBUFFERSPROC)
        SDL_GL_GetProcAddress("glGenFramebuffers");
    api.bind_framebuffer = (PFNGLBINDFRAMEBUFFERPROC)
        SDL_GL_GetProcAddress("glBindFramebuffer");
    api.framebuffer_texture_2d = (PFNGLFRAMEBUFFERTEXTURE2DPROC)
        SDL_GL_GetProcAddress("glFramebufferTexture2D");
    api.framebuffer_renderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)
        SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    api.check_framebuffer_status = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)
        SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    api.delete_framebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)
        SDL_GL_GetProcAddress("glDeleteFramebuffers");
    api.gen_renderbuffers = (PFNGLGENRENDERBUFFERSPROC)
        SDL_GL_GetProcAddress("glGenRenderbuffers");
    api.bind_renderbuffer = (PFNGLBINDRENDERBUFFERPROC)
        SDL_GL_GetProcAddress("glBindRenderbuffer");
    api.renderbuffer_storage = (PFNGLRENDERBUFFERSTORAGEPROC)
        SDL_GL_GetProcAddress("glRenderbufferStorage");
    api.delete_renderbuffers = (PFNGLDELETERENDERBUFFERSPROC)
        SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    api.blit_framebuffer = (PFNGLBLITFRAMEBUFFERPROC)
        SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!api.gen_framebuffers || !api.bind_framebuffer ||
        !api.framebuffer_texture_2d || !api.framebuffer_renderbuffer ||
        !api.check_framebuffer_status || !api.delete_framebuffers ||
        !api.gen_renderbuffers || !api.bind_renderbuffer ||
        !api.renderbuffer_storage || !api.delete_renderbuffers ||
        !api.blit_framebuffer) {
        api.failed = true;
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 渲染精度不可用: 当前 OpenGL 驱动缺 FBO/blit 入口, "
            "已退回按窗口尺寸直接渲染");
        return false;
    }
    api.ready = true;
    return true;
}

/*
 * 编译"把离屏纹理画回窗口"的程序 (只做一次)。返回 false 表示这台机器做不了渲染精度,
 * 调用方要退回"直接按窗口尺寸渲染" —— 绝不能在已经开始离屏渲染之后才发现没有程序,
 * 那样这一帧就没有任何东西会被呈现。
 */
static bool ensure_program(void) {
    if (scale_program) return true;
    if (scale_program_failed) return false;
    BongoCatError error = {0};
    if (!bongo_cat_gl_load(&scale_gl, &error)) {
        scale_program_failed = true;
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 渲染精度不可用: 加载 OpenGL 入口点失败 (%s), "
            "已退回按窗口尺寸直接渲染", error.message);
        return false;
    }
    /* 全屏三角形: 不需要顶点缓冲, 三个顶点由 gl_VertexID 现算 */
    static const char *vertex_source =
        "#version 330 core\n"
        "out vec2 uv;\n"
        "void main(){\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  uv = p;\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    /* 纹理里已经是预乘 alpha 的颜色, 原样输出 (线性过滤对预乘值是正确的) */
    static const char *fragment_source =
        "#version 330 core\n"
        "in vec2 uv; out vec4 color; uniform sampler2D image;\n"
        "void main(){ color = texture(image, uv); }\n";
    scale_program = bongo_cat_gl_program(&scale_gl, vertex_source,
        fragment_source, &error);
    if (!scale_program) {
        scale_program_failed = true;
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 渲染精度不可用: 降采样着色器编译失败 (%s), "
            "已退回按窗口尺寸直接渲染", error.message);
        return false;
    }
    scale_sampler_location = scale_gl.uniform_location(scale_program, "image");
    scale_gl.gen_vertex_arrays(1, &scale_vao);
    return scale_vao != 0;
}

/* 释放离屏目标 (保留 API 表与着色器程序, 下一次 begin 会重建纹理)。 */
static void release_target(void) {
    if (!api.ready) return;
    if (scale_framebuffer) {
        api.delete_framebuffers(1, &scale_framebuffer);
        scale_framebuffer = 0;
    }
    if (scale_texture) {
        glDeleteTextures(1, &scale_texture);
        scale_texture = 0;
    }
    scale_width = scale_height = 0;
}

/* 按目标尺寸建 (或复用) 离屏目标; 失败返回 false。 */
static bool ensure_target(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (scale_framebuffer && scale_width == width && scale_height == height)
        return true;
    release_target();
    glGenTextures(1, &scale_texture);
    if (!scale_texture) return false;
    glBindTexture(GL_TEXTURE_2D, scale_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, NULL);
    /* 采样参数只影响"把纹理当贴图画"的用法; 这里的降采样走 blit, 但设上线性
       过滤能让将来若改成"画四边形"时也不会是硬边。 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    /* 窗口本身是 SDL_GL_DEPTH_SIZE 0 建的 (2D 场景不用深度), 离屏目标也不挂深度,
       两者附件集合一致, 不会出现"帧缓冲不完整"。 */
    api.gen_framebuffers(1, &scale_framebuffer);
    if (!scale_framebuffer) { release_target(); return false; }
    api.bind_framebuffer(GL_FRAMEBUFFER, scale_framebuffer);
    api.framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, scale_texture, 0);
    GLenum status = api.check_framebuffer_status(GL_FRAMEBUFFER);
    api.bind_framebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 渲染精度离屏目标不完整 (status=0x%x, %dx%d), "
            "退回按窗口尺寸直接渲染", (unsigned)status, width, height);
        release_target();
        return false;
    }
    scale_width = width;
    scale_height = height;
    return true;
}

/*
 * 把"请求倍率"收成"这台机器真的能做的倍率":
 *   - 单边不能超过 GL_MAX_TEXTURE_SIZE (默认 16384, 4K 窗口 4.0x 是 15360, 卡得住);
 *   - 总像素不能超过 BONGO_CAT_GL_SCALE_MAX_PIXELS。
 * 降级只告警一次 (按结果值去重), 免得每帧一条。
 */
static float fit_factor(float requested, int width, int height) {
    if (width <= 0 || height <= 0) return 1.0f;
    float factor = requested;
    if (!isfinite(factor) || factor < BONGO_CAT_RENDER_SCALE_MIN)
        factor = BONGO_CAT_RENDER_SCALE_MIN;
    if (factor > BONGO_CAT_RENDER_SCALE_MAX) factor = BONGO_CAT_RENDER_SCALE_MAX;
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (maximum > 0) {
        const float limit = (float)maximum / (float)(width > height ? width : height);
        if (factor > limit) factor = limit;
    }
    const double pixels = (double)width * factor * (double)height * factor;
    if (pixels > (double)BONGO_CAT_GL_SCALE_MAX_PIXELS) {
        const double shrink = sqrt((double)BONGO_CAT_GL_SCALE_MAX_PIXELS /
            ((double)width * (double)height));
        if (shrink < (double)factor) factor = (float)shrink;
    }
    /* 小于 1.0 的倍率也能"降级": 半分辨率渲染是省资源的方向, 不该被上面两条砍掉;
       但 0.1 以下没有意义 (窗口只有几十像素), 由 config 的 clamp 保证。 */
    if (!(fabsf(factor - requested) <= 0.001f) &&
        !(fabsf(factor - warned_factor) <= 0.001f)) {
        warned_factor = factor;
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 渲染精度 %.2f 超出这台机器的上限, 本帧改用 %.2f "
            "(窗口 %dx%d, GL_MAX_TEXTURE_SIZE=%d, 像素上限 %u)",
            (double)requested, (double)factor, width, height, (int)maximum,
            (unsigned)BONGO_CAT_GL_SCALE_MAX_PIXELS);
    }
    return factor;
}

float bongo_cat_gl_scale_begin(float requested, int width, int height) {
    if (width <= 0 || height <= 0) return 1.0f;
    if (!isfinite(requested) || fabsf(requested - 1.0f) <= 0.001f) return 1.0f;
    if (!load_api()) return 1.0f;
    /* 程序必须在绑离屏之前就绪: 没有它就画不回窗口, 等于这一帧什么都不呈现。 */
    if (!ensure_program()) return 1.0f;
    const float factor = fit_factor(requested, width, height);
    const int target_width = (int)lroundf((float)width * factor);
    const int target_height = (int)lroundf((float)height * factor);
    if (target_width <= 0 || target_height <= 0) return 1.0f;
    /* 目标尺寸和窗口一致就没必要绕离屏 (例如 0.98 被 GPU 上限压回 1.0) */
    if (target_width == width && target_height == height) return 1.0f;
    if (!ensure_target(target_width, target_height)) return 1.0f;
    api.bind_framebuffer(GL_FRAMEBUFFER, scale_framebuffer);
    glViewport(0, 0, target_width, target_height);
    return factor;
}

void bongo_cat_gl_scale_viewport(float factor, int x, int y, int width,
    int height) {
    if (factor > 0.0f && !(fabsf(factor - 1.0f) <= 0.001f)) {
        glViewport((GLint)lroundf((float)x * factor),
            (GLint)lroundf((float)y * factor),
            (GLsizei)(lroundf((float)width * factor) > 0 ?
                lroundf((float)width * factor) : 1),
            (GLsizei)(lroundf((float)height * factor) > 0 ?
                lroundf((float)height * factor) : 1));
        return;
    }
    glViewport(x, y, width, height);
}

void bongo_cat_gl_scale_end(float factor, int width, int height) {
    if (!api.ready || !scale_framebuffer || !scale_program) return;
    if (fabsf(factor - 1.0f) <= 0.001f) return;
    api.bind_framebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    /* 直接覆盖整窗口, 不要混合 (颜色已经是预乘值, 混一次会再乘一次 alpha) */
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    scale_gl.use_program(scale_program);
    scale_gl.bind_vertex_array(scale_vao);
    scale_gl.active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scale_texture);
    scale_gl.uniform_1i(scale_sampler_location, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    scale_gl.bind_vertex_array(0);
    scale_gl.use_program(0);
}

void bongo_cat_gl_scale_release(void) {
    if (!api.ready) return;
    release_target();
}
