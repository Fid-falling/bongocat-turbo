#include "bongo_cat/mver_pointer.h"

#include <math.h>
#include <stddef.h>

/*
 * Mver(Live2D 旧格式)手臂跟随鼠标的几何计算。
 *
 * 这里所有坐标都在"模型画布坐标系"里(与 1000 归一化画布同一套单位)。常量
 * (211,159)、(258,228)、60.0、20.0 这些是照着该模型原始布局复刻出来的固有
 * 几何, 不是可调参数, 不能凭手感改; 真正可调的部分在 config(hand_offset /
 * offset)里, 由用户设置的模型偏移提供。
 */
typedef struct DoublePoint {
    double x;
    double y;
} DoublePoint;

/* factorial[n] 存的是 n!/1000 而不是 n!: 直接算到 20! 会超出 double 的整数
   精度。代价是 bezier 里算出的权重整体大了 1000 倍, 所以那里末尾要把结果除回
   1000 —— 改动这张表就必须同时改那处除法。 */
static const double factorial[] = {
    0.001, 0.001, 0.002, 0.006, 0.024, 0.12, 0.72, 5.04,
    40.32, 362.88, 3628.8, 39916.8, 479001.6, 6227020.8,
    87178291.2, 1307674368.0, 20922789888.0, 355687428096.0,
    6402373705728.0, 121645100408832.0, 2432902008176640.0,
    51090942171709440.0
};

/* 一般形式的 n 次贝塞尔(伯恩斯坦展开): points 是控制点, ratio 取 [0,1]。
   阶数由控制点个数决定, 最大用到 18 阶, 所以下面 path 固定 19 个点。 */
static DoublePoint bezier(double ratio, const DoublePoint *points,
    size_t count) {
    DoublePoint result = {0.0, 0.0};
    size_t degree = count - 1;
    for (size_t point = 0; point <= degree; ++point) {
        double coefficient = factorial[degree] /
            (factorial[point] * factorial[degree - point]);
        double weight = coefficient * pow(ratio, (double)point) *
            pow(1.0 - ratio, (double)(degree - point));
        result.x += points[point].x * weight;
        result.y += points[point].y * weight;
    }
    result.x /= 1000.0;
    result.y /= 1000.0;
    return result;
}

static double clamp_ratio(float value) {
    if (value < 0.0f) return 0.0;
    if (value > 1.0f) return 1.0;
    return value;
}

/* 绝对坐标 -> [0,1] 比例。越界只钳制不算失败: 鼠标可能跑到窗口外, 对模型来说
   "贴住边缘"比"不动"更合理。只有 bounds 非法(宽高 <= 0)才返回 false。 */
bool bongo_cat_mver_pointer_ratios(double x, double y,
    const BongoCatMverPointerBounds *bounds,
    float *x_ratio, float *y_ratio) {
    if (!bounds || !x_ratio || !y_ratio || !isfinite(x) || !isfinite(y) ||
        !isfinite(bounds->left) || !isfinite(bounds->top) ||
        !isfinite(bounds->width) || !isfinite(bounds->height) ||
        bounds->width <= 0.0 || bounds->height <= 0.0) return false;
    *x_ratio = (float)clamp_ratio((float)((x - bounds->left) / bounds->width));
    *y_ratio = (float)clamp_ratio((float)((y - bounds->top) / bounds->height));
    return true;
}

/*
 * 指针位置的累积与钳制, 两种模式:
 *   - 绝对模式(use_relative=false): 直接用系统给的绝对坐标并重置 initialized;
 *   - 相对模式(true): 在已有状态上累加位移, 用于拿不到绝对坐标的场合(远程
 *     桌面、独占全屏等), 但第一次仍然要用绝对坐标定初值。
 * 结果始终钳制在 bounds 内: 越界会让手臂贴图跑出手臂骨架。
 */
bool bongo_cat_mver_pointer_update(BongoCatMverPointerState *state,
    double absolute_x, double absolute_y, double relative_x,
    double relative_y, bool use_relative,
    const BongoCatMverPointerBounds *bounds, double *x, double *y) {
    if (!state || !bounds || !x || !y || !isfinite(absolute_x) ||
        !isfinite(absolute_y) || !isfinite(relative_x) ||
        !isfinite(relative_y) || bounds->width <= 0.0 ||
        bounds->height <= 0.0) return false;
    if (!state->initialized || !use_relative) {
        state->x = absolute_x;
        state->y = absolute_y;
        state->initialized = true;
    }
    if (use_relative) {
        state->x += relative_x;
        state->y += relative_y;
    }
    double right = bounds->left + bounds->width;
    double bottom = bounds->top + bounds->height;
    if (state->x < bounds->left) state->x = bounds->left;
    if (state->x > right) state->x = right;
    if (state->y < bounds->top) state->y = bounds->top;
    if (state->y > bottom) state->y = bottom;
    *x = state->x;
    *y = state->y;
    return true;
}

/*
 * 由鼠标比例位置反推手臂三段曲线的采样点, 输出 BONGO_CAT_MVER_ARM_POINT_COUNT
 * 个骨架点。这是一个封闭几何: 19 个点的拼装顺序(左曲线采样 -> 端点 -> 中段
 * 曲线 -> 端点 -> 右曲线逆序采样 -> 锚点)必须和末尾的 path_count != 19 校验
 * 一致, 少一个点整帧就被判失败。
 * 末尾用 isfinite 兜底: 输入退化(两点重合导致法线长度为 0)时宁可让调用方放弃
 * 这一帧, 也不要把 NaN 写进骨架。
 */
bool bongo_cat_mver_pointer_geometry(float x_ratio, float y_ratio,
    const BongoCatMverPointerConfig *config,
    BongoCatMverPointerGeometry *geometry) {
    if (!config || !geometry) return false;
    double fx = clamp_ratio(x_ratio), fy = clamp_ratio(y_ratio);
    double x = -97.0 * fx + 44.0 * fy + 184.0;
    double y = -76.0 * fx - 40.0 * fy + 324.0;
    const double dx = -38.0, dy = -50.0;
    const int samples = 6;
    DoublePoint path[19];
    size_t path_count = 0;
    path[path_count++] = (DoublePoint){211.0, 159.0};

    double distance = hypot(211.0 - x, 159.0 - y);
    double centre_left_x = 211.0 - 0.7237 * distance / 2.0;
    double centre_left_y = 159.0 + 0.69 * distance / 2.0;
    DoublePoint left_curve[] = {
        {211.0, 159.0}, {centre_left_x, centre_left_y}, {x, y}
    };
    for (int index = 1; index < samples; ++index)
        path[path_count++] = bezier((double)index / samples, left_curve, 3);
    path[path_count++] = (DoublePoint){x, y};

    double normal_x = y - centre_left_y;
    double normal_y = centre_left_x - x;
    double normal_length = hypot(normal_x, normal_y);
    if (normal_length <= 0.0) return false;
    double a = x + normal_x / normal_length * 60.0;
    double b = y + normal_y / normal_length * 60.0;
    const double anchor_x = 258.0, anchor_y = 228.0;
    distance = hypot(anchor_x - a, anchor_y - b);
    double centre_right_x = anchor_x - 0.6 * distance / 2.0;
    double centre_right_y = anchor_y + 0.8 * distance / 2.0;
    const double push = 20.0;
    double tangent_x = x - centre_left_x;
    double tangent_y = y - centre_left_y;
    double tangent_length = hypot(tangent_x, tangent_y);
    if (tangent_length <= 0.0) return false;
    tangent_x *= push / tangent_length;
    tangent_y *= push / tangent_length;
    double tangent2_x = a - centre_right_x;
    double tangent2_y = b - centre_right_y;
    tangent_length = hypot(tangent2_x, tangent2_y);
    if (tangent_length <= 0.0) return false;
    tangent2_x *= push / tangent_length;
    tangent2_y *= push / tangent_length;
    DoublePoint middle_curve[] = {
        {x, y}, {x + tangent_x, y + tangent_y},
        {a + tangent2_x, b + tangent2_y}, {a, b}
    };
    for (int index = 1; index < samples; ++index)
        path[path_count++] = bezier((double)index / samples, middle_curve, 4);
    path[path_count++] = (DoublePoint){a, b};

    DoublePoint right_curve[] = {
        {anchor_x, anchor_y}, {centre_right_x, centre_right_y}, {a, b}
    };
    for (int index = samples - 1; index > 0; --index)
        path[path_count++] = bezier((double)index / samples, right_curve, 3);
    path[path_count++] = (DoublePoint){anchor_x, anchor_y};
    if (path_count != 19) return false;

    for (size_t index = 0; index < BONGO_CAT_MVER_ARM_POINT_COUNT; ++index) {
        DoublePoint point = index == 0 ? path[0] :
            index == BONGO_CAT_MVER_ARM_POINT_COUNT - 1 ? path[18] :
            bezier((double)index / 25.0, path, 19);
        geometry->arm[index].x = (float)(point.x + dx + config->hand_offset_x);
        geometry->arm[index].y = (float)(point.y + dy + config->hand_offset_y);
    }
    double device_x = (a + x) / 2.0 - 52.0 - 15.0;
    double device_y = (b + y) / 2.0 - 34.0 + 5.0;
    geometry->device_x = (float)(device_x + dx + config->offset_x +
        config->hand_offset_x);
    geometry->device_y = (float)(device_y + dy + config->offset_y +
        config->hand_offset_y);
    return isfinite(geometry->device_x) && isfinite(geometry->device_y);
}
