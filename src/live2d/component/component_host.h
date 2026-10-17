#ifndef BONGO_CAT_COMPONENT_HOST_H
#define BONGO_CAT_COMPONENT_HOST_H

#include "bongo_cat/renderer_module_abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 这些函数定义在 component_host.c(C 链接), 但会被 C++ 的桥接代码经 component_sdl_shim.h
   的宏调用, 所以头文件必须显式声明 C 链接 —— 否则 C++ 侧会去找修饰名, 链接期才报
   "无法解析的外部符号"(实测踩到过)。 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 组件侧的宿主适配层(实现见 component_host.c)。这些函数由 component_sdl_shim.h 的宏映射
 * 与桥接直接引用的宿主符号共同使用, 不是给外部调用的 API。
 */
void bongo_component_bind(const BongoCatRendererModuleHost *host);
const BongoCatRendererModuleHost *bongo_component_host(void);

/* SDL 的替身 */
void bongo_component_log(int priority, const char *format, ...);
uint64_t bongo_component_ticks_ns(void);
void *bongo_component_allocate(size_t size);
void bongo_component_release(void *memory);
void *bongo_component_read_file(const char *path, size_t *size);
const char *bongo_component_base_path(void);
const char *bongo_component_error(void);
void *bongo_component_gl_current_window(void);
void *bongo_component_gl_current_context(void);
bool bongo_component_gl_make_current(void *window, void *context);
bool bongo_component_window_size_pixels(void *window, int *width, int *height);
/* 模型图集工作尺寸上限: 转发宿主的服务 (上限在进程内只该有一处判定, 见 ABI 的说明)。 */
int bongo_cat_image_model_texture_limit(void);

#ifdef __cplusplus
}
#endif

#endif
