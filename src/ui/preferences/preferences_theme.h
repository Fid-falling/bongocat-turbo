#ifndef BONGO_CAT_PREFERENCES_THEME_H
#define BONGO_CAT_PREFERENCES_THEME_H

#include "nuklear_config.h"

#include <stdbool.h>

int bongo_cat_pref_theme(struct nk_context *context, const char *id,
    const char *title, const char *const *labels, int selected);
/* "无限" 那一段的文案由调用方翻译后传进来 (options 里只有它是词, 其余是数字) */
int bongo_cat_pref_fps(struct nk_context *context, const char *id,
    const char *title, int fps, const char *unlimited_label);
/* 返回 BongoCatRenderer 的序号 (0=Auto 1=OpenGL 2=Vulkan)。
   vulkan_selectable 为假时 Vulkan 段会画成禁用态、点不动选中值, 但点击仍会通过
   clicked 上报 (可以是 NULL) —— 调用方要用它给"点了没反应"留日志与提示。 */
int bongo_cat_pref_renderer(struct nk_context *context, const char *id,
    const char *title, int renderer, bool vulkan_selectable, int *clicked);
/*
 * 页内顶部的切页条, 样式与模型页顶部那条一致 (选中/悬停动画走 ui_animation 那一套)。
 * 返回新的选中段号, 调用方负责把它翻译成页号; id 必须是字符串字面量。
 * interactive 为假 (模态弹窗期间) 时只画不响应点击。调用方按 50 预留行高。
 */
int bongo_cat_pref_page_tabs(struct nk_context *context, const char *id,
    const char *const *labels, int count, int selected, bool interactive);
/*
 * 分段控件的两块内部实现 (定义在 preferences_segment.c, 只给 preferences_theme.c 用):
 * icon 画那三个手绘特效图标 (0=电脑 1=太阳 2=月亮);
 * click 回答"用户这一帧点在本控件上了吗", 状态按 id 分开存, id 必须是字符串字面量。
 */
void bongo_cat_pref_segment_icon(struct nk_command_buffer *canvas, int icon,
    struct nk_rect bounds, struct nk_color color, struct nk_color background);
bool bongo_cat_pref_segment_click(struct nk_context *context, const char *id,
    bool hover);

#endif
