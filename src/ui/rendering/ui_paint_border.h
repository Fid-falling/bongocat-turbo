#ifndef BONGO_CAT_UI_PAINT_BORDER_H
#define BONGO_CAT_UI_PAINT_BORDER_H

#include "nuklear_config.h"

void bongo_cat_ui_raster_dashed_rounded(unsigned char *pixels,
    int width, int height, float radius, float thickness, float dash,
    float gap, struct nk_color color);

#endif
