#include "preferences_state.h"
#include "preferences_text_edit.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static BongoCatBehaviorShortcut *binding_for(BongoCatSettings *config,
    const char *id) {
    for (size_t i = 0; i < config->behavior_shortcut_count; ++i)
        if (!strcmp(config->behavior_shortcuts[i].id, id))
            return &config->behavior_shortcuts[i];
    if (config->behavior_shortcut_count >= BONGO_CAT_BEHAVIOR_BINDING_CAP)
        return NULL;
    BongoCatBehaviorShortcut *binding =
        &config->behavior_shortcuts[config->behavior_shortcut_count++];
    memset(binding, 0, sizeof(*binding));
    snprintf(binding->id, sizeof(binding->id), "%s", id);
    return binding;
}

static const char *display_label(const BongoCatBehaviorEntry *entry,
    const BongoCatBehaviorShortcut *binding) {
    return binding && binding->label[0] ? binding->label : entry->label;
}

void bongo_cat_preferences_behavior_rename_finish(
    BongoCatPreferences *value, bool save) {
    if (!value || !bongo_cat_preferences_text_session_active(
        &value->behavior_rename)) return;
    BongoCatPreferencesTextSession *session = &value->behavior_rename;
    bool label_changed = false;
    if (save) {
        bongo_cat_text_edit_trim(session->text);
        BongoCatBehaviorEntry *entry = NULL;
        for (size_t i = 0; i < value->app->behaviors.count; ++i)
            if (!strcmp(value->app->behaviors.entries[i].id, session->id)) {
                entry = &value->app->behaviors.entries[i];
                break;
            }
        BongoCatBehaviorShortcut *binding = binding_for(&value->app->settings,
            session->id);
        if (binding) {
            const char *label = entry && !strcmp(session->text, entry->label) ?
                "" : session->text;
            label_changed = strcmp(binding->label, label) != 0;
            snprintf(binding->label, sizeof(binding->label), "%s", label);
        }
    }
    bongo_cat_preferences_text_session_reset(session);
    SDL_StopTextInput(value->window);
    if (label_changed) bongo_cat_preferences_reload_fonts(value);
    value->render_dirty = true;
}

void bongo_cat_preferences_behavior_rename_begin(BongoCatPreferences *value,
    const BongoCatBehaviorEntry *entry, BongoCatBehaviorShortcut *binding,
    struct nk_rect bounds) {
    if (!value || !entry) return;
    bongo_cat_preferences_shortcut_cancel(value);
    bongo_cat_preferences_text_session_begin(&value->behavior_rename,
        entry->id, display_label(entry, binding), bounds);
    SDL_StartTextInput(value->window);
    value->render_dirty = true;
}

bool bongo_cat_preferences_behavior_rename_event(
    BongoCatPreferences *value, const SDL_Event *event) {
    if (!value) return false;
    BongoCatPreferencesTextSessionEvent result =
        bongo_cat_preferences_text_session_event(&value->behavior_rename,
            event, value->ui.layout_scale, value->ui.caption_font, 8.0f);
    if (result.finish)
        bongo_cat_preferences_behavior_rename_finish(value, result.save);
    if (result.handled) value->render_dirty = true;
    return result.handled;
}
