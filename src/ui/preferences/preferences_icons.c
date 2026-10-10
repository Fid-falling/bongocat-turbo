/*
 * 图标绘制的统一入口: 先试"段落级图标", 再试"行级图标"。
 * 这样分派的好处是调用方只需要一个 BongoCatPrefIcon 值, 不必知道某个图标属于哪张表;
 * 两张表之间用枚举值的区间划分 (见 preferences_icons_internal.h 的说明), 所以任何
 * 越界的 icon 都会在两边各自被拒绝, 不会画错图。
 */
#include "preferences_icons.h"
#include "preferences_icons_internal.h"

void bongo_cat_pref_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon, struct nk_color color) {
    if (!canvas || icon < 0 || icon >= BONGO_CAT_PREF_ICON_COUNT) return;
    if (bongo_cat_pref_section_icon_draw(canvas, bounds, icon, color)) return;
    bongo_cat_pref_row_icon_draw(canvas, bounds, icon, color);
}
