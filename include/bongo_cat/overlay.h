#ifndef BONGO_CAT_OVERLAY_H
#define BONGO_CAT_OVERLAY_H

#include "bongo_cat/model.h"

typedef struct BongoCatOverlay BongoCatOverlay;

BongoCatOverlay *bongo_cat_overlay_create(BongoCatError *error);
void bongo_cat_overlay_destroy(BongoCatOverlay *overlay);
void bongo_cat_overlay_clear(BongoCatOverlay *overlay);
BongoCatResult bongo_cat_overlay_load(BongoCatOverlay *overlay,
    const char *model_directory, bool model_pointer_preferred,
    const BongoCatLive2DRenderOptions *render_options, BongoCatError *error);
int bongo_cat_overlay_key(BongoCatOverlay *overlay, const char *name, bool pressed);
bool bongo_cat_overlay_effect(BongoCatOverlay *overlay, const char *path);
void bongo_cat_overlay_set_key_display(BongoCatOverlay *overlay, bool multi,
    int limit);
void bongo_cat_overlay_set_bubble_merge(BongoCatOverlay *overlay, bool merge);
/* 气泡与手/按键 跟随模型一起旋转: 传模型当前角度与视口像素尺寸, 内部算好变换矩阵。
   inverse_scale 传 1/(|cos|+|sin|) —— 旋转时窗口按这个系数放大, 叠加层要跟着缩回去。 */
void bongo_cat_overlay_set_rotation(BongoCatOverlay *overlay, float degrees,
    float inverse_scale, int width, int height);
/* 屏蔽按键气泡: 自带气泡素材的模型不再画按键气泡 (内置模型本来没有气泡, 不受影响)。 */
void bongo_cat_overlay_set_bubble_hidden(BongoCatOverlay *overlay, bool hidden);
bool bongo_cat_overlay_hand_active(const BongoCatOverlay *overlay, bool right);
bool bongo_cat_overlay_mver_pointer_enabled(const BongoCatOverlay *overlay);
bool bongo_cat_overlay_mver_pointer_left_handed(const BongoCatOverlay *overlay);
void bongo_cat_overlay_set_mver_pointer(BongoCatOverlay *overlay,
    float x_ratio, float y_ratio, bool left, bool right, bool side);
void bongo_cat_overlay_draw_background(BongoCatOverlay *overlay, bool mirror);
void bongo_cat_overlay_draw_pointer_before_keys(BongoCatOverlay *overlay);
void bongo_cat_overlay_draw_keys(BongoCatOverlay *overlay, bool mirror);
void bongo_cat_overlay_draw_effect(BongoCatOverlay *overlay, bool mirror);
void bongo_cat_overlay_draw_pointer_after_keys(BongoCatOverlay *overlay);

#endif
