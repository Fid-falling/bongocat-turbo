/*
 * "关于 / 支持开发者"页 (page 3): 实验版声明 + 性能分支定位说明 + logo + 作者 + 网页链接 + 致谢,
 * 以及下面两张推广卡片 (Catime / vlaina)。
 *
 * 这一页的排版全是手算坐标: 每个块先用 nk_layout_row_dynamic + nk_widget 领一块矩形,
 * 然后在矩形里用 nk_draw_* / nk_fill_rect 直接画 —— 因为卡片、圆形光晕、渐变色块
 * 这些效果不是标准控件能表达的。改坐标时要连着看整条链 (hero 里 y 方向是 0/190/
 * 226/254/300 依次排下来的), 单独挪一个就会叠在一起。
 *
 * 三条这里特有的约定:
 * - 实验版声明必须是这一页的第一行: 它是产品定位说明, 要第一眼可见; 紧跟其后的一行是
 *   "基于 BongoCat 的性能分支"定位说明, 两行都在首屏顶部, 顺序不要调换;
 * - logo 不再套一层卡片 (素材本身就是完整品牌标识), 免得和侧栏的 logo 观感不一致;
 * - 两张推广卡片的图片来自 support assets (关于页专有), 离开这一页时会被
 * page_cache_clear 释放掉, 所以本页绘制时必须假设它们可能为 0 (image_contain 会跳过)。
 * 所有动画 id 都带各自的 key (support-logo-hover / project-hover-<name>), 否则多个
 * 元素的悬停动画会互相带。
 */
#include "preferences_state.h"
#include "preferences_about_community.h"
#include "preferences_about_footer.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_animation.h"
#include "ui_paint.h"
#include "ui_icons.h"

#include <SDL3/SDL.h>
#include <stdio.h>
static const char *tr(BongoCatPreferences *value, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}
static float width(const struct nk_user_font *font, const char *value) {
    return font->width(font->userdata, font->height, value, nk_strlen(value));
}
static void text(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    nk_draw_text(canvas, bounds, value, nk_strlen(value), font,
        nk_rgba(0, 0, 0, 0), color);
}
static void centered_span(struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *value, int length,
    const struct nk_user_font *font, struct nk_color color) {
    float value_width = font->width(font->userdata, font->height, value, length);
    float target_width = NK_MIN(bounds.w, value_width + 1.0f);
    nk_draw_text(canvas, nk_rect(bounds.x + (bounds.w - target_width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f, target_width,
        font->height), value, length, font, nk_rgba(0, 0, 0, 0), color);
}
static void centered(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    centered_span(canvas, bounds, value, nk_strlen(value), font, color);
}
/*
 * 按空格折成两行再居中。选的是"让较长的那行尽量短"的断点, 所以两行宽度最接近。
 * 一个空格都没有 (比如中文长句) 就退回单行居中并允许溢出 —— 这一页的文本都是
 * 短句/长句混排, 为了它上完整的断行算法不值得。
 */
static void centered_wrapped(struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *value,
    const struct nk_user_font *font, struct nk_color color) {
    int length = nk_strlen(value), split = -1; float best = 1.0e30f;
    if (width(font, value) <= bounds.w) {
        centered_span(canvas, bounds, value, length, font, color); return;
    }
    for (int i = 1; i + 1 < length; ++i) {
        if (value[i] != ' ') continue;
        float left = font->width(font->userdata, font->height, value, i);
        float right = font->width(font->userdata, font->height,
            value + i + 1, length - i - 1);
        float widest = NK_MAX(left, right);
        if (widest < best) { best = widest; split = i; }
    }
    if (split < 0) { centered(canvas, bounds, value, font, color); return; }
    struct nk_rect line = nk_rect(bounds.x,
        bounds.y + (bounds.h - font->height * 2) * .5f, bounds.w, font->height);
    centered_span(canvas, line, value, split, font, color); line.y += font->height;
    centered_span(canvas, line, value + split + 1, length - split - 1, font, color);
}
static bool hit(struct nk_context *context, struct nk_rect bounds) {
    return nk_input_is_mouse_hovering_rect(&context->input, bounds) &&
        nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT, bounds);
}
static void link_cursor(struct nk_context *context, struct nk_rect bounds) {
    if (nk_input_is_mouse_hovering_rect(&context->input, bounds))
        bongo_cat_ui_cursor_hover_rect(context, bounds,
            BONGO_CAT_UI_CURSOR_POINTER);
}
static void open_url(const char *url) {
    if (!SDL_OpenURL(url)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Cannot open URL: %s", SDL_GetError());
}
/*
 * 等比缩放到能放进 bounds 并居中 (contain): 两张推广图的长宽比不同, 不能用同一套
 * 固定尺寸; 纹理 id 为 0 时(素材没加载出来)直接不画, 不画占位是为了不喧宾夺主。
 */
static void image_contain(struct nk_command_buffer *canvas, unsigned int texture,
    int image_width, int image_height, struct nk_rect bounds) {
    if (!texture || image_width < 1 || image_height < 1) return;
    float scale = NK_MIN(bounds.w / image_width, bounds.h / image_height);
    struct nk_rect target = nk_rect(bounds.x + (bounds.w - image_width * scale) * .5f,
        bounds.y + (bounds.h - image_height * scale) * .5f,
        image_width * scale, image_height * scale);
    struct nk_image image = nk_image_id((int)texture);
    nk_draw_image(canvas, target, &image, nk_rgb(255, 255, 255));
}
/*
 * 悬停时按 spring 缓动抬起 4% 并上移 5px; 命中区和绘制区分开 —— 绘制区被抬高了,
 * 但点击判定仍然用原来的 bounds, 否则鼠标在边缘会闪。
 */
static void support_logo(BongoCatPreferences *value,
    struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds,
    BongoCatUIPalette p) {
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    float lift = bongo_cat_ui_animate_eased(context, "support-logo-hover",
        hover ? 1.0f : 0.0f, 250.0f, BONGO_CAT_UI_EASE_SPRING);
    float scale = 1.0f + .04f * lift;
    struct nk_rect raised = nk_rect(
        bounds.x + (bounds.w - bounds.w * scale) * .5f,
        bounds.y - 5.0f * lift + (bounds.h - bounds.h * scale) * .5f,
        bounds.w * scale, bounds.h * scale);
    /* The logo asset is the complete brand mark; avoid wrapping it in a
       second card so the support page and sidebar share the same identity. */
    if (p.effects) bongo_cat_ui_paint_shadow(context, raised, 42, 0, 10, 24, 0,
        nk_rgba(p.pink.r, p.pink.g, p.pink.b, 70));
    image_contain(canvas, value->logo_texture, value->logo_width,
        value->logo_height, raised);
    link_cursor(context, bounds);
    if (hit(context, bounds)) open_url("https://bongocat.pet");
}
static void star_button(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatUIPalette p) {
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    if (hover) bounds.y -= 2;
    if (p.effects) {
        bongo_cat_ui_paint_shadow(context, bounds, 24, 0, 10, 26, 0,
            nk_rgba(p.pink.r, p.pink.g, p.pink.b, hover ? 97 : 77));
        bongo_cat_ui_paint_gradient(context, bounds, 24,
            hover ? nk_rgb(255, 141, 184) : nk_rgb(255, 155, 193),
            hover ? nk_rgb(237, 103, 154) : nk_rgb(244, 119, 168));
    } else nk_fill_rect(canvas, bounds, 23, hover ? p.pink_hover : p.pink);
    nk_stroke_rect(canvas, bounds, 24, 1, nk_rgba(255, 255, 255, 133));
    bongo_cat_preferences_icon_draw(value, canvas,
        BONGO_CAT_UI_ICON_GITHUB,
        nk_rect(bounds.x + 22, bounds.y + 14.5f, 19, 19), nk_rgb(255, 255, 255));
    text(canvas, nk_rect(bounds.x + 50, bounds.y +
        (bounds.h - value->ui.label_font->height) * .5f,
        bounds.w - 62, value->ui.label_font->height), tr(value,
        "native.support.starOnGitHub", "Star on GitHub!"),
        value->ui.label_font, nk_rgb(255, 255, 255));
    link_cursor(context, bounds);
    if (hit(context, bounds))
        open_url("https://github.com/Fid-falling/bongocat-turbo");
}
/*
 * 作者名做成链接: 前面 "by " 是普通文字, 只有作者名那块参与命中与动画, 所以
 * x 坐标是由三段的文字宽度累加算出来的 (标题宽度随语言变化)。
 */
static void hero_title(BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds,
    BongoCatUIPalette p) {
    const char *title = "BongoCat Turbo";
    const char *by = tr(value, "native.support.by", "by ");
    const char *developer = "vladelaina & Fid_falling";
    float gap = 8, title_width = width(value->ui.hero_font, title);
    float by_width = width(value->ui.caption_font, by),
        developer_width = width(value->ui.caption_font, developer);
    float x = bounds.x + (bounds.w - title_width - by_width -
        developer_width - gap) * .5f;
    text(canvas, nk_rect(x, bounds.y, title_width + 1, 32), title,
        value->ui.hero_font, p.text);
    text(canvas, nk_rect(x + title_width + gap, bounds.y + 8,
        by_width + 1, 22), by, value->ui.caption_font, p.muted);
    struct nk_rect link = nk_rect(x + title_width + gap + by_width,
        bounds.y + 8, developer_width + 1, 22);
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, link);
    float amount = bongo_cat_ui_animate_eased(context, "support-author-hover",
        hover ? 1.0f : 0.0f, 200, BONGO_CAT_UI_EASE_STANDARD);
    text(canvas, link, developer, value->ui.caption_font,
        bongo_cat_ui_color_mix(p.accent, p.pink, amount));
    link_cursor(context, link); if (hit(context, link))
        open_url("https://github.com/vladelaina/BongoCat");
}
void bongo_cat_preferences_about_hero(BongoCatPreferences *value,
    struct nk_context *context) {
    /*
     * 实验版声明: 放在"关于"页最上面一行, 打开这一页第一眼就能看到, 不打扰
     * 下面的 logo / 作者 / 社区布局。
     */
    BongoCatUIPalette notice_palette =
        bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    const float notice_height = value->ui.caption_font &&
        value->ui.caption_font->height > 0.0f ?
        value->ui.caption_font->height + 14.0f : 32.0f;
    nk_layout_row_dynamic(context, notice_height, 1);
    struct nk_rect notice_bounds;
    if (nk_widget(&notice_bounds, context) != NK_WIDGET_INVALID) {
        centered_wrapped(nk_window_get_canvas(context), notice_bounds,
            tr(value, "native.support.experimentalNotice",
            "This program is a functional-test experimental build and does not "
            "represent the final product."),
            value->ui.caption_font, notice_palette.accent);
    }
    /*
     * 定位说明 (这是基于 BongoCat 的性能分支): 紧跟在实验版声明之后, 仍在首屏内。
     * 高度按两行给 —— centered_wrapped 最多折两行, 而它是按 (h - 2*行高)/2 垂直居中的,
     * 只给一行高度时第二行会往上溢出到上一个块上。
     */
    nk_layout_row_dynamic(context, notice_height * 2.0f, 1);
    struct nk_rect fork_bounds;
    if (nk_widget(&fork_bounds, context) != NK_WIDGET_INVALID) {
        centered_wrapped(nk_window_get_canvas(context), fork_bounds,
            tr(value, "native.support.forkNotice",
            "A performance-focused fork of BongoCat: less memory, fewer stutters, "
            "plus a Vulkan backend and a transparent OpenGL background."),
            value->ui.caption_font, notice_palette.muted);
    }
    struct nk_rect bounds;
    nk_layout_row_dynamic(context, 340, 1);
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    if (p.effects) {
        nk_push_scissor(canvas, bounds);
        bongo_cat_ui_paint_radial_circle(context,
            nk_rect(bounds.x + bounds.w * .5f - 300,
            bounds.y - 190, 600, 360),
            nk_rgba(p.accent.r, p.accent.g, p.accent.b, 41),
            nk_rgba(p.pink.r, p.pink.g, p.pink.b, 20), .49f, .84f);
        nk_push_scissor(canvas, nk_window_get_content_region(context));
    }
    star_button(value, context, canvas,
        nk_rect(bounds.x + bounds.w - 188, bounds.y + 6, 177, 46), p);
    support_logo(value, context, canvas,
        nk_rect(bounds.x + (bounds.w - 168) * .5f + 3,
        bounds.y + 10, 168, 168), p);
    hero_title(value, context, canvas, nk_rect(bounds.x + 3, bounds.y + 190,
        bounds.w, 36), p);
    bongo_cat_preferences_about_localized_link(value, context, canvas,
        nk_rect(bounds.x, bounds.y + 226, bounds.w, 24),
        "native.support.website", "Website: bongocat.pet",
        "https://bongocat.pet", "support-website-hover", p);
    centered_wrapped(canvas, nk_rect(bounds.x + 36, bounds.y + 254,
        bounds.w - 72, 40),
        tr(value, "native.support.heroText",
        "Thank you for your support. Every use and share helps BongoCat grow."),
        value->ui.caption_font, p.muted);
    bongo_cat_preferences_about_footer(value, context, canvas,
        nk_rect(bounds.x, bounds.y + 300, bounds.w, 34), p);
}
/*
 * 一张推广卡片: 图标 + 底部名字, 整体可点。
 * pink 决定用粉色还是强调色 (两张卡片刻意区分开), 并顺带切换图标的绘制内缩 ——
 * 粉色那张的图自带留白, 非粉色那张需要手动内缩一点才不至于贴边。
 */
static void project(BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds, unsigned int texture,
    int image_width, int image_height, const char *name, const char *url,
    bool pink, BongoCatUIPalette p) {
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    char animation_id[48];
    snprintf(animation_id, sizeof(animation_id), "project-hover-%s", name);
    float lift = bongo_cat_ui_animate_eased(context, animation_id,
        hover ? 1.0f : 0.0f, 280.0f, BONGO_CAT_UI_EASE_SWIFT);
    struct nk_rect icon = nk_rect(bounds.x + (bounds.w - 148) * .5f,
        bounds.y - 7.0f * lift, 148, 148);
    if (p.effects) {
        bongo_cat_ui_paint_radial(context,
            nk_rect(icon.x - 16, icon.y - 16, icon.w + 32, icon.h + 32),
            nk_rgba(pink ? p.pink.r : p.accent.r,
                pink ? p.pink.g : p.accent.g, pink ? p.pink.b : p.accent.b, 43),
            nk_rgba(0, 0, 0, 0), .05f, 1.0f);
        bongo_cat_ui_paint_shadow(context, icon, 37, 0, 18, 34, 0,
            nk_rgba(p.text.r, p.text.g, p.text.b, 38));
    }
    if (!pink && p.effects) bongo_cat_ui_paint_gradient(context, icon, 37,
        nk_rgb(245, 241, 255), nk_rgb(234, 247, 255));
    else nk_fill_rect(canvas, icon, 37, pink ? p.surface : p.selection);
    image_contain(canvas, texture, image_width, image_height, pink ? icon :
        nk_rect(icon.x + 5, icon.y + 8, icon.w - 10, icon.h - 16));
    centered(canvas, nk_rect(bounds.x, bounds.y + 166, bounds.w, 30), name,
        value->ui.heading_font, hover ? (pink ? p.pink : p.accent) : p.text);
    link_cursor(context, bounds);
    if (hit(context, bounds)) open_url(url);
}

void bongo_cat_preferences_about_projects(BongoCatPreferences *value,
    struct nk_context *context) {
    struct nk_rect bounds;
    nk_layout_row_dynamic(context, 330, 1);
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    bongo_cat_preferences_about_projects_heading(value, context, bounds);
    float card_width = 220, center = bounds.x + bounds.w * .5f + 3;
    project(value, context, canvas, nk_rect(center - 262, bounds.y + 112,
        card_width, 200), value->catime_texture, value->catime_width,
        value->catime_height, "Catime", "https://cati.me/", false, p);
    project(value, context, canvas, nk_rect(center + 42, bounds.y + 112,
        card_width, 200), value->vlaina_texture, value->vlaina_width,
        value->vlaina_height, "vlaina", "https://vlaina.com/r/bongocat",
        true, p);
}
