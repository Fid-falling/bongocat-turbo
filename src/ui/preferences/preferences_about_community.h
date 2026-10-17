#ifndef BONGO_CAT_PREFERENCES_ABOUT_COMMUNITY_H
#define BONGO_CAT_PREFERENCES_ABOUT_COMMUNITY_H

#include "preferences_internal.h"

/*
 * 社区区块 (只有 QQ 群) 与"更多应用"标题。
 * localized_link 的 animation_id 必须每个链接各不相同, 否则多个链接会共用一个悬停动画。
 */
void bongo_cat_preferences_about_community(
    BongoCatPreferences *value, struct nk_context *context);
void bongo_cat_preferences_about_projects_heading(
    BongoCatPreferences *value, struct nk_context *context,
    struct nk_rect bounds);
void bongo_cat_preferences_about_localized_link(
    BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *key, const char *fallback, const char *url,
    const char *animation_id, BongoCatUIPalette palette);

#endif
