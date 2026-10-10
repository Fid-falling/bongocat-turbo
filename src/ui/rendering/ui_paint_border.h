#ifndef BONGO_CAT_UI_PAINT_BORDER_H
#define BONGO_CAT_UI_PAINT_BORDER_H

#include "nuklear_config.h"

/* 把虚线圆角边框逐像素写进 pixels (RGBA8, width*height*4 字节, 调用方分配)。
   颜色按 color.a * 覆盖率预乘到 alpha 里, RGB 为 0 时也保持 0 (避免线性采样出彩边),
   所以绘制时要用 nk_rgb(255,255,255) 当 tint, 否则颜色会被乘两次。 */
void bongo_cat_ui_raster_dashed_rounded(unsigned char *pixels,
    int width, int height, float radius, float thickness, float dash,
    float gap, struct nk_color color);

#endif
