#ifndef BONGO_CAT_RENDER_H
#define BONGO_CAT_RENDER_H

#include "bongo_cat/common.h"
#include "bongo_cat/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 渲染后端选择。
 *
 * 阶段 1 现状: 只有 OpenGL 后端是实现的。Vulkan 的编译期能力 (SDL_Vulkan_*,
 * headers, 链接) 会随 BONGO_CAT_ENABLE_VULKAN 一起就绪, 但渲染器本身尚未实现,
 * 所以 bongo_cat_renderer_available(BONGO_CAT_RENDERER_VULKAN) 仍然返回 false。
 * 这样用户可以在设置里表达偏好并被正确持久化, 而运行时不会去启动一条走不通的路。
 */

/* 这次构建是否编入了该后端的支持 (不代表后端已实现) */
bool bongo_cat_renderer_compiled_in(BongoCatRenderer value);

/* 该后端此刻是否真的能用 */
bool bongo_cat_renderer_available(BongoCatRenderer value);

/* 不可用的原因, 可用时返回 NULL */
const char *bongo_cat_renderer_unavailable_reason(BongoCatRenderer value);

/* 把请求解析成实际要用的后端: 不可用时回退到 OpenGL 并记录原因 */
BongoCatRenderer bongo_cat_renderer_resolve(BongoCatRenderer requested);

/* 启动时选定一次, 之后渲染/平台层查询 bongo_cat_renderer_active() */
void bongo_cat_renderer_select(BongoCatRenderer requested);
BongoCatRenderer bongo_cat_renderer_active(void);

/* 用于日志和设置界面 */
const char *bongo_cat_renderer_description(BongoCatRenderer value);

#ifdef __cplusplus
}
#endif

#endif
