#include "preferences_text_session.h"

#include "preferences_text_edit.h"

#include <SDL3/SDL_clipboard.h>
#include <stdio.h>
#include <string.h>

static float measure(const void *userdata, const char *text, size_t length) {
    const struct nk_user_font *font = userdata;
    return font->width(font->userdata, font->height, text, (int)length);
}

static void clipboard_event(BongoCatPreferencesTextSession *session,
    SDL_Keycode key) {
    if (key == SDLK_V) {
        char *clipboard = SDL_GetClipboardText();
        bongo_cat_text_edit_insert(session->text, sizeof(session->text),
            &session->cursor, &session->select_all, clipboard);
        SDL_free(clipboard);
        return;
    }
    if (session->select_all) SDL_SetClipboardText(session->text);
    if (key == SDLK_X && session->select_all)
        bongo_cat_text_edit_clear(session->text, &session->cursor,
            &session->select_all);
}

bool bongo_cat_preferences_text_session_active(
    const BongoCatPreferencesTextSession *session) {
    return session && session->id[0];
}

void bongo_cat_preferences_text_session_begin(
    BongoCatPreferencesTextSession *session, const char *id,
    const char *text, struct nk_rect bounds) {
    if (!session) return;
    snprintf(session->id, sizeof(session->id), "%s", id ? id : "");
    snprintf(session->text, sizeof(session->text), "%s", text ? text : "");
    session->bounds = bounds;
    bongo_cat_text_edit_begin(session->text, &session->cursor,
        &session->select_all);
}

void bongo_cat_preferences_text_session_reset(
    BongoCatPreferencesTextSession *session) {
    if (session) memset(session, 0, sizeof(*session));
}

BongoCatPreferencesTextSessionEvent bongo_cat_preferences_text_session_event(
    BongoCatPreferencesTextSession *session, const SDL_Event *event,
    float layout_scale, const struct nk_user_font *font, float text_inset) {
    BongoCatPreferencesTextSessionEvent result = {0};
    if (!bongo_cat_preferences_text_session_active(session) || !event)
        return result;
    if (event->type == SDL_EVENT_TEXT_INPUT) {
        bongo_cat_text_edit_insert(session->text, sizeof(session->text),
            &session->cursor, &session->select_all, event->text.text);
        result.handled = true;
        return result;
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        result.finish = result.save = true;
        return result;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.button == SDL_BUTTON_LEFT) {
        float scale = layout_scale > 0.0f ? layout_scale : 1.0f;
        float x = event->button.x / scale, y = event->button.y / scale;
        if (!NK_INBOX(x, y, session->bounds.x, session->bounds.y,
            session->bounds.w, session->bounds.h)) {
            result.finish = result.save = true;
            return result;
        }
        if (font) session->cursor = bongo_cat_text_edit_nearest(session->text,
            x - session->bounds.x - text_inset, measure, font);
        session->select_all = false;
        result.handled = true;
        return result;
    }
    if (event->type != SDL_EVENT_KEY_DOWN &&
        event->type != SDL_EVENT_KEY_UP) return result;
    result.handled = true;
    if (event->type == SDL_EVENT_KEY_UP) return result;
    SDL_Keycode key = event->key.key;
    bool control = (event->key.mod & SDL_KMOD_CTRL) != 0;
    if (key == SDLK_ESCAPE) {
        result.finish = true;
        return result;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        result.finish = result.save = true;
        return result;
    }
    if (control && key == SDLK_A)
        bongo_cat_text_edit_select_all(session->text, &session->cursor,
            &session->select_all);
    else if (control && (key == SDLK_C || key == SDLK_X || key == SDLK_V))
        clipboard_event(session, key);
    else if (key == SDLK_BACKSPACE || key == SDLK_DELETE)
        bongo_cat_text_edit_erase(session->text, &session->cursor,
            &session->select_all, key == SDLK_DELETE);
    else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_HOME ||
        key == SDLK_END)
        bongo_cat_text_edit_move(session->text, &session->cursor,
            &session->select_all,
            key == SDLK_LEFT ? BONGO_CAT_TEXT_EDIT_LEFT :
            key == SDLK_RIGHT ? BONGO_CAT_TEXT_EDIT_RIGHT :
            key == SDLK_HOME ? BONGO_CAT_TEXT_EDIT_HOME :
            BONGO_CAT_TEXT_EDIT_END);
    return result;
}
