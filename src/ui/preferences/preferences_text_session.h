#ifndef BONGO_CAT_PREFERENCES_TEXT_SESSION_H
#define BONGO_CAT_PREFERENCES_TEXT_SESSION_H

#include "nuklear_config.h"
#include "bongo_cat/common.h"

#include <SDL3/SDL_events.h>
#include <stdbool.h>

/*
 * 文本编辑会话 (实现在 preferences_text_session.c)。
 * id 既标识"哪个输入框在编辑", 也是会话是否存活的判据 (空 = 没有会话);
 * bounds 用的是**逻辑像素**, 而事件里的坐标是物理像素, 所以 event() 要接收
 * layout_scale 做换算; text_inset 是文本相对控件左边缘的内缩, 用于点击定位光标。
 * 会话本身不保存"编辑的是哪块缓冲" —— 目标缓冲由调用方在 begin 前记住, finish 时
 * 自己写回 (这样会话不必知道业务字段的布局)。
 */
typedef struct BongoCatPreferencesTextSession {
    char id[BONGO_CAT_BEHAVIOR_ID_CAP];
    char text[BONGO_CAT_ID_CAP];
    struct nk_rect bounds;
    size_t cursor;
    bool select_all;
} BongoCatPreferencesTextSession;

/* finish 为真时编辑结束, 其中 save 为真表示"保存", 为假表示"放弃 (ESC)"
 * —— 调用方必须先看 finish 再看 save */
typedef struct BongoCatPreferencesTextSessionEvent {
    bool handled;
    bool finish;
    bool save;
} BongoCatPreferencesTextSessionEvent;

bool bongo_cat_preferences_text_session_active(
    const BongoCatPreferencesTextSession *session);
void bongo_cat_preferences_text_session_begin(
    BongoCatPreferencesTextSession *session, const char *id,
    const char *text, struct nk_rect bounds);
void bongo_cat_preferences_text_session_reset(
    BongoCatPreferencesTextSession *session);
BongoCatPreferencesTextSessionEvent bongo_cat_preferences_text_session_event(
    BongoCatPreferencesTextSession *session, const SDL_Event *event,
    float layout_scale, const struct nk_user_font *font, float text_inset);

#endif
