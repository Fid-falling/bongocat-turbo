/* 指针审计: 把鼠标依次放到显示器四角附近, 每处推 90 帧 (1.5s) 让参数收敛, 再把 ParamAngleX/Y 与
 * ParamMouseX/Y 采进全局 bongo_cat_pointer_audit 供断言读取, 并记录相邻帧 ParamAngleX 的最大跳变。
 */
#include "live2d_pointer_audit.h"

/* 全局单例: 断言写在 live2d_audit.c, 结果靠这个模块级符号传递; 只有 reverse 审计会重置它并把 ran 置真。 */
BongoCatPointerAudit bongo_cat_pointer_audit;

static bool value(BongoCatApp *app, const char *id, float *output) {
    BongoCatParameterRange range;
    if (!bongo_cat_live2d_parameter(app->live2d, id, &range)) return false;
    if (output) *output = range.value;
    return true;
}

/* 取窗口中心点所在的那块显示器 (不是主显示器), 输出显示器边界与窗口中心坐标。用窗口中心而不是鼠标位置
 * 选显示器, 是因为多屏下窗口可能横跨两块屏; 任一步失败都返回 false, 调用方据此判"环境不具备审计条件"。
 */
static bool test_center(BongoCatApp *app, SDL_Rect *bounds,
    double *center_x, double *center_y) {
    int window_x, window_y, width, height;
    if (!app || !app->window || !bounds || !center_x || !center_y ||
        !SDL_GetWindowPosition(app->window, &window_x, &window_y) ||
        !SDL_GetWindowSize(app->window, &width, &height)) return false;
    *center_x = window_x + width * 0.5;
    *center_y = window_y + height * 0.5;
    SDL_Point point = {(int)*center_x, (int)*center_y};
    SDL_DisplayID display = SDL_GetDisplayForPoint(&point);
    return display && SDL_GetDisplayBounds(display, bounds) &&
        bounds->w > 0 && bounds->h > 0;
}

/* mirror 直接写进 settings.model.mouse_mirror 且不恢复 (场景本身就是"带镜像跑一次"), 落点固定在
 * 窗口中心右上方 40% 处; 真正的方向判定在 live2d_audit.c 的 mouse-move 分支里。
 */
bool bongo_cat_live2d_pointer_audit_run(BongoCatApp *app, bool mirror) {
    SDL_Rect bounds;
    double center_x, center_y;
    if (!test_center(app, &bounds, &center_x, &center_y)) return false;
    app->settings.model.mouse_mirror = mirror;
    app->left_mouse_down = false;
    bongo_cat_app_apply_mouse_position(app, center_x + bounds.w * 0.4,
        center_y - bounds.h * 0.4, 1.0f / 60.0f);
    for (int frame = 0; frame < 90; ++frame)
        bongo_cat_app_step_live2d(app, 1.0f / 60.0f);
    return true;
}

/* 四角顺序 tl/tr/bl/br 与断言里的下标一一对应, 改顺序会让 mouse-reverse 的断言全反。期望: 左右两侧
 * 给出符号相反的 ParamAngleX (左负右正), 上下两侧给出符号相反的 ParamAngleY (上正下负)。
 * maximum_step 记录相邻帧的最大跳变 (每帧变化量, 不是总位移), 上限由 POINTER_VISIBLE_STEP_LIMIT 判定。
 */
bool bongo_cat_live2d_pointer_reverse_audit_run(BongoCatApp *app) {
    SDL_Rect bounds;
    double center_x, center_y;
    if (!test_center(app, &bounds, &center_x, &center_y)) return false;
    bongo_cat_pointer_audit = (BongoCatPointerAudit){.ran = true};
    app->settings.model.mouse_mirror = false;
    app->left_mouse_down = false;
    const float ratios[4][2] = {
        {-0.4f, -0.4f}, {0.4f, -0.4f}, {-0.4f, 0.4f}, {0.4f, 0.4f}};
    float previous_x = 0.0f;
    bool previous_ready = value(app, "ParamAngleX", &previous_x);
    for (int corner = 0; corner < 4; ++corner) {
        bongo_cat_app_apply_mouse_position(app,
            center_x + bounds.w * ratios[corner][0],
            center_y + bounds.h * ratios[corner][1], 1.0f / 60.0f);
        for (int frame = 0; frame < 90; ++frame) {
            bongo_cat_app_step_live2d(app, 1.0f / 60.0f);
            float current_x = previous_x;
            if (!value(app, "ParamAngleX", &current_x)) return false;
            if (previous_ready) {
                float step = SDL_fabsf(current_x - previous_x);
                if (step > bongo_cat_pointer_audit.maximum_step)
                    bongo_cat_pointer_audit.maximum_step = step;
            }
            previous_x = current_x;
            previous_ready = true;
        }
        if (!value(app, "ParamAngleX",
                &bongo_cat_pointer_audit.angle_x[corner]) ||
            !value(app, "ParamAngleY",
                &bongo_cat_pointer_audit.angle_y[corner])) return false;
        bool mouse_x = value(app, "ParamMouseX",
            &bongo_cat_pointer_audit.mouse_x[corner]);
        bool mouse_y = value(app, "ParamMouseY",
            &bongo_cat_pointer_audit.mouse_y[corner]);
        /* 四个角必须一致地"有/没有" ParamMouseX 与 ParamMouseY: 模型不该只在某些姿态下才有这组参数。 */
        if (corner == 0)
            bongo_cat_pointer_audit.has_mouse = mouse_x && mouse_y;
        else if (bongo_cat_pointer_audit.has_mouse != (mouse_x && mouse_y))
            return false;
    }
    return true;
}
