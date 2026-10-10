#include "bongo_cat/mouse.h"

#include <math.h>

/*
 * 鼠标跟随的数学部分: 采样新鲜度、指数平滑、归一化坐标换算。
 *
 * 新鲜度窗口 100ms: 超过这个时间的样本说明输入链路已经停了(失去焦点、钩子被
 * 摘掉、设备拔出), 这时宁可不动, 也不能拿一个陈旧坐标继续驱动模型。
 * 时间戳来自单调时钟(ns), 所以只比较差值; now 比 sample 还早(时钟异常)时直接
 * 判为不新鲜。
 */
bool bongo_cat_mouse_sample_fresh(uint64_t now_ns, uint64_t sample_ns) {
    return sample_ns && now_ns >= sample_ns &&
        now_ns - sample_ns < 100000000ull;
}

/* 设置目标点。第一次收到目标时把当前位置直接对齐过去(initialized), 否则窗口
   刚出现时猫会从 (0,0) 一路"飞"到鼠标那儿。settled 复位, 让下一步重新平滑。 */
void bongo_cat_mouse_target(BongoCatMouseTracking *tracking, double x, double y) {
    if (!tracking) return;
    tracking->target_x = x;
    tracking->target_y = y;
    if (!tracking->initialized) {
        tracking->current_x = x;
        tracking->current_y = y;
        tracking->initialized = true;
    }
    tracking->settled = false;
}

/* 指数平滑: alpha 用 pow(0.75, dt*60) 计算, 保证不同帧率下的跟随手感一致
   (60fps 时每帧走 25%)。残差小于 0.5px 就吸附到目标并置 settled, 这样静止时
   不再产生无意义的参数更新。返回 false 表示这一步没有新位置。 */
bool bongo_cat_mouse_step(BongoCatMouseTracking *tracking, float delta_seconds,
    double *x, double *y) {
    if (!tracking || !tracking->initialized || tracking->settled || !x || !y)
        return false;
    if (delta_seconds < 0.0f) delta_seconds = 0.0f;
    double alpha = 1.0 - pow(0.75, (double)delta_seconds * 60.0);
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    tracking->current_x += (tracking->target_x - tracking->current_x) * alpha;
    tracking->current_y += (tracking->target_y - tracking->current_y) * alpha;
    double dx = tracking->target_x - tracking->current_x;
    double dy = tracking->target_y - tracking->current_y;
    if (dx * dx + dy * dy < 0.25) {
        tracking->current_x = tracking->target_x;
        tracking->current_y = tracking->target_y;
        tracking->settled = true;
    }
    *x = tracking->current_x;
    *y = tracking->current_y;
    return true;
}

/* 以 center 为 0.5 把 [minimum, maximum] 映射到 [0, 1], 左右两侧各自线性
   拉伸。于是"鼠标在屏幕中心"对应参数中点, 而不是把 center 当作坐标中点。 */
float bongo_cat_mouse_centered_ratio(double position, double center,
    double minimum, double maximum) {
    if (!isfinite(position) || !isfinite(center) || !isfinite(minimum) ||
        !isfinite(maximum) || maximum <= minimum) return 0.5f;
    if (center < minimum) center = minimum;
    if (center > maximum) center = maximum;
    if (position < center) {
        double span = center - minimum;
        if (span <= 0.0) return 0.5f;
        double ratio = 0.5 * (position - minimum) / span;
        return (float)(ratio < 0.0 ? 0.0 : ratio);
    }
    if (position > center) {
        double span = maximum - center;
        if (span <= 0.0) return 0.5f;
        double ratio = 0.5 + 0.5 * (position - center) / span;
        return (float)(ratio > 1.0 ? 1.0 : ratio);
    }
    return 0.5f;
}

static float clamp_pointer_ratio(float value) {
    if (!isfinite(value)) return 0.5f;
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/* 归一化比例 -> Live2D 拖拽参数空间: X 落在 [-1, 1], Y 轴向下为正所以要翻转
   (1 - 2*ratio)。镜像只翻 X, 不动 Y。 */
void bongo_cat_mouse_drag_coordinates(float x_ratio, float y_ratio,
    bool horizontal_mirror, float *x, float *y) {
    float horizontal = clamp_pointer_ratio(x_ratio);
    float vertical = clamp_pointer_ratio(y_ratio);
    if (horizontal_mirror) horizontal = 1.0f - horizontal;
    if (x) *x = 2.0f * horizontal - 1.0f;
    if (y) *y = 1.0f - 2.0f * vertical;
}

/* 归一化值 -> 实际参数值。区间跨 0 时按"正负分别映射到最大/最小值"处理, 这样
   参数在中立位附近是对称的; 区间不跨 0 就退化成普通线性映射。 */
static float parameter_from_normalized(float minimum, float maximum,
    float normalized) {
    if (!isfinite(minimum) || !isfinite(maximum) || maximum < minimum)
        return 0.0f;
    if (minimum <= 0.0f && maximum >= 0.0f)
        return normalized >= 0.0f ? normalized * maximum :
            -normalized * minimum;
    return minimum + (normalized + 1.0f) * 0.5f * (maximum - minimum);
}

/* 轴字符是 Mver 的约定: 'Y' 用纵坐标, 'Z' 用 x*y(斜向轴的复合量), 其余都当
   横坐标处理。 */
float bongo_cat_mouse_parameter_value(float minimum, float maximum,
    float x_ratio, float y_ratio, char axis, bool mirror) {
    float x = 0.0f, y = 0.0f;
    bongo_cat_mouse_drag_coordinates(x_ratio, y_ratio, mirror, &x, &y);
    float normalized = axis == 'Y' ? y : axis == 'Z' ? -x * y : x;
    return parameter_from_normalized(minimum, maximum, normalized);
}
