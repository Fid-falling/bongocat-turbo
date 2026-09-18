#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "preferences_controls.h"

bool bongo_cat_pref_toggle_float(struct nk_context *context, const char *id,
    const char *title, bool *enabled, float minimum, float *value,
    float maximum, float step, float default_value) {
    FormStyle saved;
    if (!bongo_cat_pref_form_begin(context, id, 0, &saved)) return false;
    float available = nk_window_get_content_region(context).w;
    int columns = *enabled ? 3 : 2;
    float input_width = *enabled ? 122.0f : 0.0f;
    float spacing = 8.0f * (columns - 1);
    float left = NK_MAX(220.0f,
        available - input_width - 80.0f - spacing);
    nk_layout_row_begin(context, NK_STATIC, 36, columns);
    nk_layout_row_push(context, left);
    bongo_cat_pref_form_label(context, title);
    bool changed = false;
    if (*enabled) {
        nk_layout_row_push(context, input_width);
        changed = bongo_cat_pref_control_float(context, id,
            minimum, value, maximum, step, default_value);
    }
    nk_layout_row_push(context, NK_MAX(80.0f,
        available - left - input_width - spacing));
    changed = bongo_cat_pref_control_toggle(context, id, enabled) || changed;
    nk_layout_row_end(context);
    bongo_cat_pref_form_end(context, &saved);
    return changed;
}
