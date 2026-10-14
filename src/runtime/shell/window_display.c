/* 多显示器几何: 判断窗口是否落在显示器"可用区域"的并集内, 越界时夹回或恢复。
 * 位置语义: 全部是屏幕坐标下的逻辑像素 (与 SDL_GetWindowPosition 一致), 不是渲染
 * 用的像素尺寸; keep_in_screen 只夹位置, 不改窗口尺寸。
 * 调用时序: 拖动/滚轮中随时可以调 clamp_to_display; 显示器热插拔与分辨率变化先由
 * display_event 记下, 再经 update_display_recovery 落地。
 * 易改错: DISPLAY_RECT_CAP 是栈数组的容量约定, 枚举结果超出就放弃判断 —— 宁愿不动。
 * 那个常量与"纯几何"的那一半函数住在 window_display_geometry.h/.c (行数策略, 见
 * cmake/CheckLines.cmake)。
 */
#include "runtime.h"
#include "window_display_geometry.h"

#include <math.h>

/* 显示器事件之后等多久再落地恢复 (见 update_display_recovery)。 */
#define DISPLAY_RECOVERY_DELAY_NS 750000000ull

/* 取窗口当前的位置与逻辑尺寸; 任一 SDL 调用失败或尺寸非正都返回 false, 调用方据此
   跳过后续几何处理 (窗口还没建好, 或正在被销毁)。 */
static bool window_rect(BongoCatApp *app, SDL_Rect *rect) {
    return app && app->window && rect &&
        SDL_GetWindowPosition(app->window, &rect->x, &rect->y) &&
        SDL_GetWindowSize(app->window, &rect->w, &rect->h) &&
        rect->w > 0 && rect->h > 0;
}



/* 收集显示器边界到调用方给的定长数组: 优先"可用区域" (扣掉任务栏/dock), 失败才退回
   整块显示器。返回写入的条数, 0 表示枚举失败或超出数组容量。 */
static int available_bounds(SDL_Rect bounds[DISPLAY_RECT_CAP]) {
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    if (!displays || count < 1 || count > DISPLAY_RECT_CAP) {
        SDL_free(displays); return 0;
    }
    int found = 0;
    for (int i = 0; i < count; ++i)
        if (SDL_GetDisplayUsableBounds(displays[i], &bounds[found]) ||
            SDL_GetDisplayBounds(displays[i], &bounds[found])) ++found;
    SDL_free(displays);
    return found;
}

/* 拖动中优先用拖动开始时缓存的显示器布局: 每次移动都重新枚举既慢, 又可能读到正在变化
   的布局; 没有缓存时现查。 */
static bool available_displays_cover(BongoCatApp *app,
    const SDL_Rect *window) {
    if (app && app->window_drag_active && app->drag_display_bounds &&
        app->drag_display_count > 0)
        return bongo_cat_window_bounds_covered(window, app->drag_display_bounds,
            app->drag_display_count);
    SDL_Rect bounds[DISPLAY_RECT_CAP];
    int count = available_bounds(bounds);
    return bongo_cat_window_bounds_covered(window, bounds, count);
}

/* 刷新缓存: 先释放旧块, 分配失败就保持"没有缓存"状态 (由 available_displays_cover
   退回现查)。所有权归 app, 由 bongo_cat_window_drag_bounds_clear 释放。 */
void bongo_cat_window_drag_bounds_refresh(BongoCatApp *app) {
    if (!app) return;
    bongo_cat_window_drag_bounds_clear(app);
    SDL_Rect *bounds = SDL_malloc(sizeof(*bounds) * DISPLAY_RECT_CAP);
    if (!bounds) return;
    int count = available_bounds(bounds);
    if (!count) { SDL_free(bounds); return; }
    app->drag_display_bounds = bounds;
    app->drag_display_count = count;
}

void bongo_cat_window_drag_bounds_clear(BongoCatApp *app) {
    if (!app) return;
    SDL_free(app->drag_display_bounds);
    app->drag_display_bounds = NULL;
    app->drag_display_count = 0;
}

/* 拖动/缩放时以指针所在显示器为准 (窗口还停在旧显示器上); 其余情况按窗口、再按矩形、
   最后落到主显示器逐级回退。跨文件: 滑行撞边 (window_inertia.c) 也用它挑边界, 于是
   "窗口该贴哪块屏"只有这一份实现。 */
SDL_DisplayID bongo_cat_window_target_display(BongoCatApp *app,
    const SDL_Rect *rect) {
    if (app->window_drag_active || app->resize_gesture) {
        float pointer_x = 0.0f, pointer_y = 0.0f;
        SDL_GetGlobalMouseState(&pointer_x, &pointer_y);
        SDL_Point pointer = {(int)pointer_x, (int)pointer_y};
        SDL_DisplayID display = SDL_GetDisplayForPoint(&pointer);
        if (display) return display;
    }
    SDL_DisplayID display = SDL_GetDisplayForWindow(app->window);
    if (!display && rect) display = SDL_GetDisplayForRect(rect);
    return display ? display : SDL_GetPrimaryDisplay();
}

/* 把窗口夹进指定显示器。返回 false 有两种含义: 位置本来就合法 (没动窗口), 或者平台
   拒绝移动 —— 调用方两种都只需当成"无需再处理"。 */
static bool fit_to_display(BongoCatApp *app, SDL_DisplayID display,
    const SDL_Rect *window) {
    SDL_Rect bounds;
    if (!display || (!SDL_GetDisplayUsableBounds(display, &bounds) &&
        !SDL_GetDisplayBounds(display, &bounds))) return false;
    SDL_Point next = bongo_cat_window_fitted_position(window, &bounds);
    if (next.x == window->x && next.y == window->y) return false;
    if (!SDL_SetWindowPosition(app->window, next.x, next.y)) return false;
    app->session.window.x = next.x;
    app->session.window.y = next.y;
    app->session.window.position_known = true;
    bongo_cat_window_mark_hit_dirty(app);
    return true;
}

/* keep_in_screen 打开时的入口: 已经被显示器并集覆盖就什么都不做, 免得拖动过程中每个
   鼠标事件都下发一次 SetWindowPos。 */
void bongo_cat_window_clamp_to_display(BongoCatApp *app) {
    SDL_Rect rect;
    if (!app || !app->settings.window.keep_in_screen || !window_rect(app, &rect)) return;
    if (available_displays_cover(app, &rect)) return;
    fit_to_display(app, bongo_cat_window_target_display(app, &rect), &rect);
}

/* 拖动落点, 入参是屏幕逻辑坐标: 越界时按目标显示器夹取, 坐标没变就直接返回 (拖动中
   大量重复事件)。有交互快照时同步预览几何, rect 为 NULL 表示跟随原生窗口矩形。 */

/* 边缘吸附: 拖动中把窗口贴到最近且在对向维度重叠的显示器边缘。方向维度 (x 或 y)
   overlap_* 要求窗口与显示器在另一维度有重叠, 避免隔着一条边时被远处边缘误吸附。
   吸附阈值 = 设置项 window.edge_snap_threshold, 可调 (0-64px, 见 config.h)。 */
static void snap_to_screen_edges(BongoCatApp *app, SDL_Point *next,
    int width, int height) {
    const SDL_Rect *bounds = app->drag_display_bounds;
    int count = app->drag_display_count;
    int snap_x = 0, snap_y = 0;
    int threshold = app->settings.window.edge_snap_threshold;
    int best_x = threshold, best_y = threshold;
    bool found_x = false, found_y = false;
    if (!app->settings.window.edge_snap || !bounds || count < 1) return;
    for (int i = 0; i < count; ++i) {
        bool overlap_y = next->y < bounds[i].y + bounds[i].h &&
            (int64_t)next->y + height > bounds[i].y;
        bool overlap_x = next->x < bounds[i].x + bounds[i].w &&
            (int64_t)next->x + width > bounds[i].x;
        const int xs[2] = { bounds[i].x,
            bounds[i].x + bounds[i].w - width };
        const int ys[2] = { bounds[i].y,
            bounds[i].y + bounds[i].h - height };
        for (int c = 0; c < 2; ++c) {
            double dx = SDL_fabs((double)xs[c] - next->x);
            double dy = SDL_fabs((double)ys[c] - next->y);
            if (overlap_y && dx < best_x) {
                best_x = (int)dx; snap_x = xs[c]; found_x = true;
            }
            if (overlap_x && dy < best_y) {
                best_y = (int)dy; snap_y = ys[c]; found_y = true;
            }
        }
    }
    if (found_x) next->x = snap_x;
    if (found_y) next->y = snap_y;
}

void bongo_cat_window_drag_to(BongoCatApp *app, int x, int y) {
    int width = 0, height = 0, current_x = 0, current_y = 0;
    if (!app || !app->window || !SDL_GetWindowSize(app->window, &width, &height) ||
        width <= 0 || height <= 0) return;
    SDL_Rect requested = {x, y, width, height};
    SDL_Point next = {x, y};
    snap_to_screen_edges(app, &next, width, height);
    if (app->settings.window.keep_in_screen &&
        !available_displays_cover(app, &requested)) {
        SDL_Rect bounds;
        SDL_DisplayID display = bongo_cat_window_target_display(app, &requested);
        if (display && (SDL_GetDisplayUsableBounds(display, &bounds) ||
            SDL_GetDisplayBounds(display, &bounds)))
            next = bongo_cat_window_fitted_position(&requested, &bounds);
    }
    if (SDL_GetWindowPosition(app->window, &current_x, &current_y) &&
        current_x == next.x && current_y == next.y) return;
    if (!SDL_SetWindowPosition(app->window, next.x, next.y)) return;
    app->session.window.x = next.x; app->session.window.y = next.y;
    app->session.window.position_known = true;
    bongo_cat_window_mark_hit_dirty(app);
    if (app->window_snapshot) {
        bongo_cat_window_snapshot_geometry(app, NULL);
        app->dirty = true;
    }
}

/* keep_in_screen 关闭时的补救: 只在窗口与所有显示器都不相交时才拉回, 部分越界是允许的
   (桌宠可以骑在屏幕边缘上); 枚举不到显示器时不动窗口。 */
bool bongo_cat_window_recover_to_display(BongoCatApp *app) {
    SDL_Rect rect;
    if (!window_rect(app, &rect)) return false;
    bool known = false;
    if (bongo_cat_window_intersects_displays(&rect, &known) || !known) return false;
    SDL_DisplayID display = SDL_GetDisplayForRect(&rect);
    if (!display) display = SDL_GetPrimaryDisplay();
    return fit_to_display(app, display, &rect);
}

/* 显示器事件 (热插拔/分辨率/DPI): 先结束交互快照, 重置指针追踪, 再把恢复排到 750ms
   之后 —— 系统切换显示模式时会连发多个事件, 立刻处理会反复移动窗口。 */
void bongo_cat_window_display_event(BongoCatApp *app, const SDL_Event *event) {
    if (!app || !event) return;
    if (event->type < SDL_EVENT_DISPLAY_FIRST ||
        event->type > SDL_EVENT_DISPLAY_LAST) return;
    /* 显示器布局要变了: 正在滑行的窗口必须立刻停下 (边界本身马上就不成立了)。 */
    bongo_cat_window_stop_inertia(app);
    bongo_cat_window_snapshot_end(app);
    if (app->window_drag_active && app->settings.window.keep_in_screen)
        bongo_cat_window_drag_bounds_refresh(app);
    bongo_cat_app_reset_pointer_tracking(app);
    app->display_recovery_due_ns = SDL_GetTicksNS() + DISPLAY_RECOVERY_DELAY_NS;
}

/* 主循环每轮调用; due 为 0 表示没有待办。到点后按 keep_in_screen 选择钳制或恢复, 并
   清掉 due, 保证只执行一次。 */
void bongo_cat_window_update_display_recovery(BongoCatApp *app, uint64_t now) {
    if (!app || !app->display_recovery_due_ns ||
        now < app->display_recovery_due_ns) return;
    app->display_recovery_due_ns = 0;
    if (app->settings.window.keep_in_screen) bongo_cat_window_clamp_to_display(app);
    else bongo_cat_window_recover_to_display(app);
}
