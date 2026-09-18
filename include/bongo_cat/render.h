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
 * 阶段 1: OpenGL 后端 + Vulkan 的编译期能力与设置项。
 * 阶段 2: Vulkan 后端落地 (设备/交换链/呈现/主窗口绘制)。于是可用性判断不再
 * 只看编译开关, 还要真的探测到可用设备: compiled_in 说的是"这次构建编进来了
 * 没有", available 说的是"这次构建 + 这台机器此刻能不能真的跑起来"。
 * 不可用时 bongo_cat_renderer_resolve() 回退 OpenGL 并把原因写进日志; 运行中
 * 初始化失败则调用 bongo_cat_renderer_fallback() 走同一条回退。
 */

/* 这次构建是否编入了该后端的支持 */
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

/* 已选定的后端在初始化阶段失败: 记录原因并立刻切回 OpenGL */
void bongo_cat_renderer_fallback(BongoCatRenderer failed, const char *reason);

/* 用于日志和设置界面 */
const char *bongo_cat_renderer_description(BongoCatRenderer value);

#ifdef __cplusplus
}
#endif

#endif
