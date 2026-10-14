#ifndef BONGO_CAT_PREFERENCES_WIDGETS_H
#define BONGO_CAT_PREFERENCES_WIDGETS_H

#include <stdbool.h>
#include "bongo_cat/config.h"
#include "nuklear_config.h"
#include "preferences_icons.h"

/*
 * 表单行的公开接口 (实现在 preferences_widgets.c / preferences_widgets_compound.c)。
 * 契约: id 必须在设置窗口内唯一且跨帧稳定 (控件内部按 id 记草稿与拖拽状态);
 * 每个函数都自带标题/说明的排版, 调用方不要再套外层布局。
 * 返回 bool 的是"值变了", 返回 int 的是新的选项下标/数值 —— 调用方据此决定要不要
 * 写 settings、标 app->dirty 或调用对应的副作用函数。
 */
void bongo_cat_pref_section(struct nk_context *context, const char *title);
void bongo_cat_pref_section_icon(struct nk_context *context,
    const char *title, BongoCatPrefIcon icon);
void bongo_cat_pref_row_icon(struct nk_context *context,
    BongoCatPrefIcon icon);
bool bongo_cat_pref_toggle(struct nk_context *context, const char *id,
    const char *title, const char *description, bool *value);
bool bongo_cat_pref_toggle_float(struct nk_context *context, const char *id,
    const char *title, bool *enabled, float minimum, float *value,
    float maximum, float step, float default_value);
bool bongo_cat_pref_toggle_slider(struct nk_context *context, const char *id,
    const char *title, const char *description, bool *enabled,
    float minimum, float *value, float maximum, float step,
    float default_value, const char *suffix);
bool bongo_cat_pref_obs_background(struct nk_context *context, const char *id,
    const char *title, const char *question, const char *reply, bool *enabled,
    BongoCatObsBackgroundColor *color);
bool bongo_cat_pref_float(struct nk_context *context, const char *id,
    const char *title, const char *description, float minimum, float *value,
    float maximum, float step, float default_value);
bool bongo_cat_pref_int(struct nk_context *context, const char *id,
    const char *title, const char *description, int minimum, int *value,
    int maximum, int step, int default_value);
bool bongo_cat_pref_slider(struct nk_context *context, const char *id,
    const char *title, const char *description, float minimum, float *value,
    float maximum, float step, float default_value);
int bongo_cat_pref_combo(struct nk_context *context, const char *id,
    const char *title, const char *description, const char *const *items,
    int count, int selected);
int bongo_cat_pref_edit(struct nk_context *context, const char *id,
    const char *title, const char *description, const char *value,
    bool recording, const char *idle_hint, const char *record_hint);
bool bongo_cat_pref_button(struct nk_context *context, const char *id,
    const char *title, const char *description, const char *button);
void bongo_cat_pref_status(struct nk_context *context, const char *id,
    const char *title, const char *description);

#endif