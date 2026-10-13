/*
 * 分段选择器 (胶囊控件) 的两块"叶子"实现, 从 preferences_theme.c 搬出来:
 *   1) 三个手绘特效图标 (电脑/太阳/月亮);
 *   2) "这一段被点了吗"的判定。
 *
 * 为什么单独一个文件: preferences_theme.c 贴着 cmake/CheckLines.cmake 的 500 行上限,
 * 而下面这两段的注释比代码还长 —— 它们踩过的坑都是实测出来的 (一次点击被重放成 6 次
 * 切换、点不动却一条日志都没有), 只能留在代码旁边, 不能为了行数删掉。
 */
#include "preferences_theme.h"

#include <stddef.h>

/*
 * 手绘的三个图标。背景色必须传进来: 月亮是"画一个圆, 再用背景色盖掉一个偏移的圆"
 * 抠出来的, 换主题背景色变了而这里没跟着换, 月牙就是花的。
 */
static void draw_computer(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color) {
    float x = bounds.x + bounds.w * .5f, y = bounds.y + bounds.h * .5f;
    nk_stroke_rect(canvas, nk_rect(x - 8, y - 7, 16, 11), 2, 1.7f, color);
    nk_stroke_line(canvas, x, y + 4, x, y + 7, 1.7f, color);
    nk_stroke_line(canvas, x - 5, y + 7, x + 5, y + 7, 1.7f, color);
}

static void draw_sun(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color) {
    float x = bounds.x + bounds.w * .5f, y = bounds.y + bounds.h * .5f;
    nk_stroke_circle(canvas, nk_rect(x - 4, y - 4, 8, 8), 1.7f, color);
    const float rays[][4] = {
        {0, -9, 0, -7}, {0, 7, 0, 9}, {-9, 0, -7, 0}, {7, 0, 9, 0},
        {-6.4f, -6.4f, -5, -5}, {5, 5, 6.4f, 6.4f},
        {-6.4f, 6.4f, -5, 5}, {5, -5, 6.4f, -6.4f}
    };
    for (size_t i = 0; i < sizeof(rays) / sizeof(rays[0]); ++i)
        nk_stroke_line(canvas, x + rays[i][0], y + rays[i][1],
            x + rays[i][2], y + rays[i][3], 1.7f, color);
}

static void draw_moon(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color,
    struct nk_color background) {
    float x = bounds.x + bounds.w * .5f;
    float y = bounds.y + bounds.h * .5f;
    nk_fill_circle(canvas, nk_rect(x - 7.5f, y - 7.5f, 15, 15), color);
    nk_fill_circle(canvas, nk_rect(x - 3.4f, y - 8.3f, 12.4f, 12.4f),
        background);
}

void bongo_cat_pref_segment_icon(struct nk_command_buffer *canvas, int icon,
    struct nk_rect bounds, struct nk_color color,
    struct nk_color background) {
    if (icon == 0) draw_computer(canvas, bounds, color);
    else if (icon == 1) draw_sun(canvas, bounds, color);
    else draw_moon(canvas, bounds, color, background);
}

/*
 * 每个控件一份点击状态, 键是控件的 id —— 调用方一律传字符串字面量, 比较指针就够。
 * 槽位固定: 一帧里最多画两三个分段控件 (偏好页: 最大帧率在前, 渲染后端在后),
 * 用满就退化成"这一帧不认点击", 不会串到别的控件上。
 */
#define BONGO_CAT_SEGMENT_SLOTS 8

typedef struct SegmentPress {
    const char *id;
    bool down;        /* 上一帧这个按键是不是按下的 */
    bool armed;       /* 按下时指针在本控件内: 松手只有它还成立才算一次点击 */
    unsigned clicks;  /* 上一帧 Nuklear 的点击计数 */
} SegmentPress;

static SegmentPress segment_press[BONGO_CAT_SEGMENT_SLOTS];

static SegmentPress *segment_slot(const char *id) {
    for (int i = 0; i < BONGO_CAT_SEGMENT_SLOTS; ++i) {
        if (segment_press[i].id == id) return &segment_press[i];
        if (!segment_press[i].id) {
            segment_press[i].id = id;
            return &segment_press[i];
        }
    }
    return NULL;
}

/*
 * "用户这一帧点在本控件上了吗" —— hover 由调用方按控件矩形算好传进来。
 *
 * 为什么必须按控件分开记状态 (上一版共用一个全局状态, 那正是"Vulkan 点不动"的直接
 * 原因): 一帧里会画好几个分段控件, 共用状态时排在前面的控件会先把这一击消费掉, 真正
 * 被点的那一个反而收不到 —— 界面上就是点不动, 日志里一个字都没有。
 *
 * 为什么不能直接用 Nuklear 的 nk_input_is_mouse_click_in_rect: 那个判据要 clicked 非零,
 * 而 clicked 只在 nk_input_begin 里清零。设置窗口是事件驱动渲染, 渲染次数与 input_begin
 * 不是一一对应 (中途还会弹模态确认框把主循环停住), 同一个 clicked 值会被后一帧再看一遍 ——
 * 实测一次点击在 2 秒内把渲染后端来回切了 6 次。
 *
 * 三种投递方式都要认, 少一种就是一个"点不动":
 *   release_edge  这一帧从按下变成松开, 而且按下时指针就在本控件内 (拖出去再松开不算);
 *                 真人点击最常见的样子, 按下与松开被两次事件轮询分开。
 *   batch_click   按下与松开落在同一批事件里 (触摸板轻点 / 注入式输入 / 快速点击): 这时
 *                 根本没有"按下帧"可看, 只能拿当前悬停当判据; 必须比对 clicked 计数,
 *                 否则那一击会被之后每一帧重放。
 * 之后的重绘帧两个条件都不成立, 所以同一击只算一次。
 */
bool bongo_cat_pref_segment_click(struct nk_context *context, const char *id,
    bool hover) {
    SegmentPress *slot = segment_slot(id);
    if (!slot) return false;
    const struct nk_mouse_button *button =
        &context->input.mouse.buttons[NK_BUTTON_LEFT];
    const bool down = button->down != 0;
    const unsigned clicks = button->clicked;
    bool clicked = false;
    if (down) {
        slot->armed = hover;
    } else if (slot->down) {
        clicked = slot->armed && hover;
        slot->armed = false;
    } else if (clicks != 0 && clicks != slot->clicks) {
        clicked = hover;
    }
    slot->down = down;
    slot->clicks = clicks;
    return clicked;
}
