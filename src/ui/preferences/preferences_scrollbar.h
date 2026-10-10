#ifndef BONGO_CAT_PREFERENCES_SCROLLBAR_H
#define BONGO_CAT_PREFERENCES_SCROLLBAR_H

#include "nuklear_config.h"

#include <stdbool.h>

/*
 * 自绘滚动条的接口 (不是 Nuklear 的 nk_scrollbar)。
 * 契约: 调用方持有偏移量并每帧传进来 (本结构体只记拖拽状态, 不存偏移);
 * 行的可用宽度必须用 scrollbar_content_width 算, 它会预留滚动条的 14px;
 * draw 返回的 changed 为真时调用方才需要保存新偏移并标脏。
 */
typedef struct BongoCatPreferencesScrollbar {
    float grab_y;
    bool dragging;
} BongoCatPreferencesScrollbar;

typedef struct BongoCatPreferencesScrollbarResult {
    float offset;
    bool scrollable;
    bool changed;
} BongoCatPreferencesScrollbarResult;

bool bongo_cat_preferences_scrollbar_needed(
    struct nk_rect viewport, float content_height);
float bongo_cat_preferences_scrollbar_content_width(
    struct nk_rect viewport, float content_height);
void bongo_cat_preferences_scrollbar_reset(
    BongoCatPreferencesScrollbar *scrollbar);
BongoCatPreferencesScrollbarResult bongo_cat_preferences_scrollbar_draw(
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect viewport, float content_height, float offset,
    BongoCatPreferencesScrollbar *scrollbar, struct nk_color cursor_color,
    float opacity, bool enabled);

#endif
