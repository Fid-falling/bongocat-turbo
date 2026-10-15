#ifndef BONGO_CAT_COMPONENT_SDL_SHIM_H
#define BONGO_CAT_COMPONENT_SDL_SHIM_H

/*
 * 组件里的 "SDL 改道表" —— 由构建系统强制包含(-FI / -include), 见 cmake/RendererModule.cmake。
 *
 * 为什么用宏改道而不是改桥接源码: 桥接有上万行、几十处 SDL_* 调用, 逐个改既危险又让 diff
 * 铺满全仓。这里先把 SDL 头文件**原样包含进来**(类型与声明照旧可用), 再用同名宏把函数调用
 * 改到 component_host.c 的转发函数上。于是:
 *   * 桥接源码零改动;
 *   * 组件最终**不链接 SDL**(宏展开后没有任何 SDL 符号被引用), 也就不会出现"进程里两份 SDL,
 *     组件那份看不到宿主窗口/GL 上下文"这种致命的错配。
 *
 * 顺序很重要: 必须先 include 再 define, 否则 SDL 自己的函数声明会被宏破坏。
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
/* 刻意**不**包含 SDL3/SDL_opengl.h: 它会先拉进 GL/gl.h, 之后 glew.h 就会报
   'gl.h included before glew.h'。GL 的入口由宿主头 bongo_cat/gl_api.h 负责(glew)。 */
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <stdlib.h>

#include "component_host.h"

/* 日志: 全部汇到宿主的日志系统(带组件前缀), 组件不自己写文件。 */
#define SDL_Log(...) bongo_component_log(SDL_LOG_PRIORITY_INFO, __VA_ARGS__)
#define SDL_LogInfo(category, ...) \
    bongo_component_log(SDL_LOG_PRIORITY_INFO, __VA_ARGS__)
#define SDL_LogWarn(category, ...) \
    bongo_component_log(SDL_LOG_PRIORITY_WARN, __VA_ARGS__)
#define SDL_LogError(category, ...) \
    bongo_component_log(SDL_LOG_PRIORITY_ERROR, __VA_ARGS__)
#define SDL_LogDebug(category, ...) \
    bongo_component_log(SDL_LOG_PRIORITY_DEBUG, __VA_ARGS__)

/* 时间/内存/文件: 与宿主同源, 避免两套实现给出不同答案。 */
#define SDL_GetTicksNS() bongo_component_ticks_ns()
#define SDL_malloc(size) bongo_component_allocate(size)
#define SDL_free(memory) bongo_component_release(memory)
#define SDL_LoadFile(path, size) bongo_component_read_file(path, size)

/* 路径与环境变量: 组件的"当前位置"没有意义(它只是宿主进程里的一个模块)。
   环境变量例外: getenv 读的是**进程级**环境块, 两份 CRT 看到的是同一份, 所以直接用 C 库的
   实现比多开一个服务更简单, 也不会出现"组件与宿主读到不同的值"。 */
#define SDL_GetBasePath() bongo_component_base_path()
#define SDL_getenv(name) getenv(name)
#define SDL_GetError() bongo_component_error()

/* GL 上下文与窗口: 宿主拥有, 组件只比较句柄、请求切换、问尺寸。 */
#define SDL_GL_GetCurrentWindow() \
    ((SDL_Window *)bongo_component_gl_current_window())
#define SDL_GL_GetCurrentContext() \
    ((SDL_GLContext)bongo_component_gl_current_context())
#define SDL_GL_MakeCurrent(window, context) \
    bongo_component_gl_make_current((void *)(window), (void *)(context))
#define SDL_GetWindowSizeInPixels(window, width, height) \
    bongo_component_window_size_pixels((void *)(window), (width), (height))

#endif
