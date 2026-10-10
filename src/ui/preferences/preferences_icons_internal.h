#ifndef BONGO_CAT_PREFERENCES_ICONS_INTERNAL_H
#define BONGO_CAT_PREFERENCES_ICONS_INTERNAL_H

#include "preferences_icons.h"

/*
 * 两份图标表的内部接口 (实现在 preferences_section_icons.c 与 preferences_row_icons.c)。
 * 两个函数都返回 bool: true 表示"这个图标归我管, 已经画了"。统一入口
 * (preferences_icons.c) 就是靠返回值来分派的, 所以实现里对越界 icon 必须老老实实
 * 返回 false, 不能画任何东西。
 */
bool bongo_cat_pref_section_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon, struct nk_color color);
bool bongo_cat_pref_row_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon, struct nk_color color);

#endif
