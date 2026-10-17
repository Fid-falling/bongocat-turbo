/*
 * 模型卡片上的两种"画出来的装饰": 沿卡片轮廓走的加载进度环, 以及右上角的选中角标。
 *
 * 进度环不能用 Nuklear 的圆弧 API —— 卡片的轮廓是**圆角矩形**, 库只提供画圆和画
 * 矩形描边。所以这里自己把圆角矩形的周长离散成一条折线 (四个角各 16 段), 再按弧长
 * 截取前 progress 比例的一段画出来。这也解释了那两个上限宏: 折线点数必须能装进栈上
 * 的固定数组, 段数一多就会溢出, 所以路径长度是编译期封顶的。
 * 端点收尾特意做了两处短路: progress≈0 直接不画, progress≈1 直接用
 * nk_stroke_rect 画整圈 —— 满圈时用折线反而会在起点/终点接缝处留一个小豁口。
 */
#include "preferences_model_card_paint.h"
#include "ui_icons.h"
#include "ui_paint.h"

#include <math.h>

/* 圆角分段数 (每个角) 与折线点数上限: 两者共同决定栈上路径数组的大小 */
#define MODEL_PROGRESS_ARC_SEGMENTS 16
#define MODEL_PROGRESS_POINT_CAP 80

/* 描边是沿路径中心线画的, 所以要内缩半个线宽, 否则粗描边会糊出卡片边界 */
struct nk_rect bongo_cat_preferences_model_card_outline_bounds(
    struct nk_rect bounds, float thickness) {
    float inset = thickness * .5f;
    return nk_rect(bounds.x + inset, bounds.y + inset,
        NK_MAX(1.0f, bounds.w - thickness),
        NK_MAX(1.0f, bounds.h - thickness));
}

/* 往路径里追加一个点; 满了就静默丢弃 (宁可路径短一点, 也不能越界写栈内存) */
static int progress_point(float *points, int count, float x, float y) {
    if (count >= MODEL_PROGRESS_POINT_CAP) return count;
    points[count * 2] = x;
    points[count * 2 + 1] = y;
    return count + 1;
}

/*
 * 把圆角矩形展开成一条闭合折线 (起点在左上角圆弧的起点, 顺时针走一圈再回到起点)。
 * 半径先夹到不超过短边的一半: 传进来的圆角可能大于卡片半高, 不夹的话四个圆心会
 * 交叉, 路径会自交成一团乱线。
 */
static int rounded_outline_path(struct nk_rect bounds, float rounding,
    float *points) {
    const float pi = 3.14159265358979323846f;
    float radius = NK_CLAMP(0.0f, rounding,
        NK_MIN(bounds.w, bounds.h) * .5f);
    float centers_x[] = {bounds.x + bounds.w - radius,
        bounds.x + bounds.w - radius, bounds.x + radius, bounds.x + radius};
    float centers_y[] = {bounds.y + radius, bounds.y + bounds.h - radius,
        bounds.y + bounds.h - radius, bounds.y + radius};
    float starts[] = {-pi * .5f, 0.0f, pi * .5f, pi};
    int count = 0;
    count = progress_point(points, count, bounds.x + radius, bounds.y);
    for (int corner = 0; corner < 4; ++corner) {
        for (int i = 0; i <= MODEL_PROGRESS_ARC_SEGMENTS; ++i) {
            float angle = starts[corner] + pi * .5f * i /
                MODEL_PROGRESS_ARC_SEGMENTS;
            count = progress_point(points, count,
                centers_x[corner] + cosf(angle) * radius,
                centers_y[corner] + sinf(angle) * radius);
        }
    }
    count = progress_point(points, count, points[0], points[1]);
    return count;
}

/*
 * 画进度环。先算累积弧长, 再按 progress 找到落点所在的那一段并做线性插值 ——
 * 这样进度是"按周长均匀推进"的, 而不是"按顶点均匀推进"(后者在四角会明显加速)。
 */
void bongo_cat_preferences_model_card_draw_progress(
    struct nk_command_buffer *canvas, struct nk_rect bounds, float rounding,
    float thickness, float progress, struct nk_color color) {
    progress = NK_CLAMP(0.0f, progress, 1.0f);
    if (progress <= 0.001f) return;
    if (progress >= .999f) {
        nk_stroke_rect(canvas, bounds, rounding, thickness, color);
        return;
    }
    float path[MODEL_PROGRESS_POINT_CAP * 2];
    int path_count = rounded_outline_path(bounds, rounding, path);
    if (path_count < 2) return;
    float lengths[MODEL_PROGRESS_POINT_CAP];
    lengths[0] = 0.0f;
    for (int i = 1; i < path_count; ++i) {
        float dx = path[i * 2] - path[(i - 1) * 2];
        float dy = path[i * 2 + 1] - path[(i - 1) * 2 + 1];
        lengths[i] = lengths[i - 1] + sqrtf(dx * dx + dy * dy);
    }
    float target = lengths[path_count - 1] * progress;
    float partial[MODEL_PROGRESS_POINT_CAP * 2];
    int partial_count = progress_point(partial, 0, path[0], path[1]);
    for (int i = 1; i < path_count; ++i) {
        if (target >= lengths[i]) {
            partial_count = progress_point(partial, partial_count,
                path[i * 2], path[i * 2 + 1]);
            continue;
        }
        float segment = lengths[i] - lengths[i - 1];
        float amount = segment > 0.0f ?
            (target - lengths[i - 1]) / segment : 0.0f;
        partial_count = progress_point(partial, partial_count,
            path[(i - 1) * 2] +
                (path[i * 2] - path[(i - 1) * 2]) * amount,
            path[(i - 1) * 2 + 1] +
                (path[i * 2 + 1] - path[(i - 1) * 2 + 1]) * amount);
        break;
    }
    if (partial_count >= 2)
        nk_stroke_polyline(canvas, partial, partial_count, thickness, color);
}

/*
 * 右上角的选中角标 (粉圆 + 白勾)。尺寸随出现动画从 85% 放大到 100%,
 * amount 为 0 时整块不画 —— 未选中的卡片不该有任何残留。
 */
void bongo_cat_preferences_model_card_draw_selection_badge(
    BongoCatPreferences *value, struct nk_command_buffer *canvas,
    struct nk_rect preview, BongoCatUIPalette palette, float amount) {
    if (amount <= .001f) return;
    float size = 25.0f * (.85f + .15f * amount);
    struct nk_rect badge = nk_rect(preview.x + preview.w - size - 8,
        preview.y + 8, size, size);
    nk_fill_circle(canvas, badge,
        bongo_cat_ui_color_alpha(palette.pink, amount));
    bongo_cat_preferences_icon_draw(value, canvas, BONGO_CAT_UI_ICON_CHECK,
        nk_rect(badge.x + 5, badge.y + 5, badge.w - 10, badge.h - 10),
        bongo_cat_ui_color_alpha(nk_rgb(255, 255, 255), amount));
}

/*
 * 左上角的"靠边梯形"角标 (顺序切换模型的参与/跳过标记)。两条平行斜边是
 * 45° 对角线, 另两条边分别沿预览的顶边和左边。斜边取在预览圆角的起始点
 * (距角点 radius 处), 不再画圆弧 —— 从圆弧开始的地方各往顶边/左边延伸出
 * 一个直角梯形, 既贴合圆角又不尖锐、不与角之间留间隔。size 是两直边长度。
 */
void bongo_cat_preferences_model_card_draw_corner_marker(
    struct nk_command_buffer *canvas, float x, float y, float size,
    float radius, struct nk_color color) {
    struct nk_vec2 pts[4];
    pts[0] = nk_vec2(x, y + size + 6.0f);   /* 左下, 沿左边向下并多探一段 */
    pts[1] = nk_vec2(x, y + radius);        /* 圆角在左边的起点 (贴左边) */
    pts[2] = nk_vec2(x + radius, y);        /* 圆角在顶边的起点 (贴顶边) */
    pts[3] = nk_vec2(x + size + 6.0f, y);   /* 沿顶边向右 (贴顶边) */
    nk_fill_polygon(canvas, (const float *)pts, 4, color);
}