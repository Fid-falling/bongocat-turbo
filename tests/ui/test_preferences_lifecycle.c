#include "preferences_state.h"
#include "preferences_import_internal.h"
#include "preferences_render_internal.h"
#include "ui_catime.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void busy(BongoCatImportDialog *dialog, bool open, bool importing) {
    SDL_LockMutex(dialog->mutex);
    dialog->open = open;
    dialog->busy = importing;
    SDL_UnlockMutex(dialog->mutex);
}

static void close_over_scrolled_models(BongoCatPreferences *value) {
    if (bongo_cat_ui_native_chrome()) return;
    struct nk_context *context = &value->ui.context;
    value->page = 1;
    value->page_seen = true;
    value->last_page = 1;
    value->page_transition_ns = 0;
    for (int scroll = 0; scroll <= 800; scroll += 40) {
        nk_input_begin(context);
        nk_input_button(context, NK_BUTTON_LEFT, 746, 36, nk_false);
        nk_input_end(context);
        bongo_cat_preferences_draw_frame(value, 800, 500, false);
        nk_clear(context);
        value->scroll_ready[1] = true;
        value->scroll_current[1] = (float)scroll;
        value->scroll_target[1] = (float)scroll;
        nk_input_begin(context);
        nk_input_motion(context, 746, 36);
        nk_input_button(context, NK_BUTTON_LEFT, 746, 36, nk_true);
        nk_input_button(context, NK_BUTTON_LEFT, 746, 36, nk_false);
        nk_input_end(context);
        CHECK(bongo_cat_preferences_draw_frame(value, 800, 500, false));
        CHECK(!value->model_selection_pending);
        value->model_selection_pending = false;
        nk_clear(context);
    }
}

int main(int argc, char **argv) {
    BongoCatApp *app = calloc(1, sizeof(*app));
    BongoCatError error = {0};
    if (!app) return 1;
    if (!bongo_cat_app_initialize(app, argc, argv, &error)) {
        fprintf(stderr, "Initialization failed: %s\n", error.message);
        bongo_cat_app_shutdown(app, "test:failed", 1);
        free(app);
        return 1;
    }
    BongoCatPreferences *value = app->preferences;
    CHECK(value != NULL);
    if (value) {
        for (int cycle = 0; cycle < 3; ++cycle) {
            bongo_cat_preferences_show(value);
            CHECK(value->window && value->gl_context && value->ui_initialized);
            bongo_cat_preferences_close(value);
            CHECK(!value->window && !value->gl_context && !value->ui_initialized);
            CHECK(SDL_GL_GetCurrentContext() == app->gl_context);
        }
        bongo_cat_preferences_show(value);
        SDL_Window *window = value->window;
        busy(value->import_dialog, true, false);
        bongo_cat_preferences_close(value);
        bongo_cat_preferences_render(value);
        CHECK(value->window == window && !value->visible);
        busy(value->import_dialog, false, true);
        bongo_cat_preferences_render(value);
        CHECK(value->window == window);
        bongo_cat_preferences_show(value);
        CHECK(value->window == window && value->visible);
        busy(value->import_dialog, false, false);
        bongo_cat_preferences_render(value);
        CHECK(value->window == window && value->visible);
        busy(value->import_dialog, false, true);
        bongo_cat_preferences_close(value);
        CHECK(value->window == window && !value->visible);
        busy(value->import_dialog, false, false);
        bongo_cat_preferences_render(value);
        CHECK(!value->window && !value->gl_context && !value->ui_initialized);
        bongo_cat_preferences_show(value);
        CHECK(value->visible && value->ui_initialized);
        close_over_scrolled_models(value);
        bongo_cat_preferences_close(value);
    }
    bongo_cat_app_shutdown(app, "test:complete", failures != 0);
    free(app);
    printf("Preferences lifecycle: %d failures\n", failures);
    return failures != 0;
}
