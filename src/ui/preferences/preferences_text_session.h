#ifndef BONGO_CAT_PREFERENCES_TEXT_SESSION_H
#define BONGO_CAT_PREFERENCES_TEXT_SESSION_H

#include "nuklear_config.h"
#include "bongo_cat/common.h"

#include <SDL3/SDL_events.h>
#include <stdbool.h>

typedef struct BongoCatPreferencesTextSession {
    char id[BONGO_CAT_BEHAVIOR_ID_CAP];
    char text[BONGO_CAT_ID_CAP];
    struct nk_rect bounds;
    size_t cursor;
    bool select_all;
} BongoCatPreferencesTextSession;

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
