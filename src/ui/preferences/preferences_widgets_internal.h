#ifndef BONGO_CAT_PREFERENCES_WIDGETS_INTERNAL_H
#define BONGO_CAT_PREFERENCES_WIDGETS_INTERNAL_H

#include <stdbool.h>
#include "nuklear_config.h"

/*
 * 表单骨架的内部接口 (只在 preferences_* 之间用, 不是对外 API)。
 * FormStyle 是"被改过的全局样式"的备份 —— form_begin 填, form_end 还原。
 * 任何走 form_begin 的代码, 都必须在同一条路径上配对 form_end (裁剪早退那条路除外,
 * 见 form_begin 的返回值约定)。
 */
typedef struct FormStyle {
    struct nk_style_item background;
    struct nk_color window_color;
    struct nk_color border_color;
    struct nk_vec2 padding;
    struct nk_vec2 spacing;
    float border;
} FormStyle;

bool bongo_cat_pref_form_begin(struct nk_context *context, const char *id,
    int lines, FormStyle *saved);
void bongo_cat_pref_form_end(struct nk_context *context,
    const FormStyle *saved);
void bongo_cat_pref_form_label(struct nk_context *context, const char *title);
void bongo_cat_pref_row_icon_clear(struct nk_context *context);
int bongo_cat_pref_detail_lines(const struct nk_context *context,
    const char *text);
void bongo_cat_pref_form_title_sized(struct nk_context *context,
    const char *title, float control_width);
void bongo_cat_pref_description(struct nk_context *context,
    const char *text, int lines);

#endif
