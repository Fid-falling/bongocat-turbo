#ifndef BONGO_CAT_UI_FONT_ATLAS_INTERNAL_H
#define BONGO_CAT_UI_FONT_ATLAS_INTERNAL_H

#include "ui_font_atlas.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct UIFontSource {
    void *data;
    size_t size;
    FILE *file;
    void *mapping;
} UIFontSource;

bool bongo_cat_ui_font_source_load(UIFontSource *source, const char *path);
void bongo_cat_ui_font_source_release(UIFontSource *source);
void bongo_cat_ui_font_detach_source(struct nk_font_atlas *atlas,
    const UIFontSource *source);
struct nk_font *bongo_cat_ui_font_add_family(struct nk_font_atlas *atlas,
    const UIFontSource *primary, const UIFontSource *fallback,
    const UIFontSource *korean_fallback, float size, const nk_rune *all,
    const nk_rune *primary_ranges, const nk_rune *cjk,
    const nk_rune *korean);

bool bongo_cat_ui_font_split_ranges(const nk_rune *ranges,
    nk_rune **primary, nk_rune **cjk, nk_rune **korean);
bool bongo_cat_ui_font_has_ranges(const struct nk_font *font,
    const nk_rune *ranges);

bool bongo_cat_ui_font_upload_atlas(BongoCatUIBackend *ui);

#endif
