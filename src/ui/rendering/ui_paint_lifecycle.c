#include "ui_paint.h"
#include "ui_paint_cache.h"

void bongo_cat_ui_paint_begin_frame(BongoCatUIBackend *backend) {
    bongo_cat_ui_paint_cache_begin_frame(backend);
}

void bongo_cat_ui_paint_destroy(BongoCatUIBackend *backend) {
    bongo_cat_ui_paint_cache_destroy(backend);
}
