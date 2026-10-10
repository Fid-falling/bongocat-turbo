/* 窗口背景与圆角遮罩: OBS 背景模式的清屏颜色, 以及 OpenGL 路径的圆角实现。取色函数由
 * 两条渲染后端共用 (OpenGL 拿去 glClearColor, Vulkan 拿去做渲染通道的 clear 值), 所以
 * 颜色语义必须一致。圆角遮罩在整帧 (含背景与模型) 画完之后做一次全屏 alpha 相乘, Vulkan
 * 侧另有 vk_mask_corners, 两边必须给出同样的覆盖率曲线。
 * 调用时序: clear_background 在每帧绘制之前, mask_corners 在绘制之后、呈现之前。
 * 易改错: 遮罩着色器按 gl_FragCoord 算覆盖率, 依赖视口等于整个窗口 (0,0,width,height)。
 */
#include "runtime.h"
#include "bongo_cat/gl_api.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL_opengl.h>

/* 圆角遮罩的 GL 对象是进程级单例 (一个进程只有一个主窗口)。初始化失败后 corner_failed
   会一直挡住重试: 失败通常意味着驱动缺东西, 每帧重试只会白刷日志。 */
static BongoCatGL corner_gl;
static GLuint corner_program, corner_vao;
static GLint corner_width, corner_height, corner_radius;
static PFNGLBLENDEQUATIONSEPARATEPROC corner_equation_separate;
static bool corner_failed;

/* 必须在 GL 上下文 current 时调用 (window.c 的销毁路径会先 MakeCurrent); 这里也清掉
   corner_failed, 让下次能真正重新初始化。 */
void bongo_cat_window_destroy_corner_mask(void) {
    if (corner_vao) corner_gl.delete_vertex_arrays(1, &corner_vao);
    if (corner_program) corner_gl.delete_program(corner_program);
    corner_vao = corner_program = 0;
    corner_failed = false;
}

/* 懒初始化: 首次用到才编译着色器并申请空 VAO —— 三角形的三个顶点由 gl_VertexID 在顶点
   着色器里算出来, 不需要顶点缓冲。glBlendEquationSeparate 走 SDL 取址, 因为它不在
   Windows 的 GL 1.1 导出表里; 取不到就当作不支持, 放弃圆角。 */
static bool prepare_corner_mask(void) {
    if (corner_program) return true;
    if (corner_failed) return false;
    const char *vertex =
        "#version 330 core\n"
        "void main() {\n"
        " vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char *fragment =
        "#version 330 core\n"
        "uniform int width, height, radiusMilli;\n"
        "out vec4 color;\n"
        "void main() {\n"
        " vec2 halfSize = vec2(width, height) * 0.5;\n"
        " float radius = float(radiusMilli) * 0.001;\n"
        " vec2 q = abs(gl_FragCoord.xy - halfSize) - (halfSize - radius);\n"
        " float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;\n"
        " float coverage = clamp(0.5 - d, 0.0, 1.0);\n"
        " color = vec4(coverage);\n"
        "}\n";
    BongoCatError error = {0};
    corner_equation_separate = (PFNGLBLENDEQUATIONSEPARATEPROC)
        SDL_GL_GetProcAddress("glBlendEquationSeparate");
    if (corner_equation_separate && bongo_cat_gl_load(&corner_gl, &error))
        corner_program = bongo_cat_gl_program(&corner_gl, vertex, fragment, &error);
    if (corner_program) {
        corner_gl.gen_vertex_arrays(1, &corner_vao);
        if (corner_vao) {
            corner_width = corner_gl.uniform_location(corner_program, "width");
            corner_height = corner_gl.uniform_location(corner_program, "height");
            corner_radius = corner_gl.uniform_location(corner_program, "radiusMilli");
            return true;
        }
        bongo_cat_window_destroy_corner_mask();
    }
    corner_failed = true;
    SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Cannot initialize window corners: %s",
        error.message);
    return false;
}

/* 把圆角覆盖率乘进 RGB 与 alpha (glBlendFunc(GL_ZERO, GL_SRC_ALPHA)), 因此必须在整帧画完
   之后调用: 它连 OBS 背景一起乘。关闭圆角或半径为 0 时完全不碰 GPU 状态。函数自己保存并
   恢复会被改动的状态 (程序、VAO、混合方程与因子、颜色掩码、五个能力开关), 可以安全地夹在
   其它绘制之间。 */
void bongo_cat_window_mask_corners(BongoCatApp *app, int width, int height) {
    if (!app->settings.window.rounded_corners ||
        app->settings.window.corner_radius_percent <= 0.0f ||
        width <= 0 || height <= 0) return;
    float percent = SDL_clamp(app->settings.window.corner_radius_percent, 0.0f, 50.0f);
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        bongo_cat_vk_mask_corners(bongo_cat_vk_current(), width, height, percent);
        return;
    }
    if (!prepare_corner_mask()) return;
    GLint program, vao, equation_rgb, equation_alpha, src_rgb, dst_rgb,
        src_alpha, dst_alpha;
    GLboolean color_mask[4];
    const GLenum capabilities[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
        GL_SCISSOR_TEST, GL_CULL_FACE};
    GLboolean enabled[SDL_arraysize(capabilities)];
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equation_alpha);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    for (size_t i = 0; i < SDL_arraysize(capabilities); ++i) {
        enabled[i] = glIsEnabled(capabilities[i]);
        if (i == 0) glEnable(capabilities[i]);
        else glDisable(capabilities[i]);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    corner_gl.blend_equation(GL_FUNC_ADD);
    /* Multiply premultiplied RGB and alpha together, including the solid
       capture background, before either native presentation path reads it. */
    glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
    corner_gl.use_program(corner_program);
    corner_gl.bind_vertex_array(corner_vao);
    corner_gl.uniform_1i(corner_width, width);
    corner_gl.uniform_1i(corner_height, height);
    corner_gl.uniform_1i(corner_radius,
        (GLint)(SDL_min(width, height) * percent * 10.0f + 0.5f));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    corner_gl.bind_vertex_array((GLuint)vao);
    corner_gl.use_program((GLuint)program);
    corner_equation_separate(equation_rgb, equation_alpha);
    corner_gl.blend_func_separate(src_rgb, dst_rgb, src_alpha, dst_alpha);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    for (size_t i = 0; i < SDL_arraysize(capabilities); ++i) {
        if (enabled[i]) glEnable(capabilities[i]);
        else glDisable(capabilities[i]);
    }
}

/*
 * 背景色是两条渲染路径共用的状态: OpenGL 用它 glClearColor, Vulkan 用它做
 * 渲染通道的 clear 值。这里只算颜色, 不碰任何 GPU 状态。
 */
/* 每帧都会调用, 所以日志只在"开关或颜色变化"时打一次 (静态变量缓存上一次的值); 函数只读
   app, 不分配、不碰 GPU。 */
void bongo_cat_window_background_rgba(const BongoCatApp *app, float rgba[4]) {
    const BongoCatWindowPreferences *window = &app->settings.window;
    uint32_t rgb = bongo_cat_obs_background_color_rgb(window->obs_background_color);
    float alpha = window->obs_background ? 1.0f : 0.0f;
    rgba[0] = window->obs_background ? ((rgb >> 16) & 255) / 255.0f : 0.0f;
    rgba[1] = window->obs_background ? ((rgb >> 8) & 255) / 255.0f : 0.0f;
    rgba[2] = window->obs_background ? (rgb & 255) / 255.0f : 0.0f;
    rgba[3] = alpha;
    static int last_enabled = -1;
    static BongoCatObsBackgroundColor last_color =
        BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT;
    if (last_enabled != (window->obs_background ? 1 : 0) ||
        last_color != window->obs_background_color) {
        last_enabled = window->obs_background;
        last_color = window->obs_background_color;
        SDL_Log("OBS background mode: enabled=%d color=%s",
            window->obs_background,
            bongo_cat_obs_background_color_name(window->obs_background_color));
    }
}

/* 仅 OpenGL 路径使用 (Vulkan 的 clear 值在渲染通道里设置); 两边都从 background_rgba 取值,
   改颜色语义时不要只改一边。 */
void bongo_cat_window_clear_background(BongoCatApp *app) {
    float rgba[4];
    bongo_cat_window_background_rgba(app, rgba);
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}
