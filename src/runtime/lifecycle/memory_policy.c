#include "runtime.h"
#include "bongo_cat/memory.h"

static unsigned pending_presented_frames;
static bool pending_ui_frame;

static void consume_pending_frame(void) {
    if (!pending_presented_frames) return;
    pending_presented_frames--;
    if (!pending_presented_frames) bongo_cat_platform_trim_memory();
}

void bongo_cat_memory_policy_model_loaded(void) {
    pending_presented_frames = 2;
}

void bongo_cat_memory_policy_frame_presented(void) {
    consume_pending_frame();
}

void bongo_cat_memory_policy_idle(void) {
    /* Tray-only and minimized modes may not present a frame after loading. */
    consume_pending_frame();
}

void bongo_cat_memory_policy_ui_loaded(void) {
    pending_ui_frame = true;
}

void bongo_cat_memory_policy_ui_frame_presented(void) {
    if (!pending_ui_frame) return;
    pending_ui_frame = false;
    bongo_cat_platform_trim_memory();
}
