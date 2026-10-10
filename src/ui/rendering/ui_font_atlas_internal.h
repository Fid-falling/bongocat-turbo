#ifndef BONGO_CAT_UI_FONT_ATLAS_INTERNAL_H
#define BONGO_CAT_UI_FONT_ATLAS_INTERNAL_H

#include "ui_font_atlas.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * 图集构建的内部接口, 只给 rendering/ 下的实现文件用。
 *
 * UIFontSource 的生命周期固定为三步, 顺序不能变:
 *   load() -> (建图集) -> detach_source() -> release()
 * detach 的意义是在 Nuklear 真正释放 config 之前把 ttf_blob 置空, 免得它去 free
 * 一块 mmap/MapViewOfFile 来的内存 (Windows 上直接崩, Linux 上是 munmap 报错)。
 */
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
