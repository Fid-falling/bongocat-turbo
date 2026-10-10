/*
 * 鼠标坐标 -> 模型输入的映射。这是"屏幕坐标只在这里被翻译成模型输入"的唯一一处, 改鼠标
 * 跟随行为基本都应该改这个文件。
 * 输入: 当帧解析好的坐标基准 (BongoCatMouseProjection) + 屏幕坐标 (或相对模式下的模型
 * 坐标)。输出: mver 手臂指针、ParamMouseX/Y、拖拽参数, 以及覆盖层的按键/指针状态。
 * 契约: map_pointer 返回 false 时调用方必须保留上一帧的显示位置 —— 指针位置不可信时归零
 * 会让手臂瞬间跳到左上角。只有 map_pointer 成功且位置真的变了, 才值得往下写参数。
 */
#include "runtime.h"
#include "mouse_internal.h"
#include "bongo_cat/overlay.h"

#include <string.h>

/* 把屏幕坐标 (绝对) 或位移 (相对模式) 映射成模型坐标, 写回 x/y 并置 *changed。
   相对模式只有指针被锁定 (cursor_locked) 时才可用: 平台拿不到位移增量时保持上一次的
   虚拟位置不变, 而不是把"没有增量"当成"指针回到 (0,0)"。 */
bool bongo_cat_app_map_pointer(BongoCatApp *app,
    const BongoCatMouseProjection *projection, bool relative_requested,
    double absolute_x, double absolute_y, double *x, double *y, bool *changed) {
    if (!app || !projection || !x || !y || !changed) return false;
    const BongoCatMverPointerBounds *bounds = &projection->bounds;
    double relative_x = 0.0, relative_y = 0.0;
    bool use_relative = relative_requested &&
        bongo_cat_platform_relative_pointer(&app->platform,
            &relative_x, &relative_y);
    bool initialized = app->mver_pointer.initialized;
    double previous_x = app->mver_pointer.x, previous_y = app->mver_pointer.y;
    /* 相对模式下一旦拿不到增量, 就用上次的位置顶替绝对坐标: mver_pointer_update 在
       已初始化时会在这个值上累加位移, 换成别的值会让指针自己漂移。 */
    /* Preserve the virtual position while relative samples are unavailable. */
    if (relative_requested && initialized && !use_relative) {
        absolute_x = previous_x;
        absolute_y = previous_y;
    }
    if (!bongo_cat_mver_pointer_update(&app->mver_pointer,
        absolute_x, absolute_y, relative_x, relative_y, use_relative,
        bounds, x, y)) return false;
    *changed = !initialized || previous_x != *x || previous_y != *y;
    return true;
}

/* 给一个模型参数写值。轴字符从参数 ID 的最后一个字符取 ("...X" / "...Y" / "...Z"),
   参数不存在就直接返回 —— 模型允许只实现一部分参数。 */
static void set_parameter(BongoCatApp *app, const char *id,
    float x_ratio, float y_ratio, bool horizontal_mirror) {
    BongoCatParameterRange range;
    if (!bongo_cat_live2d_parameter(app->live2d, id, &range)) return;
    size_t length = strlen(id);
    char axis = length ? id[length - 1] : 'X';
    float value = bongo_cat_mouse_parameter_value(range.minimum, range.maximum,
        x_ratio, y_ratio, axis, horizontal_mirror);
    bongo_cat_live2d_set_parameter(app->live2d, id, value);
}

/* 把这一帧的坐标落到模型上: 手/设备的参数、覆盖层的按键与指针贴图、眼睛拖拽参数。
   hand_x/hand_y 是已经映射过的模型坐标 (相对模式下可能与指针实际位置不同), gaze_x/gaze_y
   是屏幕坐标, 只有投影标记为 centered 时才会以模型锚点为中心重新算一遍比例。 */
void bongo_cat_app_apply_mouse_coordinates(BongoCatApp *app,
    const BongoCatMouseProjection *projection, double hand_x, double hand_y,
    double gaze_x, double gaze_y) {
    if (!app || !projection) return;
    const BongoCatMverPointerBounds *bounds = &projection->bounds;
    float hand_x_ratio, hand_y_ratio;
    if (!bongo_cat_mver_pointer_ratios(hand_x, hand_y, bounds,
        &hand_x_ratio, &hand_y_ratio)) return;
    /* 非居中时 gaze 与手共用同一份比例; centered 时以模型锚点为 0.5 重新映射, 这样
       "宠物在哪, 视线中心就在哪"。 */
    float gaze_x_ratio = hand_x_ratio, gaze_y_ratio = hand_y_ratio;
    if (projection->centered) {
        gaze_x_ratio = bongo_cat_mouse_centered_ratio(gaze_x, projection->center_x,
            bounds->left, bounds->left + bounds->width);
        gaze_y_ratio = bongo_cat_mouse_centered_ratio(gaze_y, projection->center_y,
            bounds->top, bounds->top + bounds->height);
    }
    /* 覆盖层用"精确指针"时 ParamMouseX/Y 由覆盖层自己驱动, 这里再写一遍会打架。 */
    bool exact_pointer = bongo_cat_overlay_mver_pointer_enabled(app->overlay);
    bool overlay_left_handed = exact_pointer &&
        bongo_cat_overlay_mver_pointer_left_handed(app->overlay);
    bool left_handed = app->model_render_options.pointer_left_handed ||
        overlay_left_handed;
    /* horizontal_mirror 是"最终是否镜像" (模型/配置的左右手倾向 + 鼠标镜像开关);
       overlay_mirror 再按覆盖层自己的左右手设置翻转一次, 两者不是同一个量。 */
    bool horizontal_mirror = left_handed != app->settings.model.mouse_mirror;
    float drag_x = 0.0f, drag_y = 0.0f;
    /* 拖拽参数 (眼睛/身体跟随) 用 gaze 比例。窗口拖拽期间两个坐标源都不可信, 跳过写入,
       否则松手瞬间模型会抽搐一下。 */
    bongo_cat_mouse_drag_coordinates(gaze_x_ratio, gaze_y_ratio,
        horizontal_mirror, &drag_x, &drag_y);
    bool overlay_mirror = horizontal_mirror != overlay_left_handed;
    float overlay_x_ratio = overlay_mirror
        ? 1.0f - hand_x_ratio : hand_x_ratio;
    /* 按键状态每帧都写: 手臂贴图的按下姿势由覆盖层维护, 只在移动时更新会让按键状态
       卡在上一帧。 */
    bongo_cat_overlay_set_mver_pointer(app->overlay, overlay_x_ratio, hand_y_ratio,
        app->left_mouse_down, app->right_mouse_down, app->side_mouse_down);
    if (!exact_pointer) {
        set_parameter(app, "ParamMouseX", hand_x_ratio, hand_y_ratio,
            horizontal_mirror);
        set_parameter(app, "ParamMouseY", hand_x_ratio, hand_y_ratio,
            horizontal_mirror);
    }
    /* Pointer and window messages are not atomic during a native window move. */
    if (!app->settings.model.mouse_centered || !app->window_drag_active) {
        if (app->settings.model.mouse_centered)
            bongo_cat_live2d_set_centered_dragging(app->live2d, drag_x, drag_y);
        else bongo_cat_live2d_set_dragging(app->live2d, drag_x, drag_y);
    }
/* 让下一次跟随重新定位: 换模型、改居中开关、窗口重新显示时调用。
   副作用包含释放平台层的相对指针资源, 并把"第一次要看绝对坐标"的标记清掉。 */
    app->dirty = true;
}

void bongo_cat_app_reset_pointer_tracking(BongoCatApp *app) {
    if (!app) return;
    bongo_cat_platform_relative_pointer_release(&app->platform);
    app->mver_pointer = (BongoCatMverPointerState){0};
    app->model_pointer_anchor_ready = false;
    app->pointer_known = false;
    app->mouse_last_ns = 0;
    app->pointer_relative_active = false;
    app->pointer_cursor_locked = false;
    app->mouse_button_event_pending = false;
    app->dirty = true;
}

/* 供拖拽与诊断使用的一次性入口: 直接把屏幕坐标当成指针位置, 只在这一帧里更新模型,
   不改 app->pointer_x/pointer_y (主循环的跟随流程仍以它为准)。 */
void bongo_cat_app_apply_mouse_position(BongoCatApp *app, double x, double y,
    float elapsed_seconds) {
/* ignore_mouse 时完全不动模型: 用户明确要求忽略鼠标事件。 */
    if (!app || app->settings.model.ignore_mouse) return;
    /* elapsed_seconds 目前不参与计算 (跟随平滑在 core/mouse_tracking.c 里做), 保留参数是
       为了签名稳定。 */

    (void)elapsed_seconds;
    BongoCatMouseProjection projection;
    if (bongo_cat_app_mouse_projection(app, x, y, &projection))
        bongo_cat_app_apply_mouse_coordinates(app, &projection, x, y, x, y);
}
