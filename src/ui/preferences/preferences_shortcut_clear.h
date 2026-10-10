#ifndef BONGO_CAT_PREFERENCES_SHORTCUT_CLEAR_H
#define BONGO_CAT_PREFERENCES_SHORTCUT_CLEAR_H

#include "ui_catime.h"

/*
 * 快捷键的"清除"小按钮。返回真表示用户点了它。
 * enabled 为假时既不绘制也不参与命中 (不只是画成灰色) —— 避免误触。
 */
bool bongo_cat_pref_shortcut_clear(struct nk_context *context,
    struct nk_command_buffer *canvas, const char *id, struct nk_rect bounds,
    BongoCatUIPalette palette, float opacity, bool enabled);

#endif
