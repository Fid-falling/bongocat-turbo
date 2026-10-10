/*
 * 主题层的公共接口与几何常量。
 *
 * 这里的尺寸常量 (边距/页头高/侧边栏宽) 是全界面共用的基准: 侧边栏、页头、标签页
 * 都用同一个 BONGO_CAT_UI_MARGIN 对齐, 所以它们不是"随手调的魔法数", 改动前先确认
 * 三处布局仍然对得上 (尤其 ui_catime_tabs.c 是按可用高度反推每格高度的)。
 */
#ifndef BONGO_CAT_UI_CATIME_H
#define BONGO_CAT_UI_CATIME_H

#include <stdbool.h>
#include "nuklear_config.h"

#define BONGO_CAT_UI_MARGIN 8.0f
#define BONGO_CAT_UI_HEADER_HEIGHT 56.0f
#define BONGO_CAT_UI_SIDEBAR_WIDTH 148.0f
#define BONGO_CAT_UI_SIDEBAR_NARROW 84.0f

/* 调色板。dark 只是"选哪一套常量", 真正的取值在 ui_catime_theme.c; effects 为假时
   调用方应跳过发光/阴影 (见同文件里 [性能] 开关的说明)。 */
typedef struct BongoCatUIPalette {
    struct nk_color background;
    struct nk_color surface;
    struct nk_color surface_glass;
    struct nk_color field;
    struct nk_color border;
    struct nk_color border_subtle;
    struct nk_color text;
    struct nk_color muted;
    struct nk_color accent;
    struct nk_color accent_hover;
    struct nk_color accent_pressed;
    struct nk_color pink;
    struct nk_color pink_hover;
    struct nk_color hover;
    struct nk_color hover_pink;
    struct nk_color selection;
    struct nk_color danger;
    struct nk_color danger_background;
    bool effects;
} BongoCatUIPalette;

BongoCatUIPalette bongo_cat_ui_palette(bool dark);
/* 关闭发光/阴影类特效 (设置界面的[性能]区) */
void bongo_cat_ui_set_effects_enabled(bool enabled);
bool bongo_cat_ui_dark(const struct nk_context *context);
void bongo_cat_ui_apply_theme(struct nk_context *context, bool dark);
void bongo_cat_ui_shell_draw(struct nk_context *context, float width,
    float height, bool dark, bool native_frame);
typedef void (*BongoCatUIIconDraw)(void *userdata,
    struct nk_command_buffer *canvas, int icon, struct nk_rect bounds,
    struct nk_color color);
bool bongo_cat_ui_header(struct nk_context *context, const char *title,
    const struct nk_user_font *font, unsigned int logo_texture,
    bool *title_clicked, bool interactive, bool dark, bool native_chrome);
bool bongo_cat_ui_content_header(struct nk_context *context,
    const char *title, int icon, bool interactive, bool dark,
    bool native_chrome);
bool bongo_cat_ui_close_button(struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds,
    struct nk_color normal, struct nk_color hover, bool interactive);
void bongo_cat_ui_tabs(struct nk_context *context, const char *const *labels,
    const int *icons, int count, int *active, bool interactive, bool dark,
    float available_height, bool about_badge,
    BongoCatUIIconDraw draw_icon, void *icon_userdata);
void bongo_cat_ui_set_icons(BongoCatUIIconDraw draw_icon,
    void *icon_userdata);
bool bongo_cat_ui_draw_icon(struct nk_command_buffer *canvas, int icon,
    struct nk_rect bounds, struct nk_color color);
void bongo_cat_ui_fallback_icon(struct nk_command_buffer *canvas,
    int icon, struct nk_rect bounds, struct nk_color color);
float bongo_cat_ui_sidebar_width(float window_width);
bool bongo_cat_ui_native_chrome(void);
bool bongo_cat_ui_close_hit(float x, float y, float width);
bool bongo_cat_ui_title_link_hit(float x, float y, float width);
bool bongo_cat_ui_title_drag_hit(float x, float y, float width);

#endif
