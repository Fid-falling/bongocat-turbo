#ifndef BONGO_CAT_PREFERENCES_CONTROLS_H
#define BONGO_CAT_PREFERENCES_CONTROLS_H

#include <stdbool.h>
#include "bongo_cat/config.h"
#include "nuklear_config.h"

/*
 * 数值/开关类控件的实现入口 (实现在 preferences_controls.c)。
 * 契约: id 必须在整个设置窗口内唯一且跨帧稳定 —— 数字编辑的草稿、按住连发、拖拽
 * 都按 id 记账, id 变了等于换了一个控件 (草稿丢失、拖拽中断)。
 */
/* Stable, unique ids retain numeric drafts; valid edits update value immediately.
 * Integer fields accept digits and an optional minus; floats also accept a dot.
 * Values are clamped to the supplied range. Clicking away discards incomplete text.
 */
bool bongo_cat_pref_control_float(struct nk_context *context, const char *id,
    float minimum, float *value, float maximum, float step,
    float default_value);
bool bongo_cat_pref_control_int(struct nk_context *context, const char *id,
    int minimum, int *value, int maximum, int step, int default_value);
bool bongo_cat_pref_control_slider(struct nk_context *context, const char *id,
    float minimum, float *value, float maximum, float step,
    float default_value, const char *suffix);
bool bongo_cat_pref_control_toggle(struct nk_context *context,
    const char *id, bool *value);
bool bongo_cat_pref_control_toggle_rect(struct nk_context *context,
    const char *id, bool *value, struct nk_rect cell, bool enabled);
bool bongo_cat_pref_control_obs_background(struct nk_context *context,
    const char *id, bool *enabled, BongoCatObsBackgroundColor *color);
int bongo_cat_pref_control_combo(struct nk_context *context, const char *id,
    const char *const *items, int count, int selected);
bool bongo_cat_pref_controls_animating(struct nk_context *context);
void bongo_cat_pref_controls_reset(struct nk_context *context);

#endif