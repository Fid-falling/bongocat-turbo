/*
 * 表单行左侧的标题 (可带一个行图标)。
 *
 * 这里的"图标待定"机制值得说清楚: 调用顺序是先 bongo_cat_pref_row_icon(context, icon)
 * 再 bongo_cat_pref_toggle/float/... , 但行内真正开始排版、拿到 bounds 的时刻在
 * form_title 里 —— 图标要在**标题文字之前**画出来, 而那时参数早已传完。所以用一个
 * 文件级"待画图标"槽位把 icon 从调用点带到这里 (icon_pending + icon_context)。
 * 槽位只记一次且用完即清 (row_icon_clear): 忘了清会让下一行也画上同一个图标。
 *
 * 标题文字的排版有两条路: 没有图标时直接 nk_label (走 Nuklear 自己的绘制), 有图标时
 * 必须自己 nk_draw_text —— 因为图标占掉了左边 28px, 文本要手动画在 33px 之后。
 */
#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "ui_backend.h"
#include "ui_catime.h"

static struct nk_context *icon_context;
static BongoCatPrefIcon pending_icon;
static bool icon_pending;

/* 登记"这一行要用的图标"; 下一行的 form_label 会消费它 */
void bongo_cat_pref_row_icon(struct nk_context *context,
    BongoCatPrefIcon icon) {
    icon_context = context;
    pending_icon = icon;
    icon_pending = true;
}

void bongo_cat_pref_row_icon_clear(struct nk_context *context) {
    if (icon_context != context) return;
    icon_context = NULL;
    icon_pending = false;
}

/*
 * 标题 + 控件两列布局: control_width 是给右侧控件留的列宽, 左侧标题列拿剩下的
 * (但不小于 220px, 窄窗口下宁可让控件被挤一点, 也不要把标题压成竖排)。
 * 注意 push 的宽度用 NK_MAX(control_width, available - left - 8) —— 当左侧被 220px
 * 下限顶住时, 右侧列要吃掉剩余的全部宽度, 否则行尾会空出一条。
 */
void bongo_cat_pref_form_title_sized(struct nk_context *context,
    const char *title, float control_width) {
    float available = nk_window_get_content_region(context).w;
    float left = NK_MAX(220.0f, available - control_width - 8.0f);
    nk_layout_row_begin(context, NK_STATIC, 36, 2);
    nk_layout_row_push(context, left);
    bongo_cat_pref_form_label(context, title);
    nk_layout_row_push(context, NK_MAX(control_width,
        available - left - 8.0f));
}

/* 画标题文字。两条分支见文件头注释; 顺便清掉待画图标槽位, 保证只生效一次 */
void bongo_cat_pref_form_label(struct nk_context *context, const char *title) {
    if (!icon_pending || icon_context != context) {
        nk_style_push_font(context, bongo_cat_ui_label_font(context));
        struct nk_vec2 padding = context->style.text.padding;
        context->style.text.padding.x += 5.0f;
        nk_label(context, title, NK_TEXT_LEFT);
        context->style.text.padding = padding;
        nk_style_pop_font(context);
        return;
    }
    struct nk_rect bounds;
    enum nk_widget_layout_states state = nk_widget(&bounds, context);
    BongoCatPrefIcon icon = pending_icon;
    bongo_cat_pref_row_icon_clear(context);
    if (state == NK_WIDGET_INVALID) return;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    struct nk_rect icon_bounds = nk_rect(bounds.x + 5,
        bounds.y + (bounds.h - 18) * .5f, 18, 18);
    bongo_cat_pref_icon_draw(canvas, icon_bounds, icon, p.accent);
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    struct nk_rect text = nk_rect(bounds.x + 33,
        bounds.y + (bounds.h - font->height) * .5f,
        bounds.w - 33, font->height);
    nk_draw_text(canvas, text, title, nk_strlen(title), font,
        nk_rgba(0, 0, 0, 0), p.text);
}
