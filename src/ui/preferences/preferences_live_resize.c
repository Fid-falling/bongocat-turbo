#include "preferences_state.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <SDL3/SDL_properties.h>

static const wchar_t original_property[] = L"BongoCat.PreferenceResizeProc";
static const wchar_t value_property[] = L"BongoCat.PreferenceResizeValue";
#define BONGO_CAT_LIVE_RESIZE_TIMER ((UINT_PTR)0xBC50)
#define BONGO_CAT_LIVE_RESIZE_INTERVAL_MS 16

static HWND native_window(BongoCatPreferences *value) {
    return value && value->window ? (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(value->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}

static void render_live(BongoCatPreferences *value) {
    if (!value || !value->window || value->live_resize_rendering) return;
    value->live_resize_rendering = true;
    bool fast = value->live_resize_active;
    value->ui.live_resize_fast = fast;
    if (!value->input_active) bongo_cat_preferences_input_begin(value);
    value->render_dirty = true;
    bongo_cat_preferences_render(value);
    value->ui.live_resize_fast = false;
    if (fast) value->live_resize_layout_frames++;
    value->live_resize_rendering = false;
}

static bool capture_live(BongoCatPreferences *value) {
    if (!value || !SDL_GL_MakeCurrent(value->window, value->gl_context))
        return false;
    bool result = bongo_cat_ui_resize_cache_capture(&value->ui);
    SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
    return result;
}

static bool present_live(BongoCatPreferences *value) {
    if (!value || !SDL_GL_MakeCurrent(value->window, value->gl_context))
        return false;
    bool result = bongo_cat_ui_resize_cache_present(&value->ui) &&
        SDL_GL_SwapWindow(value->window);
    SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
    if (result) bongo_cat_preferences_record_frame(value);
    return result;
}

static void pump_pet(BongoCatPreferences *value) {
    if (value && value->live_resize_modal_ready)
        bongo_cat_modal_frame_tick(&value->live_resize_modal_frame);
}

static LRESULT CALLBACK live_resize_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    WNDPROC original = (WNDPROC)GetPropW(window, original_property);
    BongoCatPreferences *value = (BongoCatPreferences *)GetPropW(
        window, value_property);
    bool enter = value && message == WM_ENTERSIZEMOVE;
    bool resize = value && value->live_resize_active && message == WM_SIZE;
    bool timer = value && value->live_resize_active && message == WM_TIMER &&
        wparam == BONGO_CAT_LIVE_RESIZE_TIMER;
    LRESULT result = CallWindowProcW(original ? original : DefWindowProcW,
        window, message, wparam, lparam);
    if (enter) {
        value->live_resize_active = true;
        value->live_resize_pending = false;
        value->live_resize_timer = SetTimer(window,
            BONGO_CAT_LIVE_RESIZE_TIMER,
            BONGO_CAT_LIVE_RESIZE_INTERVAL_MS, NULL) != 0;
        bongo_cat_modal_frame_init(&value->live_resize_modal_frame,
            value->app);
        value->live_resize_modal_ready = true;
        capture_live(value);
        pump_pet(value);
    }
    if (resize) {
        value->live_resize_pending = true;
        if (!present_live(value) || !value->live_resize_timer) {
            value->live_resize_pending = false;
            render_live(value);
            capture_live(value);
        }
        pump_pet(value);
    }
    if (timer) {
        if (value->live_resize_pending) {
            value->live_resize_pending = false;
            render_live(value);
            capture_live(value);
        }
        pump_pet(value);
    }
    if (value && message == WM_EXITSIZEMOVE) {
        KillTimer(window, BONGO_CAT_LIVE_RESIZE_TIMER);
        value->live_resize_active = false;
        value->live_resize_pending = false;
        value->live_resize_timer = false;
        render_live(value);
        pump_pet(value);
        value->live_resize_modal_ready = false;
        if (SDL_GL_MakeCurrent(value->window, value->gl_context)) {
            bongo_cat_ui_resize_cache_destroy(&value->ui);
            SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
        }
    }
    return result;
}

void bongo_cat_preferences_live_resize_install(BongoCatPreferences *value) {
    HWND window = native_window(value);
    if (!window || GetPropW(window, original_property)) return;
    WNDPROC original = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (!original || !SetPropW(window, original_property, (HANDLE)original)) return;
    if (!SetPropW(window, value_property, (HANDLE)value)) {
        RemovePropW(window, original_property);
        return;
    }
    if (!SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)live_resize_proc)) {
        RemovePropW(window, value_property);
        RemovePropW(window, original_property);
    }
}

void bongo_cat_preferences_live_resize_uninstall(BongoCatPreferences *value) {
    if (!value) return;
    HWND window = native_window(value);
    WNDPROC original = window ? (WNDPROC)GetPropW(window, original_property) : NULL;
    if (window) KillTimer(window, BONGO_CAT_LIVE_RESIZE_TIMER);
    if (original) SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)original);
    if (window) {
        RemovePropW(window, value_property);
        RemovePropW(window, original_property);
    }
    value->live_resize_active = false;
    value->live_resize_pending = false;
    value->live_resize_timer = false;
    value->live_resize_modal_ready = false;
    value->ui.live_resize_fast = false;
    value->live_resize_rendering = false;
}
#else
void bongo_cat_preferences_live_resize_install(BongoCatPreferences *value) {
    (void)value;
}
void bongo_cat_preferences_live_resize_uninstall(BongoCatPreferences *value) {
    (void)value;
}
#endif
