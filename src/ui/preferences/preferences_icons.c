#include "preferences_icons.h"
#include "preferences_icons_internal.h"

void bongo_cat_pref_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon, struct nk_color color) {
    if (!canvas || icon < 0 || icon >= BONGO_CAT_PREF_ICON_COUNT) return;
    if (bongo_cat_pref_section_icon_draw(canvas, bounds, icon, color)) return;
    bongo_cat_pref_row_icon_draw(canvas, bounds, icon, color);
}
