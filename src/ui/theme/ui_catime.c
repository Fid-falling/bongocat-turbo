/*
 * 设置界面的"外壳"与通用绘制件: 侧边栏、品牌页头、内容页头、关闭按钮, 以及无边框
 * 窗口的命中测试。
 *
 * 图标绘制是从上层注入的 (bongo_cat_ui_set_icons): 图标图集属于应用层, 主题层不该
 * 反向依赖它, 所以这里只存一个函数指针; 没有注入时退回 ui_catime_icons.c 里手绘的
 * 简笔图标 (丑但不会让界面缺东西)。
 *
 * 本文件最容易出错的地方是"画出来的矩形"和"命中测试用的矩形"必须一致: 下面三个
 * *_hit() 函数是给窗口拖动/关闭用的, 它们写死了和绘制代码同一套坐标 (8/58/12/14/62...),
 * 改了绘制参数就必须同步改这里, 否则会出现"点得到但拖不动"或"拖到关闭按钮上"。
 */
#include "ui_catime.h"
#include "ui_backend.h"
#include "ui_animation.h"
#include "ui_icons.h"
#include "ui_paint.h"

static BongoCatUIIconDraw external_icon_draw;
static void *external_icon_userdata;

void bongo_cat_ui_set_icons(BongoCatUIIconDraw draw_icon,
    void *icon_userdata) {
    external_icon_draw = draw_icon;
    external_icon_userdata = icon_userdata;
}

bool bongo_cat_ui_draw_icon(struct nk_command_buffer *canvas, int icon,
    struct nk_rect bounds, struct nk_color color) {
    if (!external_icon_draw) return false;
    external_icon_draw(external_icon_userdata, canvas, icon, bounds, color);
    return true;
}

/* target_width 多给 1.0f 并夹到 bounds.w: Nuklear 画文字时会按 rect 宽度决定是否
   换行/裁剪, 浮点宽度恰好等于文字宽度时会在某些字号下把最后一个字切掉。
   背景色传全透明, 文字颜色由调用方给 (避免每个调用点各写一遍矩形计算)。 */
static void centered_span(struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *text, int length,
    const struct nk_user_font *font, struct nk_color color) {
    float width = font->width(font->userdata, font->height, text, length);
    float target_width = NK_MIN(bounds.w, width + 1.0f);
    struct nk_rect target = nk_rect(bounds.x + (bounds.w - target_width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f,
        target_width, font->height);
    nk_draw_text(canvas, target, text, length, font,
        nk_rgba(0, 0, 0, 0), color);
}

static void centered(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *text, const struct nk_user_font *font, struct nk_color color) {
    centered_span(canvas, bounds, text, nk_strlen(text), font, color);
}

/*
 * 侧边栏宽度只有两档, 阈值 780 是"窗口再窄就放不下中文标签"的经验值。窄档下标签
 * 不显示、只留图标和 tooltip, 这个约定在 ui_catime_tabs.c 里按同一个宽度判断
 * (tile.w < 80 时走图标模式), 两边必须同时改。
 */
float bongo_cat_ui_sidebar_width(float window_width) {
    return window_width <= 780.0f ? BONGO_CAT_UI_SIDEBAR_NARROW :
        BONGO_CAT_UI_SIDEBAR_WIDTH;
}

bool bongo_cat_ui_native_chrome(void) {
    /* Keep SDL borderless input handling consistent on macOS. */
    return false;
}

/*
 * 画窗口底板。非原生边框时用两遍绘制 + scissor 造出"整块毛玻璃 + 侧边栏实色":
 * 先按整窗圆角铺 surface_glass, 再把裁剪框压到侧边栏区域重铺一次 surface —— 这样
 * 侧边栏右侧是一条干净的直边, 而外层圆角仍然完整。native_frame 时系统自己在画边框,
 * 圆角必须为 0, 否则 DWM 的方角会和我们的圆角打架。
 * 侧边栏发光只在浅色主题、开了特效、且不在实时缩放时烘焙 (见 live_resize_fast)。
 */
void bongo_cat_ui_shell_draw(struct nk_context *context, float width,
    float height, bool dark, bool native_frame) {
    BongoCatUIPalette p = bongo_cat_ui_palette(dark);
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    float side = bongo_cat_ui_sidebar_width(width);
    float rounding = native_frame ? 0.0f : 24.0f;
    float sidebar_right = BONGO_CAT_UI_MARGIN + side;
    struct nk_rect surface = nk_rect(0, 0, width, height);
    BongoCatUIBackend *backend = bongo_cat_ui_backend_for_context(context);
    bool fast = backend && backend->live_resize_fast;
    if (native_frame) {
        nk_fill_rect(canvas, surface, 0, p.surface_glass);
        nk_fill_rect(canvas, nk_rect(0, 0, sidebar_right, height), 0,
            p.surface);
    } else {
        nk_fill_rect(canvas, surface, rounding, p.surface_glass);
        nk_push_scissor(canvas, nk_rect(0, 0, sidebar_right, height));
        nk_fill_rect(canvas, surface, rounding, p.surface);
        nk_push_scissor(canvas, surface);
    }
    if (p.effects && !fast && !dark)
        bongo_cat_ui_paint_sidebar_glow(context, surface, sidebar_right,
            rounding,
            nk_rgba(p.accent.r, p.accent.g, p.accent.b, 56));
    nk_stroke_line(canvas, sidebar_right, BONGO_CAT_UI_MARGIN, sidebar_right,
        height - BONGO_CAT_UI_MARGIN, 1,
        p.border_subtle);
}

/*
 * 品牌页头 (logo + 标题)。是一个"占位控件": 只申请高度、自己画内容, 不返回任何
 * 可交互状态, 所以两处 return 都写死 false。
 * 布局有四种组合 (窄/宽 x 原生边框/无边框) —— native_chrome 时系统标题栏占着顶部,
 * 所以 logo 的 top 从 8 抬到 34、页头整体也更高, 这就是 top/title_y 两组常数的由来。
 * 悬停缩放走 EASE_SPRING, 冲过头一点才有"弹一下"的手感; 缩放会同时改 logo 的
 * y, 保证它是从中心放大而不是向下长。
 */
bool bongo_cat_ui_header(struct nk_context *context, const char *title,
    const struct nk_user_font *font, unsigned int logo_texture,
    bool *title_clicked, bool interactive, bool dark, bool native_chrome) {
    struct nk_rect bounds;
    bool compact = nk_window_get_content_region(context).w < 100;
    float height = compact ? (native_chrome ? 148.0f : 118.0f) :
        (native_chrome ? 176.0f : 148.0f);
    nk_layout_row_dynamic(context, height, 1);
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return false;
    BongoCatUIPalette p = bongo_cat_ui_palette(dark);
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    /* The current logo asset already contains its own rounded brand mark.
       Give it room to read at a glance instead of framing it a second time. */
    float base_size = compact ? 68.0f : 84.0f;
    float top = native_chrome ? 34.0f : (compact ? 6.0f : 8.0f);
    struct nk_rect base_logo = nk_rect(bounds.x + (bounds.w - base_size) * .5f,
        bounds.y + top, base_size, base_size);
    bool logo_hover = interactive && nk_input_is_mouse_hovering_rect(
        &context->input, base_logo);
    bool hover = interactive && nk_input_is_mouse_hovering_rect(&context->input,
        nk_rect(bounds.x, bounds.y, bounds.w, bounds.h));
    float hover_amount = bongo_cat_ui_animate_eased(context,
        "brand-logo-hover", hover ? 1.0f : 0.0f, 250.0f,
        BONGO_CAT_UI_EASE_SPRING);
    float size = base_size * (1.0f + .08f * hover_amount);
    struct nk_rect logo = nk_rect(bounds.x + (bounds.w - size) * .5f,
        bounds.y + top - (size - base_size) * .5f, size, size);
    if (p.effects) bongo_cat_ui_paint_shadow(context, logo,
        compact ? 17.0f : 21.0f, 0, 4, 14, 0,
        nk_rgba(p.pink.r, p.pink.g, p.pink.b, 64));
    if (logo_texture) {
        struct nk_image image = nk_image_id((int)logo_texture);
        nk_draw_image(canvas, logo, &image, nk_rgb(255, 255, 255));
    }
    if (bounds.w >= 100) {
        if (!font) font = bongo_cat_ui_caption_font(context);
        float title_y = native_chrome ? 124.0f : 94.0f;
        struct nk_color title_color = logo_hover ? nk_rgb(84, 174, 255) : p.pink;
        centered(canvas, nk_rect(bounds.x, bounds.y + title_y, bounds.w, 28), title,
            font, title_color);
    }
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    if (title_clicked) *title_clicked = hover &&
        nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT, bounds);
    return false;
}
/*
 * 内容页头: 左侧图标 + 标题, 右侧关闭按钮。返回"是否点了关闭"。
 * native_chrome 时直接跳过关闭按钮 (系统标题栏已经有), 但仍然吃掉同一行的高度,
 * 否则开启原生边框后整页内容会上移、和页头重叠。
 */
bool bongo_cat_ui_content_header(struct nk_context *context,
    const char *title, int icon, bool interactive, bool dark,
    bool native_chrome) {
    struct nk_rect bounds;
    nk_layout_row_dynamic(context, BONGO_CAT_UI_HEADER_HEIGHT, 1);
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return false;
    BongoCatUIPalette p = bongo_cat_ui_palette(dark);
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, bounds, 0,
        bongo_cat_ui_color_mix(p.surface_glass, p.surface, .2f));
    struct nk_rect icon_bounds = nk_rect(bounds.x + 20, bounds.y + 16, 22, 22);
    if (external_icon_draw) external_icon_draw(external_icon_userdata, canvas,
        icon, icon_bounds, p.accent);
    else bongo_cat_ui_fallback_icon(canvas, icon, icon_bounds, p.accent);
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    struct nk_rect text = nk_rect(bounds.x + 52,
        bounds.y + (bounds.h - font->height) * .5f, bounds.w - 110, font->height);
    nk_draw_text(canvas, text, title, nk_strlen(title), font,
        nk_rgba(0, 0, 0, 0), p.accent);
    if (native_chrome) return false;
    struct nk_rect close = nk_rect(bounds.x + bounds.w - 48, bounds.y + 11, 34, 34);
    return bongo_cat_ui_close_button(context, canvas, close, p.muted,
        p.accent, interactive);
}

bool bongo_cat_ui_close_button(struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds,
    struct nk_color normal, struct nk_color hover, bool interactive) {
    bool is_hovered = interactive && nk_input_is_mouse_hovering_rect(
        &context->input, bounds);
    struct nk_color color = is_hovered ? hover : normal;
    float icon_size = NK_MAX(0.0f, NK_MIN(bounds.w, bounds.h) - 14.0f);
    struct nk_rect icon = nk_rect(bounds.x + (bounds.w - icon_size) * .5f,
        bounds.y + (bounds.h - icon_size) * .5f, icon_size, icon_size);
    if (!bongo_cat_ui_draw_icon(canvas, BONGO_CAT_UI_ICON_CLOSE, icon, color)) {
        float inset = icon_size * .22f;
        nk_stroke_line(canvas, icon.x + inset, icon.y + inset,
            icon.x + icon.w - inset, icon.y + icon.h - inset, 2, color);
        nk_stroke_line(canvas, icon.x + icon.w - inset, icon.y + inset,
            icon.x + inset, icon.y + icon.h - inset, 2, color);
    }
    if (is_hovered) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return is_hovered && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, bounds);
}

/*
 * 下面三个命中测试给无边框窗口的鼠标按下处理用 (判定"这一下是拖窗口、还是点关闭")。
 * 它们是纯几何函数, 不依赖 nk_context, 因为事件到达时 Nuklear 还没开始画这一帧 ——
 * 也就是说这里的坐标必须和绘制代码逐字对齐, 不能靠"反正下一帧会重算"。
 * 三者的优先级由调用方保证: 先 close_hit, 再 title_link_hit, 剩下的区域才是拖动
 * (title_drag_hit 内部也显式排除了关闭按钮, 免得顺序被改坏)。
 */
bool bongo_cat_ui_close_hit(float x, float y, float width) {
    return x >= width - 58.0f && x <= width - 12.0f && y >= 14.0f && y <= 62.0f;
}

bool bongo_cat_ui_title_link_hit(float x, float y, float width) {
    return x >= 8.0f && x <= 8.0f + bongo_cat_ui_sidebar_width(width) &&
        y >= 8.0f && y <= 156.0f;
}

bool bongo_cat_ui_title_drag_hit(float x, float y, float width) {
    float side = bongo_cat_ui_sidebar_width(width);
    return x >= 8.0f + side && x <= width - 8.0f &&
        y >= 8.0f && y <= 8.0f + BONGO_CAT_UI_HEADER_HEIGHT &&
        !bongo_cat_ui_close_hit(x, y, width);
}
