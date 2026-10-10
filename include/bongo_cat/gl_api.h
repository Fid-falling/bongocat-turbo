#ifndef BONGO_CAT_GL_API_H
#define BONGO_CAT_GL_API_H

#include "bongo_cat/common.h"
#include <SDL3/SDL_opengl.h>

/*
 * 动态加载的 OpenGL 入口点表 (OpenGL 后端的公共底座)。
 *
 * 为什么不直接调用 glCreateShader 这些符号: Windows 的 opengl32.dll 只导出
 * GL 1.1 的入口, 着色器/VAO/VBO/统一变量/独立混色这些 2.0+ 的入口一个都没有,
 * 链接期根本找不到符号 (Linux 上由 libGL 提供, 所以这类代码在 Linux 上直接
 * 链接也能过, 一旦搬到 Windows 就整体链接失败)。GL 1.1 的调用
 * (glGetError / glViewport / glBindTexture ...) 不在表里, 保持直接调用。
 *
 * 每个子系统各自持有一份 BongoCatGL (overlay / mver 指针层 / 设置界面 /
 * 轮盘 / 圆角遮罩), 不做全局单例, 原因是:
 *  - Vulkan 后端下没有 GL 上下文, 这些子系统直接跳过加载 (见 overlay.c 的
 *    早退分支), 全局单例反而要额外维护"加载了没有"的状态;
 *  - 各子系统可以独立初始化/销毁, 互不牵连;
 *  - tests/media/test_image_filter.c 这类测试可以脱离应用单独加载。
 *
 * 契约: 加载必须在 GL 上下文已经 current 的线程上进行 —— SDL_GL_GetProcAddress
 * 在 Windows 上最终走 wglGetProcAddress, 没有 current 上下文时对 2.0+ 入口返回
 * NULL, 于是 bongo_cat_gl_load 会"莫名其妙"失败。任何成员为 NULL 都表示这次
 * 加载没成功, 调用方必须整体判断返回值, 不能只看自己关心的那一个成员。
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct BongoCatGL {
    PFNGLCREATESHADERPROC create_shader;
    PFNGLSHADERSOURCEPROC shader_source;
    PFNGLCOMPILESHADERPROC compile_shader;
    PFNGLGETSHADERIVPROC get_shader_iv;
    PFNGLGETSHADERINFOLOGPROC get_shader_log;
    PFNGLCREATEPROGRAMPROC create_program;
    PFNGLATTACHSHADERPROC attach_shader;
    PFNGLLINKPROGRAMPROC link_program;
    PFNGLGETPROGRAMIVPROC get_program_iv;
    PFNGLGETPROGRAMINFOLOGPROC get_program_log;
    PFNGLDELETESHADERPROC delete_shader;
    PFNGLDELETEPROGRAMPROC delete_program;
    PFNGLUSEPROGRAMPROC use_program;
    PFNGLGENVERTEXARRAYSPROC gen_vertex_arrays;
    PFNGLBINDVERTEXARRAYPROC bind_vertex_array;
    PFNGLDELETEVERTEXARRAYSPROC delete_vertex_arrays;
    PFNGLGENBUFFERSPROC gen_buffers;
    PFNGLBINDBUFFERPROC bind_buffer;
    PFNGLBUFFERDATAPROC buffer_data;
    PFNGLDELETEBUFFERSPROC delete_buffers;
    PFNGLENABLEVERTEXATTRIBARRAYPROC enable_attribute;
    PFNGLVERTEXATTRIBPOINTERPROC attribute_pointer;
    PFNGLGETUNIFORMLOCATIONPROC uniform_location;
    PFNGLUNIFORM1IPROC uniform_1i;
    PFNGLUNIFORMMATRIX4FVPROC uniform_matrix_4fv;
    PFNGLACTIVETEXTUREPROC active_texture;
    PFNGLBLENDEQUATIONPROC blend_equation;
    PFNGLBLENDFUNCSEPARATEPROC blend_func_separate;
} BongoCatGL;

/*
 * 一次填满整张表。返回 false 时 error 已填好 ("Missing OpenGL function: xxx"),
 * 可以直接作为"这个后端不可用"的原因透给用户; 返回 false 后结构体里可能只填了
 * 前一半, 调用方不得再用它 (见 gl_api.c 里的 memset)。
 */
bool bongo_cat_gl_load(BongoCatGL *gl, BongoCatError *error);

/*
 * 清空 GL 的错误队列。GL 的错误是队列而不是单个标志, 而且跨子系统共享:
 * Cubism / Nuklear / 图像上传留下的旧错误会被后来的代码误判成"自己刚失败",
 * 所以在关键 GL 操作之前先排空, 之后再用它判断本次是否真的出错。
 * 返回 false = 连续 16 次都没排空, 说明有东西在持续报错, 调用方按"结果不可信"
 * 处理即可, 不要当成致命错误。
 */
bool bongo_cat_gl_clear_errors(void);

/*
 * 编译并链接一个顶点/片元程序。失败时写入 error (编译日志/链接日志) 并返回 0,
 * 链接成功后两个中间 shader 对象由内部回收。返回的非 0 程序归调用方所有, 必须用
 * glDeleteProgram 释放, 否则反复重建时会在驱动里堆积。
 */
unsigned int bongo_cat_gl_program(BongoCatGL *gl, const char *vertex, const char *fragment,
    BongoCatError *error);

#ifdef __cplusplus
}
#endif

#endif
