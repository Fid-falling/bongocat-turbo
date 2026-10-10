/*
 * 指针坐标系的解析: 决定这一帧的"指针比例"是按哪块矩形算的。
 * 三套坐标系: mouse_centered 用模型可见像素的锚点 + 指针所在显示器; mver_projection 用
 * 模型元数据里标定的 pointer bounds, 没有标定就退回桌面; 其余情况用指针所在显示器。
 * 结果 (BongoCatMouseProjection) 必须与当帧的指针一起交给 mouse_mapping/mouse_pipeline,
 * 不要缓存跨帧使用: 窗口或显示器变化后缓存值会算错, 而错的是"手指位置"这类难查的现象。
 * Windows 上的坑: 测量桌面矩形前后要临时切到 SYSTEM_AWARE 的 DPI 感知上下文, 否则
 * 高 DPI 下拿到的是被虚拟化过的逻辑像素, 比例会整体缩水。
 * 本文件不碰窗口不透明度与穿透, 只回答"这一帧的坐标基准是什么"。
 */
#include "runtime.h"
#include "mouse_internal.h"

#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* 解析 mver 投影用的矩形, 结果写进 bounds 并返回是否可用。优先用模型标定的 pointer bounds,
   不可用再退回桌面。调用方已经保证了 options->mver_projection。 */
static bool mver_pointer_bounds(const BongoCatApp *app, SDL_Rect *bounds) {
    const BongoCatLive2DRenderOptions *options = &app->model_render_options;
    if (!options->mver_projection || !bounds) return false;
    /* custom_pointer_bounds 存的是左上坐标 + 右下坐标, 这里转成 SDL 的 x/y + 宽高;
       宽高非正说明标定数据坏了, 返回 false 让调用方退回桌面而不是画出 0 宽区域。 */
    if (options->custom_pointer_bounds) {
        *bounds = (SDL_Rect){options->pointer_left, options->pointer_top,
            options->pointer_right - options->pointer_left,
            options->pointer_bottom - options->pointer_top};
        return bounds->w > 0 && bounds->h > 0;
    }
#ifdef _WIN32
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContextFn)(
        DPI_AWARENESS_CONTEXT);
    FARPROC set_thread_dpi_proc = GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    SetThreadDpiAwarenessContextFn set_thread_dpi = NULL;
    memcpy(&set_thread_dpi, &set_thread_dpi_proc, sizeof(set_thread_dpi));
    DPI_AWARENESS_CONTEXT previous = set_thread_dpi
        ? set_thread_dpi(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) : NULL;
    RECT desktop = {0};
    bool available = GetWindowRect(GetDesktopWindow(), &desktop) != FALSE;
    /* 只在测量期间切 DPI 感知, 测完立刻恢复: 这个设置是按线程的, 留着会改变其它代码
       (包括 SDL) 的坐标语义。用 GetProcAddress 而不是直接调, 是为了兼容老系统上没有
       这个入口的情况。 */
    if (previous && set_thread_dpi) set_thread_dpi(previous);
    if (available && desktop.right > 0 && desktop.bottom > 0) {
        /* Mver 0.1.6 uses a zero origin and stores right/bottom as dimensions. */
    /* Mver 0.1.6 的桌面矩形原点在绝对 (0,0), right/bottom 就是尺寸; 这里必须保持同样的
       语义 —— 换成显示器相对坐标会让标定过的指针偏移整体错位。 */
        *bounds = (SDL_Rect){0, 0, desktop.right, desktop.bottom};
        return true;
    }
#endif
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_Rect display;
    if (!primary || !SDL_GetDisplayBounds(primary, &display)) return false;
    *bounds = (SDL_Rect){0, 0, display.w, display.h};
    return bounds->w > 0 && bounds->h > 0;
}

/* 钳到 [0,1]。这个函数不做 isfinite 检查, 因为越界值 (指针滑出屏幕) 是正常输入;
   非有限值由调用方负责 (anchor 那一处会先判 isfinite 再钳)。 */
static float clamp_ratio(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/* 反解模型锚点的屏幕坐标。可见包络是 NDC ([-1,1], 向上为正), 这里换算成窗口内的 0..1
   比例: x 用 0.5*x+0.5, y 因为方向相反用 0.5-0.5*y。
   只在第一次拿到有效包络时算一次并缓存: 包络要等模型真正画过一帧才有 (fitted/visible),
   而 resize/换模型/切镜像会让 model_pointer_anchor_ready 失效, 重新走这里。 */
static bool model_pointer_center(BongoCatApp *app, double *x, double *y) {
    int window_x, window_y, width, height;
    if (!app || !app->window || !x || !y ||
        !SDL_GetWindowPosition(app->window, &window_x, &window_y) ||
        !SDL_GetWindowSize(app->window, &width, &height) ||
        width <= 0 || height <= 0) return false;
    if (!app->model_pointer_anchor_ready) {
    /* 包络还没准备好时退化成窗口几何中心 (锚点比例默认 0.5, 0.5)。 */
        BongoCatLive2DVisualState state = {0};
        if (bongo_cat_live2d_visual_state(app->live2d, &state) && state.visible) {
            float center_x = (state.visible_min_x + state.visible_max_x) * 0.5f;
            float center_y = (state.visible_min_y + state.visible_max_y) * 0.5f;
            if (isfinite(center_x) && isfinite(center_y)) {
                app->model_pointer_anchor_x = clamp_ratio(center_x * 0.5f + 0.5f);
                app->model_pointer_anchor_y = clamp_ratio(0.5f - center_y * 0.5f);
                app->model_pointer_anchor_ready = true;
            }
        }
    }
    float anchor_x = app->model_pointer_anchor_ready
        ? app->model_pointer_anchor_x : 0.5f;
    float anchor_y = app->model_pointer_anchor_ready
        ? app->model_pointer_anchor_y : 0.5f;
    *x = window_x + (double)width * anchor_x;
    *y = window_y + (double)height * anchor_y;
    return true;
}

/* 模型锚点所在显示器的矩形, 并把锚点的屏幕坐标回传给调用方 (居中的 gaze 比例要用它)。
   锚点可能落在窗口外, 所以用"点 -> 显示器"查询而不是窗口所属显示器。 */
static bool model_pointer_bounds(BongoCatApp *app, SDL_Rect *bounds,
    double *center_x, double *center_y) {
    double visible_center_x, visible_center_y;
    if (!bounds || !model_pointer_center(app, &visible_center_x,
        &visible_center_y)) return false;
    if (center_x) *center_x = visible_center_x;
    if (center_y) *center_y = visible_center_y;
    SDL_Point point = {(int)visible_center_x, (int)visible_center_y};
    SDL_DisplayID display = SDL_GetDisplayForPoint(&point);
    return display && SDL_GetDisplayBounds(display, bounds) &&
        bounds->w > 0 && bounds->h > 0;
}

/* 决定这一帧的投影基准, 成功才填 projection 并返回 true。centered 字段同时表示"是否
   按模型锚点居中", 调用方据此决定 gaze 比例怎么算。
   指针所在显示器是最后兜底: 多显示器下宠物被拖到副屏时, 比例必须按副屏算。 */
bool bongo_cat_app_mouse_projection(BongoCatApp *app, double screen_x,
    double screen_y, BongoCatMouseProjection *projection) {
    if (!app || !projection || !isfinite(screen_x) || !isfinite(screen_y))
        return false;
    *projection = (BongoCatMouseProjection){0};
    SDL_Rect bounds = {0};
    bool centered = app->settings.model.mouse_centered;
    projection->centered = centered && model_pointer_bounds(app, &bounds,
        &projection->center_x, &projection->center_y);
    bool mapped = projection->centered || (!centered &&
        mver_pointer_bounds(app, &bounds));
    /* 三套基准都拿不到 (没有显示器信息) 才失败; 失败时调用方沿用上一帧的显示位置。 */
    if (!mapped) {
        SDL_Point point = {(int)screen_x, (int)screen_y};
        SDL_DisplayID display = SDL_GetDisplayForPoint(&point);
        if (!display || !SDL_GetDisplayBounds(display, &bounds)) return false;
    }
    if (bounds.w <= 0 || bounds.h <= 0) return false;
    /* SDL_Rect 用整数存显示器绝对坐标 (多显示器时可为负), mouse.h 的 bounds 用 double
       存, 以保留副屏与 DPI 缩放下的比例精度。 */

    projection->bounds = (BongoCatMverPointerBounds){
        bounds.x, bounds.y, bounds.w, bounds.h
    };
    return true;
}
