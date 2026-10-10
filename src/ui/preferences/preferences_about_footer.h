#ifndef BONGO_CAT_PREFERENCES_ABOUT_FOOTER_H
#define BONGO_CAT_PREFERENCES_ABOUT_FOOTER_H

#include "preferences_internal.h"

/*
 * 关于页底部信息条。契约: bounds 是一整行 (高度按 36 算), 里面会按可用宽度决定
 * 并排还是折成两行; canvas 由调用方给, 因为整页共用同一个命令缓冲。
 */
void bongo_cat_preferences_about_footer(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatUIPalette palette);

#endif
