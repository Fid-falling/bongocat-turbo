/*
 * 松手后的惯性滑行 (window_display.c 只留窗口几何与夹取; 这里只动窗口位置)。
 *
 * 为什么单独一个文件: window_display.c 贴着 500 行的策略上限 (见 cmake/CheckLines.cmake),
 * 而滑行是一个自成一体的逐帧状态机 (初速在 window_drag.c 松手时算好, 这里只负责推进),
 * 与"窗口该在哪块显示器上"没有共享状态。
 *
 * 速度与衰减只有一套状态 (app->slide_velocity_x/y + window.slide_slowdown_percent),
 * 设置页里也只有一组相关开关。它们的关系是 —— 改这一块之前先把这三条读一遍:
 *
 *   1. window.slide_slowdown_percent: 每拍速度乘 (100-值)/100。0 = 不衰减, 即恒速。
 *   2. window.slide_no_decay ("一直滑动", 拖动惯性打开时才显示的那一行): 一个布尔快捷
 *      方式, 它只把**本帧生效的**放缓值钉在 0。用户自己拖的百分比原样留在配置里, 关掉
 *      这一项立刻按原值恢复衰减。它不引入第二套速度状态, 也没有自己的判停分支。
 *   3. 生效的放缓值为 0 (= 恒速) 时, 撞边必须完整反射 (restitution 1.0) 并且跳过拍数
 *      兜底 —— 否则"恒速"会被这两条各自掐断。于是这种滑行永不自行停止 (屏保那种在屏幕上
 *      撞来撞去的效果), 停止条件只剩显式的那几个: 再次抓住宠物/拖动、关掉"一直滑动"或
 *      "拖动惯性"、窗口隐藏或最小化、显示器变化、程序退出, 全部经由
 *      bongo_cat_window_stop_inertia 收口。
 *
 *   由此: 手动把滑条拖到 0 与打开"一直滑动"是同一件事 (同一个语义不留两种行为)。这是
 *   刻意的, 而且与旧值自洽 —— v1.16.3 的默认值就是 0 (1.16.4 起默认改成 15), 老配置里
 *   那个 0 的字面意思本来就是"不衰减"。
 *
 * 易改错: 判停用的是"整数像素有没有变"而不是速度阈值 (见 slide_step 的注释), 所以
 * "永不停止"的前提是速度真的能推动像素; 窗口比显示器还大那种退化情形由 slide_bounce 里的
 * max < min 分支清零速度收尾。
 */
#include "runtime.h"

#include <limits.h>
#include <math.h>

/* 滑行: 真实时间累积成 15.6ms 的标称拍, 每拍推进一次 (x += v*0.0156) 并乘一次衰减。
   按真实时间累积 (不是"每个渲染帧算一拍") 让墙钟节奏与刷新率无关, 高刷屏不会滑得更快。 */
#define SLIDE_NOMINAL_FRAME_S 0.0156
/* 普通惯性的拍数上限: 到点就停 (恒速模式不看它, 见 SLIDE_FRAME_CAP_NONE)。 */
#define SLIDE_MAX_FRAME 50
/* 撞边反弹: 法向速度反向, 只保留 5% 速度。 */
#define SLIDE_BOUNCE_RESTITUTION 0.05
/* 恒速 (放缓值为 0) 时改用完整反射 (1.0 = 一点速度都不损失), 这是"永不自行停止"的前提。 */
#define SLIDE_BOUNCE_NO_LOSS 1.0
/* 恒速模式不设拍数上限: 传这个当上限, 判停只剩"整数像素没前进"那一条。 */
#define SLIDE_FRAME_CAP_NONE INT_MAX

/* 滑行遇到屏幕边缘: 贴到目标显示器可用区边界, 法向速度反向并保留 restitution 比例的速度
   (镜面反射), 让桌宠像球撞墙一样弹回屏幕内, 而不是一路飞出桌面。
   restitution 由调用方给: 普通惯性滑行用 SLIDE_BOUNCE_RESTITUTION (撞一次就软下来),
   恒速滑行给 1.0 —— 完整反射才可能永不停止 (屏保效果)。 */
static void slide_bounce(BongoCatApp *app, int *x, int *y,
    int width, int height, double *vx, double *vy, double restitution) {
    SDL_Rect requested = {*x, *y, width, height};
    SDL_DisplayID display = bongo_cat_window_target_display(app, &requested);
    if (!display) display = SDL_GetPrimaryDisplay();
    SDL_Rect bounds;
    /* 注意: SDL_GetDisplayUsableBounds 成功返回 true, 失败返回 false —— 必须取反, 否则
       正常取到边界时会提前 return, 让下面的贴边/反弹变成死代码, 桌宠直接飞出屏幕。 */
    if (!display || !SDL_GetDisplayUsableBounds(display, &bounds)) return;
    int min_x = bounds.x, min_y = bounds.y;
    int max_x = bounds.x + bounds.w - width, max_y = bounds.y + bounds.h - height;
    /*
     * 窗口比显示器可用区还大时 (max < min) 两边墙重合不上, 没有"反弹"这回事: 强行反射会
     * 让它每拍在两条边之间来回跳 —— 位置来回变但永远不满足"整数像素没变就停", 于是恒速
     * 滑行就成了停不下来的抖动。这种情形贴住左上角并把该轴速度清零, 交给判停收尾;
     * 正常尺寸的窗口走下面原来的镜面反射 (撞哪条边翻哪个方向, 撞墙角两条各自弹一次)。
     */
    if (max_x < min_x) { *x = min_x; *vx = 0.0; }
    else if (*x < min_x) { *x = min_x; *vx = fabs(*vx) * restitution; }
    else if (*x > max_x) { *x = max_x; *vx = -fabs(*vx) * restitution; }
    if (max_y < min_y) { *y = min_y; *vy = 0.0; }
    else if (*y < min_y) { *y = min_y; *vy = fabs(*vy) * restitution; }
    else if (*y > max_y) { *y = max_y; *vy = -fabs(*vy) * restitution; }
}

/* 立刻结束松手滑行。抓住宠物、关掉开关、窗口隐藏/最小化、显示器变化、退出都会走这里 ——
   幂等, 没在滑行时调用也安全。恒速滑行不会自己停下, 所以这些停止条件是必需的。 */
void bongo_cat_window_stop_inertia(BongoCatApp *app) {
    if (!app) return;
    app->slide_active = false;
    app->slide_velocity_x = 0.0;
    app->slide_velocity_y = 0.0;
    app->slide_frame = 0;
}

/* 单拍推进, 等价于定时器收到一次 15.6ms 回调: 先按当前速度移动 -> 整数像素没前进就停 ->
   撞边反弹 -> 乘一次衰减 -> 落位。返回 true 表示还要继续下一拍。
   mult 是这一拍的速度倍率 (1.0 = 不衰减), restitution 是撞边保留比例, frame_cap 是拍数
   上限 —— 三者都由调用方按"普通惯性 / 恒速"选择, 这里不再判断模式。 */
static bool slide_step(BongoCatApp *app, double mult, double restitution,
    int frame_cap) {
    int width = 0, height = 0, actual_x = 0, actual_y = 0;
    if (!SDL_GetWindowSize(app->window, &width, &height) ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (!SDL_GetWindowPosition(app->window, &actual_x, &actual_y)) return false;
    /* 位置对不上上一拍设下的值, 说明窗口被别的东西动了 (吸附/夹取/系统改动), 停止滑行。 */
    if (app->slide_frame != 0 &&
        (actual_x != app->slide_last_set_x || actual_y != app->slide_last_set_y)) {
        return false;
    }
    int prev_x = (int)app->slide_x;
    int prev_y = (int)app->slide_y;
    app->slide_x += app->slide_velocity_x * SLIDE_NOMINAL_FRAME_S;
    app->slide_y += app->slide_velocity_y * SLIDE_NOMINAL_FRAME_S;
    int x = (int)app->slide_x;
    int y = (int)app->slide_y;
    /* 判停比较"位移": 整数像素没前进就停, 不用速度阈值。小数余量留在 slide_x/slide_y 里
       (不每拍取整), 亚像素阶段会攒够 1px 再走; 反过来走到这里说明速度已经推不动一个像素
       (含撞进 max < min 那种退化边界), 停下比空转合理 —— 恒速模式也一样。 */
    if (x == prev_x && y == prev_y) return false;
    int before_x = x, before_y = y;
    slide_bounce(app, &x, &y, width, height,
        &app->slide_velocity_x, &app->slide_velocity_y, restitution);
    if (x != before_x || y != before_y) {
        /* 撞边被夹回边界: 位置对齐到边界, 丢掉小数余量。 */
        app->slide_x = (double)x;
        app->slide_y = (double)y;
    }
    /* 反弹之后才按本拍衰减: 先移动 -> 撞边反弹 -> 再衰减。恒速时 mult 是 1.0 (放缓值为 0),
       速度一点不损失, 所以它会一直撞下去。 */
    app->slide_velocity_x *= mult;
    app->slide_velocity_y *= mult;
    if (!SDL_SetWindowPosition(app->window, x, y)) return false;
    app->slide_last_set_x = x;
    app->slide_last_set_y = y;
    app->session.window.x = x;
    app->session.window.y = y;
    app->session.window.position_known = true;
    bongo_cat_window_mark_hit_dirty(app);
    if (app->window_snapshot) {
        bongo_cat_window_snapshot_geometry(app, NULL);
        app->dirty = true;
    }
    app->slide_frame++;
    return app->slide_frame < frame_cap;
}

/* 主循环每帧调用: 拖着 window 沿惯性方向衰减移动。初速在松手时由 window_drag.c 算好。
   真实时间累积成 15.6ms 的拍, 每拍推进一次 (x += v*0.0156) 并乘一次衰减 —— 墙钟节奏与
   固定 15.6ms 定时器一致, 不随渲染帧率变。撞到屏幕边缘会弹回; 整数像素不再前进/窗口被
   外力移动/超拍数/窗口不可见就停下并夹回显示器。恒速模式 (放缓值 0) 只保留前两条自停,
   其余停止条件见 bongo_cat_window_stop_inertia。 */
void bongo_cat_window_update_inertia(BongoCatApp *app, uint64_t now) {
    if (!app || !app->slide_active) return;
    /* 拖动惯性被关掉: 立即停下, 不留在半滑行状态。 */
    if (!app->settings.window.slide_animation) {
        bongo_cat_window_stop_inertia(app);
        return;
    }
    if (!app->window || !app->session.window.visible ||
        app->window_minimized || app->hover_hidden) {
        bongo_cat_window_stop_inertia(app);
        return;
    }
    double dt = (double)(now - app->slide_last_ns) / 1.0e9;
    app->slide_last_ns = now;
    if (dt <= 0.0 || dt > 0.25) return;
    /*
     * 本拍生效的放缓值只有这一个来源: window.slide_slowdown_percent。"一直滑动"
     * (slide_no_decay) 不是第二套机制, 它只是把这个值钉在 0; 用户自己拖的百分比仍留在
     * 配置里, 关掉这一项立刻按原值衰减 —— 所以这里算出来的 slowdown 就是全局唯一的
     * 速度衰减输入。
     */
    int slowdown = app->settings.window.slide_no_decay ?
        0 : app->settings.window.slide_slowdown_percent;
    if (slowdown < 0) slowdown = 0;
    else if (slowdown > BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT)
        slowdown = BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT;
    /* 放缓值 0 = 恒速: 撞边完整反射、并且没有拍数兜底, 否则"恒速"撑不过一次撞墙或
       0.8 秒。打开"一直滑动"要的就是那个永不停止的效果。 */
    bool no_decay = slowdown == 0;
    double mult = (100.0 - slowdown) / 100.0;
    /* 60fps 下每帧约合 1 拍, 高刷下有的帧不推进, 掉帧时补多拍。 */
    app->slide_accum_s += dt;
    int ticks = (int)(app->slide_accum_s / SLIDE_NOMINAL_FRAME_S);
    if (ticks <= 0) return;
    app->slide_accum_s -= (double)ticks * SLIDE_NOMINAL_FRAME_S;
    while (ticks-- > 0) {
        if (!slide_step(app, mult,
                no_decay ? SLIDE_BOUNCE_NO_LOSS : SLIDE_BOUNCE_RESTITUTION,
                no_decay ? SLIDE_FRAME_CAP_NONE : SLIDE_MAX_FRAME)) {
            bongo_cat_window_stop_inertia(app);
            if (app->settings.window.keep_in_screen)
                bongo_cat_window_clamp_to_display(app);
            return;
        }
    }
}
