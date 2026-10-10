#ifndef BONGO_CAT_PREFERENCES_PAGE_DISPATCH_H
#define BONGO_CAT_PREFERENCES_PAGE_DISPATCH_H

#include "preferences_state.h"

/*
 * 页面分发 (实现在 preferences_page_dispatch.c)。
 * 契约: 调用方必须已经进入"当前页"的 nk_group (页面内容画在这个组里, 由 render.c 负责),
 * 并且已经用 bongo_cat_preferences_page_cache_clear 处理过切页 —— 分发本身不做任何
 * 生命周期管理。
 */
void bongo_cat_preferences_draw_page(BongoCatPreferences *value,
    struct nk_context *context);

#endif
