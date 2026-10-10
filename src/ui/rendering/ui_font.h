#ifndef BONGO_CAT_UI_FONT_H
#define BONGO_CAT_UI_FONT_H

#include <stddef.h>
#include <stdbool.h>

/* 在系统里找一个可用字体文件。成功时返回 path 自身, 失败返回 NULL。
   path/capacity 由调用方提供 (推荐 512 以上, Windows 的 %WINDIR% 展开后不短)。 */
const char *bongo_cat_ui_system_font(char *path, size_t capacity, bool multilingual);
const char *bongo_cat_ui_system_heading_font(char *path, size_t capacity,
    bool multilingual);
const char *bongo_cat_ui_system_korean_font(char *path, size_t capacity);
const char *bongo_cat_ui_system_korean_heading_font(char *path, size_t capacity);

#endif
