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
/*
 * 偏好设置页顶部的切页条 (实现在 preferences_page_dispatch.c)。返回 true 表示本页有切页条,
 * 调用方要把内容区高度减掉 50。它只切 value->preferences_tab 这个子页下标, 不改 value->page,
 * 所以点「快捷键」不会跳到侧栏那一项的设置内容上 (快捷键子页暂时留空)。
 * interactive 为假时只画不响应点击 (模态期间)。
 */
bool bongo_cat_preferences_subnavigation(BongoCatPreferences *value,
    struct nk_context *context, bool interactive);

#endif
