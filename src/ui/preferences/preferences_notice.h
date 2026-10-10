#ifndef BONGO_CAT_PREFERENCES_NOTICE_H
#define BONGO_CAT_PREFERENCES_NOTICE_H

#include "bongo_cat/app.h"
#include "bongo_cat/preferences.h"
#include "nuklear_config.h"

/*
 * 浮动提示条 (toast) 的接口。
 * show 可以从任意线程语义的位置调用 (窗口可能还没建), 内部会检查 preferences 是否存在;
 * 同一时刻最多显示 4 条, 超出时最旧的会被顶掉; draw 负责计时回收, 所以不需要额外的
 * 定时器。error 为真时显示成危险色。
 */
void bongo_cat_preferences_notice_show(BongoCatApp *app,
    const char *message, bool error);
void bongo_cat_preferences_notice_draw(BongoCatPreferences *preferences,
    struct nk_context *context, float width, float height);

#endif
