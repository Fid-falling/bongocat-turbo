#ifndef BONGO_CAT_PREFERENCES_ICONS_INTERNAL_H
#define BONGO_CAT_PREFERENCES_ICONS_INTERNAL_H

#include "preferences_icons.h"

bool bongo_cat_pref_section_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon, struct nk_color color);
bool bongo_cat_pref_row_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon, struct nk_color color);

#endif
