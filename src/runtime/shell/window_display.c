/* 多显示器几何: 判断窗口是否落在显示器"可用区域"的并集内, 越界时夹回或恢复。
 * 位置语义: 全部是屏幕坐标下的逻辑像素 (与 SDL_GetWindowPosition 一致), 不是渲染
 * 用的像素尺寸; keep_in_screen 只夹位置, 不改窗口尺寸。
 * 调用时序: 拖动/滚轮中随时可以调 clamp_to_display; 显示器热插拔与分辨率变化先由
 * display_event 记下, 再经 update_display_recovery 落地。
 * 易改错: DISPLAY_RECT_CAP 是栈数组的容量约定, 枚举结果超出就放弃判断 —— 宁愿不动。
 */
#include "runtime.h"
#include <math.h>

#define DISPLAY_RECOVERY_DELAY_NS 750000000ull
#define DISPLAY_RECT_CAP 32
/* 滑行公开成员: 每帧衰减乘数按"winhawk 标称 15.6ms 一拍"折算成指数, 帧率无关。 */
#define SLIDE_NOMINAL_FRAME_S 0.0156
#define SLIDE_MAX_FRAME 50
#define SLIDE_STOP_SPEED_PPS_SQ 100.0

/* 取窗口当前的位置与逻辑尺寸; 任一 SDL 调用失败或尺寸非正都返回 false, 调用方据此
   跳过后续几何处理 (窗口还没建好, 或正在被销毁)。 */
static bool window_rect(BongoCatApp *app, SDL_Rect *rect) {
    return app && app->window && rect &&
        SDL_GetWindowPosition(app->window, &rect->x, &rect->y) &&
        SDL_GetWindowSize(app->window, &rect->w, &rect->h) &&
        rect->w > 0 && rect->h > 0;
}

static bool intersects_bounds(const SDL_Rect *window,
    const SDL_Rect *bounds, int count) {
    if (!window || !bounds || count < 1) return false;
    for (int i = 0; i < count; ++i)
        if (SDL_HasRectIntersection(window, &bounds[i])) return true;
    return false;
}

/* 窗口是否与任一显示器相交。*known 区分"确实都不相交"与"枚举不到显示器": 后者调用方
   不能动窗口 —— 在没有输出的环境里没有可夹取的边界。列表由 SDL 分配, 这里负责 free。 */
static bool intersects_available_display(const SDL_Rect *window, bool *known) {
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    *known = displays && count > 0;
    bool intersects = false;
    for (int i = 0; displays && i < count && !intersects; ++i) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(displays[i], &bounds))
            intersects = SDL_HasRectIntersection(window, &bounds);
    }
    SDL_free(displays);
    return intersects;
}

/* 纯计算, 不碰窗口: 把位置夹进一个显示器的 bounds。SDL_max 保证下界不超过上界, 窗口比
   显示器还大时结果会退化到 bounds 的左上角 (这种尺寸本来也放不进去)。 */
static SDL_Point fitted_position(const SDL_Rect *window,
    const SDL_Rect *bounds) {
    int max_x = SDL_max(bounds->x, bounds->x + bounds->w - window->w);
    int max_y = SDL_max(bounds->y, bounds->y + bounds->h - window->h);
    SDL_Point point = {SDL_clamp(window->x, bounds->x, max_x),
        SDL_clamp(window->y, bounds->y, max_y)};
    return point;
}

static bool clipped_rect(const SDL_Rect *window, const SDL_Rect *bounds,
    SDL_Rect *clip) {
    int right = SDL_min(window->x + window->w, bounds->x + bounds->w);
    int bottom = SDL_min(window->y + window->h, bounds->y + bounds->h);
    clip->x = SDL_max(window->x, bounds->x);
    clip->y = SDL_max(window->y, bounds->y);
    clip->w = right - clip->x;
    clip->h = bottom - clip->y;
    return clip->w > 0 && clip->h > 0;
}

static void sort_edges(int *values, int count) {
    for (int i = 1; i < count; ++i) {
        int value = values[i], j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1]; --j;
        }
        values[j] = value;
    }
}

static void sort_clips(SDL_Rect *values, int count) {
    for (int i = 1; i < count; ++i) {
        SDL_Rect value = values[i]; int j = i;
        while (j > 0 && values[j - 1].y > value.y) {
            values[j] = values[j - 1]; --j;
        }
        values[j] = value;
    }
}

/* 窗口矩形是否被若干显示器矩形的并集完整覆盖 (允许跨显示器拼接与负坐标)。做法是按所有
   左右边界切竖条, 每条内按 y 贪心累加覆盖高度, 有一条没盖满就返回 false。边最多 66 个,
   所以排序用插入排序; count 超上限或参数非法同样返回 false。 */
static bool bounds_cover_window(const SDL_Rect *window,
    const SDL_Rect *bounds, int count) {
    if (!window || !bounds || window->w <= 0 || window->h <= 0 ||
        count < 1 || count > DISPLAY_RECT_CAP) return false;
    SDL_Rect clips[DISPLAY_RECT_CAP];
    int edges[DISPLAY_RECT_CAP * 2 + 2];
    int clip_count = 0, edge_count = 2;
    edges[0] = window->x; edges[1] = window->x + window->w;
    for (int i = 0; i < count; ++i) {
        SDL_Rect clip;
        if (!clipped_rect(window, &bounds[i], &clip)) continue;
        clips[clip_count++] = clip;
        edges[edge_count++] = clip.x;
        edges[edge_count++] = clip.x + clip.w;
    }
    if (!clip_count) return false;
    sort_edges(edges, edge_count); sort_clips(clips, clip_count);
    int window_bottom = window->y + window->h;
    for (int edge = 1; edge < edge_count; ++edge) {
        int left = edges[edge - 1], right = edges[edge];
        if (left == right) continue;
        int covered_to = window->y;
        for (int i = 0; i < clip_count; ++i) {
            int clip_right = clips[i].x + clips[i].w;
            if (clips[i].x > left || clip_right < right) continue;
            if (clips[i].y > covered_to) break;
            covered_to = SDL_max(covered_to, clips[i].y + clips[i].h);
            if (covered_to >= window_bottom) break;
        }
        if (covered_to < window_bottom) return false;
    }
    return true;
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
        return bounds_cover_window(window, app->drag_display_bounds,
            app->drag_display_count);
    SDL_Rect bounds[DISPLAY_RECT_CAP];
    int count = available_bounds(bounds);
    return bounds_cover_window(window, bounds, count);
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
   最后落到主显示器逐级回退。 */
static SDL_DisplayID target_display(BongoCatApp *app, const SDL_Rect *rect) {
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
    SDL_Point next = fitted_position(window, &bounds);
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
    fit_to_display(app, target_display(app, &rect), &rect);
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
        SDL_Rect bounds; SDL_DisplayID display = target_display(app, &requested);
        if (display && (SDL_GetDisplayUsableBounds(display, &bounds) ||
            SDL_GetDisplayBounds(display, &bounds))) next = fitted_position(&requested, &bounds);
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
    if (intersects_available_display(&rect, &known) || !known) return false;
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
    app->slide_active = false;
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

/* 滑行遇到屏幕边缘: 贴着可用区边界反向弹回一点点 (×0.05, 同 winhawk), 并刹住越界方向
   的速度, 让惯性看起来像"被屏幕挡住弹了一下"而不是飞出桌面。 */
static void slide_bounce(BongoCatApp *app, int *x, int *y,
    int width, int height, double *vx, double *vy) {
    SDL_Rect requested = {*x, *y, width, height};
    SDL_DisplayID display = target_display(app, &requested);
    if (!display) display = SDL_GetPrimaryDisplay();
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(display, &bounds)) return;
    int min_x = bounds.x, min_y = bounds.y;
    int max_x = bounds.x + bounds.w - width, max_y = bounds.y + bounds.h - height;
    if (*x < min_x) { *x = min_x; *vx = fabs(*vx) * 0.05; }
    else if (*x > max_x) { *x = max_x; *vx = -fabs(*vx) * 0.05; }
    if (*y < min_y) { *y = min_y; *vy = fabs(*vy) * 0.05; }
    else if (*y > max_y) { *y = max_y; *vy = -fabs(*vy) * 0.05; }
}

/* 主循环每帧调用: 拖着 window 沿惯性方向衰减移动。初速在松手时由 window_drag.c 算好。
   衰减用指数 (按 dt 折算到 SLIDE_NOMINAL_FRAME_S 一拍), 帧率无关; 撞到屏幕边缘会弹回;
   速度过低/超帧数/窗口不可见就停下并夹回显示器。 */
void bongo_cat_window_update_inertia(BongoCatApp *app, uint64_t now) {
    if (!app || !app->slide_active) return;
    if (!app->window || !app->session.window.visible ||
        app->window_minimized || app->hover_hidden) {
        app->slide_active = false;
        return;
    }
    double dt = (double)(now - app->slide_last_ns) / 1.0e9;
    app->slide_last_ns = now;
    if (dt <= 0.0 || dt > 0.25) return;
    int slowdown = app->settings.window.slide_slowdown_percent;
    if (slowdown < 1) slowdown = 1;
    else if (slowdown > BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT)
        slowdown = BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT;
    double mult = (100.0 - slowdown) / 100.0;
    double decay = pow(mult, dt / SLIDE_NOMINAL_FRAME_S);
    app->slide_velocity_x *= decay;
    app->slide_velocity_y *= decay;
    app->slide_x += app->slide_velocity_x * dt;
    app->slide_y += app->slide_velocity_y * dt;
    int width = 0, height = 0, x = 0, y = 0;
    if (!SDL_GetWindowSize(app->window, &width, &height) ||
        width <= 0 || height <= 0) {
        app->slide_active = false;
        return;
    }
    x = (int)lround(app->slide_x);
    y = (int)lround(app->slide_y);
    slide_bounce(app, &x, &y, width, height,
        &app->slide_velocity_x, &app->slide_velocity_y);
    app->slide_x = (double)x;
    app->slide_y = (double)y;
    if (!SDL_SetWindowPosition(app->window, x, y)) {
        app->slide_active = false;
        return;
    }
    app->session.window.x = x;
    app->session.window.y = y;
    app->session.window.position_known = true;
    bongo_cat_window_mark_hit_dirty(app);
    if (app->window_snapshot) {
        bongo_cat_window_snapshot_geometry(app, NULL);
        app->dirty = true;
    }
    app->slide_frame++;
    double speed_sq = app->slide_velocity_x * app->slide_velocity_x +
        app->slide_velocity_y * app->slide_velocity_y;
    if (speed_sq < SLIDE_STOP_SPEED_PPS_SQ || app->slide_frame >= SLIDE_MAX_FRAME) {
        app->slide_active = false;
        if (app->settings.window.keep_in_screen) bongo_cat_window_clamp_to_display(app);
    }
}

/* 纯逻辑自检, 不创建也不移动真实窗口: 用构造出来的显示器布局覆盖拼接、负坐标、纵向堆叠、
   显示器间留缝等情形; 传入 app 时还会验证 display 事件确实重置了指针状态并排定了恢复。
   返回 false 时由调用方报错。 */
bool bongo_cat_window_display_self_test(BongoCatApp *app) {
    const SDL_Rect displays[] = {{0, 0, 1920, 1040}, {1920, 0, 1920, 1040}};
    const SDL_Rect gapped[] = {{0, 0, 1920, 1040}, {2020, 0, 1920, 1040}};
    const SDL_Rect negative[] = {{-1920, 0, 1920, 1080}, {0, 0, 2560, 1440}};
    const SDL_Rect stacked[] = {{0, -1200, 1920, 1200}, {0, 0, 1920, 1080},
        {1920, 0, 1280, 1024}};
    SDL_Rect partial = {-80, 100, 320, 240};
    SDL_Rect secondary = {2200, 100, 320, 240};
    SDL_Rect detached = {4200, 100, 320, 240};
    SDL_Rect bridge = {1800, 100, 320, 240};
    SDL_Rect negative_bridge = {-100, 100, 200, 200};
    SDL_Rect vertical_bridge = {100, -100, 200, 200};
    SDL_Rect triple_bridge = {1800, 100, 300, 200};
    SDL_Rect corner_gap = {1800, -100, 240, 200};
    SDL_Point fitted = fitted_position(&detached, &displays[0]);
    bool overlap = intersects_bounds(&partial, displays, 2);
    bool second = intersects_bounds(&secondary, displays, 2);
    bool removed = !intersects_bounds(&secondary, displays, 1);
    bool recovered = fitted.x == 1600 && fitted.y == 100;
    bool adjacent_covered = bounds_cover_window(&bridge, displays, 2);
    bool gap_rejected = !bounds_cover_window(&bridge, gapped, 2);
    bool event_reset = false;
    if (app) {
        bool pointer_known = app->pointer_known;
        bool anchor_ready = app->model_pointer_anchor_ready;
        bool dirty = app->dirty;
        uint64_t recovery_due = app->display_recovery_due_ns;
        app->pointer_known = true; app->model_pointer_anchor_ready = true;
        app->dirty = false; app->display_recovery_due_ns = 0;
        SDL_Event event = {.type = SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED};
        bongo_cat_window_display_event(app, &event);
        event_reset = !app->pointer_known && !app->model_pointer_anchor_ready &&
            app->dirty && app->display_recovery_due_ns > 0;
        app->pointer_known = pointer_known;
        app->model_pointer_anchor_ready = anchor_ready;
        app->dirty = dirty; app->display_recovery_due_ns = recovery_due;
    }
    return overlap && second && removed && recovered && adjacent_covered &&
        gap_rejected && !bounds_cover_window(&partial, displays, 2) &&
        !intersects_bounds(&detached, displays, 2) && event_reset &&
        bounds_cover_window(&negative_bridge, negative, 2) &&
        bounds_cover_window(&vertical_bridge, stacked, 3) &&
        bounds_cover_window(&triple_bridge, stacked, 3) &&
        !bounds_cover_window(&corner_gap, stacked, 3);
}