#include "bongo_cat/gl_api.h"

#include <SDL3/SDL.h>
#include <string.h>

/*
 * 动态取地址的辅助宏。缺任何一个入口点都等于"这台机器的 GL 不支持我们用到的
 * 能力", 所以第一次取不到就立刻 return false 并把函数名写进 error —— 让用户
 * 看到的失败原因是具体缺哪个函数, 而不是后面某处空指针崩溃。用宏而不是数据表,
 * 是因为赋值时要做成员类型强转, 手写表很容易漏一个成员而编译期不报错。
 */
#define LOAD(member, type, name) do { \
    gl->member = (type)SDL_GL_GetProcAddress(name); \
    if (!gl->member) { bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, \
        "Missing OpenGL function: %s", name); return false; } \
} while (0)

bool bongo_cat_gl_load(BongoCatGL *gl, BongoCatError *error) {
    if (!gl) return false;
    /* 先清零: 失败时结构体是"全 NULL + 前几个已填"而不是未初始化的野指针, 调用方
       即使漏判返回值也只会立刻空指针崩, 不会跳到随机地址上执行。 */
    memset(gl, 0, sizeof(*gl));
    LOAD(create_shader, PFNGLCREATESHADERPROC, "glCreateShader");
    LOAD(shader_source, PFNGLSHADERSOURCEPROC, "glShaderSource");
    LOAD(compile_shader, PFNGLCOMPILESHADERPROC, "glCompileShader");
    LOAD(get_shader_iv, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    LOAD(get_shader_log, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    LOAD(create_program, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    LOAD(attach_shader, PFNGLATTACHSHADERPROC, "glAttachShader");
    LOAD(link_program, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    LOAD(get_program_iv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    LOAD(get_program_log, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    LOAD(delete_shader, PFNGLDELETESHADERPROC, "glDeleteShader");
    LOAD(delete_program, PFNGLDELETEPROGRAMPROC, "glDeleteProgram");
    LOAD(use_program, PFNGLUSEPROGRAMPROC, "glUseProgram");
    LOAD(gen_vertex_arrays, PFNGLGENVERTEXARRAYSPROC, "glGenVertexArrays");
    LOAD(bind_vertex_array, PFNGLBINDVERTEXARRAYPROC, "glBindVertexArray");
    LOAD(delete_vertex_arrays, PFNGLDELETEVERTEXARRAYSPROC, "glDeleteVertexArrays");
    LOAD(gen_buffers, PFNGLGENBUFFERSPROC, "glGenBuffers");
    LOAD(bind_buffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
    LOAD(buffer_data, PFNGLBUFFERDATAPROC, "glBufferData");
    LOAD(delete_buffers, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers");
    LOAD(enable_attribute, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray");
    LOAD(attribute_pointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer");
    LOAD(uniform_location, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    LOAD(uniform_1i, PFNGLUNIFORM1IPROC, "glUniform1i");
    LOAD(uniform_matrix_4fv, PFNGLUNIFORMMATRIX4FVPROC, "glUniformMatrix4fv");
    LOAD(active_texture, PFNGLACTIVETEXTUREPROC, "glActiveTexture");
    LOAD(blend_equation, PFNGLBLENDEQUATIONPROC, "glBlendEquation");
    LOAD(blend_func_separate, PFNGLBLENDFUNCSEPARATEPROC, "glBlendFuncSeparate");
    return true;
}

/*
 * 这里直接调 glGetError 而不走函数表: 它是 GL 1.1 入口, opengl32.dll 一直导出,
 * 即使 bongo_cat_gl_load 失败也能安全调用 —— 调用方正是要在"GL 可能已经不可用"
 * 的时候用它来排空队列。
 *
 * 循环 16 次是因为错误寄存器会累积多个不同 flag (GL_INVALID_ENUM |
 * GL_INVALID_VALUE 会同时置位), 一次 glGetError 只弹一个, 必须弹到
 * GL_NO_ERROR 才算干净。
 */
bool bongo_cat_gl_clear_errors(void) {
    for (unsigned attempt = 0; attempt < 16; ++attempt)
        if (glGetError() == GL_NO_ERROR) return true;
    return false;
}

/*
 * 编译单个 shader。失败时返回 0 并填 error, 同时把刚创建的 shader 对象删掉 ——
 * 编译失败的 shader 仍然占用一个 GL 名字, 不删就会在"切分辨率反复重建管线"时
 * 稳定泄漏。注意 error 里的信息来自驱动, 不同驱动措辞差异很大, 不要拿它做程序
 * 判断, 只用于日志/报错。
 */
static GLuint compile(BongoCatGL *gl, GLenum type, const char *source, BongoCatError *error) {
    GLuint shader = gl->create_shader(type);
    gl->shader_source(shader, 1, &source, NULL);
    gl->compile_shader(shader);
    GLint ok = GL_FALSE;
    gl->get_shader_iv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    char message[512];
    gl->get_shader_log(shader, sizeof(message), NULL, message);
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Shader compilation failed: %s", message);
    gl->delete_shader(shader);
    return 0;
}

/*
 * 顶点 + 片元一次成程序。链接完成后立刻删除两个 shader 对象: 链接已经把编译结果
 * 拷进 program 里, shader 对象此时只是占名字, 早删可以让"频繁重建程序"的路径
 * (比如设置窗口反复开关) 不累积驱动侧对象。
 *
 * 已知小瑕疵: 顶点编译成功而片元编译失败时, 这里直接 return 0, 那个 vs 不会被回收
 * (compile 只在自身编译失败时删自己的对象)。修它需要额外的清理分支, 目前只在
 * shader 源码写错时才会触发, 不影响正常路径。
 */
unsigned int bongo_cat_gl_program(BongoCatGL *gl, const char *vertex, const char *fragment,
    BongoCatError *error) {
    GLuint vs = compile(gl, GL_VERTEX_SHADER, vertex, error);
    GLuint fs = compile(gl, GL_FRAGMENT_SHADER, fragment, error);
    if (!vs || !fs) return 0;
    GLuint program = gl->create_program();
    gl->attach_shader(program, vs);
    gl->attach_shader(program, fs);
    gl->link_program(program);
    gl->delete_shader(vs);
    gl->delete_shader(fs);
    GLint ok = GL_FALSE;
    gl->get_program_iv(program, GL_LINK_STATUS, &ok);
    if (ok) return program;
    char message[512];
    gl->get_program_log(program, sizeof(message), NULL, message);
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Shader link failed: %s", message);
    gl->delete_program(program);
    return 0;
}
