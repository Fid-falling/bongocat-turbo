/*
 * 模态弹层 (删除确认框 / 行为设置对话框) 的公共部分: 遮罩、面板几何、开关动画、输入
 * 武装。
 *
 * 两个时间戳分工明确: opened_ns 驱动淡入, closing_ns 非 0 表示正在淡出; 两者都在
 * frame() 里换算成 visibility/close_amount, 并给出 finished (淡出结束) —— 调用方
 * 看到 finished 才能真的销毁弹层状态, 提前销毁会让动画戛然而止。
 *
 * input_ready 是"吃掉打开弹层那一次点击"的机制: 打开弹层的点击本身还没结束 (左键
 * 还按着), 如果立刻允许输入, 同一个点击会马上命中弹层上的按钮。所以要先等左键抬起,
 * 之后 armed 置真并一直保持。
 */
#include "preferences_overlay.h"
#include "ui_animation.h"
#include "ui_paint.h"

#include <SDL3/SDL.h>

/* 打开 200ms / 关闭 180ms: 关闭比打开略快, 手感上更像"立刻响应" */
enum { OVERLAY_OPEN_MS = 200, OVERLAY_CLOSE_MS = 180 };

bool bongo_cat_preferences_overlay_input_ready(struct nk_context *context,
    bool *armed) {
    if (!context || !armed) return false;
    if (*armed) return true;
    const struct nk_mouse_button *left =
        &context->input.mouse.buttons[NK_BUTTON_LEFT];
    if (!left->down) *armed = true;
    return false;
}

struct nk_color bongo_cat_preferences_overlay_alpha(
    struct nk_color color, float visibility) {
    color.a = (nk_byte)(color.a * NK_CLAMP(0.0f, visibility, 1.0f) + .5f);
    return color;
}

/*
 * 算面板矩形与可见度。关闭时面板会缩 1.5% 并下移 6px —— 这是"收进背景"的视觉暗示,
 * 让弹层消失看起来是有方向的, 而不是原地闪没。
 * closing_ns 优先于 opened_ns: 一个弹层可能在淡入还没结束时就被关掉。
 */
BongoCatOverlayFrame bongo_cat_preferences_overlay_frame(
    struct nk_rect region, float width, float height, uint64_t opened_ns,
    uint64_t closing_ns) {
    uint64_t now = SDL_GetTicksNS();
    float visibility = 1.0f, close = 0.0f;
    bool finished = false;
    if (closing_ns) {
        float progress = (float)(now - closing_ns) /
            (OVERLAY_CLOSE_MS * 1000000.0f);
        finished = progress >= 1.0f;
        close = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_STANDARD,
            NK_CLAMP(0.0f, progress, 1.0f));
        visibility = 1.0f - close;
    } else if (opened_ns) {
        float progress = (float)(now - opened_ns) /
            (OVERLAY_OPEN_MS * 1000000.0f);
        visibility = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_STANDARD,
            NK_CLAMP(0.0f, progress, 1.0f));
    }
    float scale = 1.0f - .015f * close;
    float panel_width = width * scale, panel_height = height * scale;
    BongoCatOverlayFrame frame = {
        .panel = nk_rect(region.x + (region.w - panel_width) * .5f,
            region.y + (region.h - panel_height) * .5f + 6.0f * close,
            panel_width, panel_height),
        .visibility = visibility, .close_amount = close, .finished = finished};
    return frame;
}

/*
 * 画遮罩层 (面板本体由各自的调用方画, 因为面板内容不一样)。
 * 遮罩颜色按主题区分: 暗色主题用深色薄雾, 亮色主题用白色薄雾 —— 亮色主题下用深色
 * 遮罩会显得很脏。
 * 注意这里只是把 scissor 压到整窗口区域, 面板的阴影要靠外扩才能画出来, 所以调用方
 * 必须在面板画完之后自己恢复 scissor。
 */
void bongo_cat_preferences_overlay_draw(struct nk_context *context,
    struct nk_rect region, const BongoCatOverlayFrame *frame,
    BongoCatUIPalette palette) {
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    bool dark = bongo_cat_ui_dark(context);
    struct nk_color veil = dark ? nk_rgba(7, 10, 15,
        (nk_byte)(92.0f * frame->visibility + .5f)) : nk_rgba(255, 255, 255,
        (nk_byte)(148.0f * frame->visibility + .5f));
    nk_push_scissor(canvas, region);
    nk_fill_rect(canvas, region, 24, veil);
    if (palette.effects) bongo_cat_ui_paint_shadow(context, frame->panel,
        18, 0, 20, 50, 0, nk_rgba(0, 0, 0,
        (nk_byte)(77.0f * frame->visibility + .5f)));
}
