#ifndef BONGO_CAT_PREFERENCES_FONTS_H
#define BONGO_CAT_PREFERENCES_FONTS_H

#include "preferences_state.h"

/*
 * 字体解析结果。
 * 六个路径缓冲内嵌在结构体里 (而不是返回裸指针) 是为了让调用方持有它们时生命周期
 * 明确 —— resolve 只是查路径, 真正的加载在 ui_font 里做, 所以路径必须在整段使用期间
 * 有效; ranges 指向 preferences->glyph_ranges (借用, 不拥有)。
 */
typedef struct BongoCatPreferenceFonts {
    char body_path[BONGO_CAT_PATH_CAP];
    char body_fallback_path[BONGO_CAT_PATH_CAP];
    char body_korean_fallback_path[BONGO_CAT_PATH_CAP];
    char heading_path[BONGO_CAT_PATH_CAP];
    char heading_fallback_path[BONGO_CAT_PATH_CAP];
    char heading_korean_fallback_path[BONGO_CAT_PATH_CAP];
    const char *body;
    const char *body_fallback;
    const char *body_korean_fallback;
    const char *heading;
    const char *heading_fallback;
    const char *heading_korean_fallback;
    const nk_rune *ranges;
} BongoCatPreferenceFonts;

void bongo_cat_preferences_fonts_resolve(BongoCatPreferences *preferences,
    BongoCatPreferenceFonts *fonts);

#endif
