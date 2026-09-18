#ifndef BONGO_CAT_UI_PAINT_H
#define BONGO_CAT_UI_PAINT_H

#include "nuklear_config.h"

struct BongoCatUIBackend;

struct nk_color bongo_cat_ui_color_mix(struct nk_color first,
    struct nk_color second, float amount);
struct nk_color bongo_cat_ui_color_alpha(struct nk_color color,
    float amount);
void bongo_cat_ui_paint_gradient(struct nk_context *context,
    struct nk_rect bounds, float rounding, struct nk_color first,
    struct nk_color second);
void bongo_cat_ui_paint_radial(struct nk_context *context,
    struct nk_rect bounds, struct nk_color center, struct nk_color edge,
    float midpoint, float outer);
void bongo_cat_ui_paint_radial_circle(struct nk_context *context,
    struct nk_rect bounds, struct nk_color center, struct nk_color edge,
    float midpoint, float outer);
void bongo_cat_ui_paint_sidebar_glow(struct nk_context *context,
    struct nk_rect surface, float sidebar, float rounding,
    struct nk_color color);
void bongo_cat_ui_paint_rounded_surface(struct nk_context *context,
    struct nk_rect bounds, float rounding, struct nk_color color);
void bongo_cat_ui_paint_shadow(struct nk_context *context,
    struct nk_rect bounds, float rounding, float offset_x, float offset_y,
    float blur, float spread, struct nk_color color);
void bongo_cat_ui_paint_dashed_rounded(struct nk_context *context,
    struct nk_rect bounds, float rounding, float thickness, float dash,
    float gap, struct nk_color color);
void bongo_cat_ui_paint_begin_frame(struct BongoCatUIBackend *backend);
void bongo_cat_ui_paint_destroy(struct BongoCatUIBackend *backend);

#endif
