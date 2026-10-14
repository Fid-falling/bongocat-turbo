/*
 * 显示器几何的纯计算 + 自检 (声明见 window_display_geometry.h)。
 *
 * 这里只做计算: 不移动窗口、不读 app 的设置, 所以可以在自检里用构造出来的显示器布局
 * 直接验证"拼接 / 负坐标 / 纵向堆叠 / 显示器间留缝"这些情形。真正动窗口的 clamp /
 * 拖动落点 / 恢复逻辑都在 window_display.c。
 *
 * 拆出来的原因: window_display.c 已经贴着 500 行的策略上限 (见 cmake/CheckLines.cmake),
 * 而这一块与"什么时候该动窗口"没有共享状态。
 */
#include "window_display_geometry.h"
#include "runtime.h"

#include <SDL3/SDL.h>

bool bongo_cat_window_intersects_bounds(const SDL_Rect *window,
    const SDL_Rect *bounds, int count) {
    if (!window || !bounds || count < 1) return false;
    for (int i = 0; i < count; ++i)
        if (SDL_HasRectIntersection(window, &bounds[i])) return true;
    return false;
}


/* 窗口是否与任一显示器相交。*known 区分"确实都不相交"与"枚举不到显示器": 后者调用方
   不能动窗口 —— 在没有输出的环境里没有可夹取的边界。列表由 SDL 分配, 这里负责 free。 */
bool bongo_cat_window_intersects_displays(const SDL_Rect *window, bool *known) {
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
SDL_Point bongo_cat_window_fitted_position(const SDL_Rect *window,
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
bool bongo_cat_window_bounds_covered(const SDL_Rect *window,
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
    SDL_Point fitted = bongo_cat_window_fitted_position(&detached, &displays[0]);
    bool overlap = bongo_cat_window_intersects_bounds(&partial, displays, 2);
    bool second = bongo_cat_window_intersects_bounds(&secondary, displays, 2);
    bool removed = !bongo_cat_window_intersects_bounds(&secondary, displays, 1);
    bool recovered = fitted.x == 1600 && fitted.y == 100;
    bool adjacent_covered = bongo_cat_window_bounds_covered(&bridge, displays, 2);
    bool gap_rejected = !bongo_cat_window_bounds_covered(&bridge, gapped, 2);
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
        gap_rejected && !bongo_cat_window_bounds_covered(&partial, displays, 2) &&
        !bongo_cat_window_intersects_bounds(&detached, displays, 2) && event_reset &&
        bongo_cat_window_bounds_covered(&negative_bridge, negative, 2) &&
        bongo_cat_window_bounds_covered(&vertical_bridge, stacked, 3) &&
        bongo_cat_window_bounds_covered(&triple_bridge, stacked, 3) &&
        !bongo_cat_window_bounds_covered(&corner_gap, stacked, 3);
}
