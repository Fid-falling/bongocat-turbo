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
   vulkan_selectable 为假时 Vulkan 段会画成禁用态且点不动。 */
int bongo_cat_pref_renderer(struct nk_context *context, const char *id,
    const char *title, int renderer, bool vulkan_selectable);

#endif
