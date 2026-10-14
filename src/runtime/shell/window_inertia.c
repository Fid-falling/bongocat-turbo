/*
 * 松手后的惯性滑行 (window_display.c 只留窗口几何与夹取; 这里只动窗口位置)。
 *
 * 为什么单独一个文件: window_display.c 贴着 500 行的策略上限 (见 cmake/CheckLines.cmake),
 * 而滑行是一个自成一体的逐帧状态机 (初速在 window_drag.c 松手时算好, 这里只负责推进),
 * 与"窗口该在哪块显示器上"没有共享状态。
 *
 * 两种模式, 同一套边界处理 (slide_bounce) 与同一条推进路径:
 *   - 普通惯性: 每拍按 window.slide_slowdown_percent 衰减, 撞边只保留 5% 速度, 于是会
 *     自己停下; 位移停滞/超帧数/窗口不可见都是停止条件。
 *   - 关闭动量衰减 (window.slide_no_decay): 把本帧生效的放缓值压成 0 (即恒速, 复用同一
 *     套百分比机制, 不引入第二套速度状态) + 撞边完整反射 (速度不损失), 所以永不自行
 *     停止 —— 屏保那种在屏幕上撞来撞去的效果。它的停止条件只有显式的那几个: 再次抓住
 *     宠物/拖动、关掉"关闭动量衰减"、关掉"拖动惯性"、窗口隐藏或最小化、退出, 全部经由
 *     bongo_cat_window_stop_inertia 收口。
 *
 * 易改错: 判停用的是"整数像素有没有变"而不是速度阈值 (见 update_inertia 的注释),
 * 所以"永不停止"的实现前提是速度真的能推动像素; 窗口比显示器还大那种退化情形由
 * slide_bounce 里的 max < min 分支清零速度收尾。
 */
#include "runtime.h"

#include <math.h>

/* 滑行: 每个渲染帧算作一个 15.6ms 标称拍, 位移与衰减都按这一拍推进。 */
#define SLIDE_NOMINAL_FRAME_S 0.0156
#define SLIDE_MAX_FRAME 50
/* 撞边反弹: 法向速度反向, 只保留 5% 速度。 */
#define SLIDE_BOUNCE_RESTITUTION 0.05
/* "关闭动量衰减"时用完整反射 (1.0 = 一点速度都不损失), 这是"永不自行停止"的前提。 */
#define SLIDE_BOUNCE_NO_LOSS 1.0

/* 滑行遇到屏幕边缘: 贴到目标显示器可用区边界, 法向速度反向并保留 restitution 比例的速度
   (镜面反射), 让桌宠像球撞墙一样弹回屏幕内, 而不是一路飞出桌面。
   restitution 由调用方给: 普通惯性滑行用 SLIDE_BOUNCE_RESTITUTION (撞一次就软下来),
   "关闭动量衰减"时给 1.0 —— 完整反射才可能永不停止 (屏保效果)。 */
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
     * 让它每帧在两条边之间来回跳 —— 位置来回变但永远不满足"整数像素没变就停", 于是
     * 关闭动量衰减时就成了停不下来的抖动。这种情形贴住左上角并把该轴速度清零, 交给判停
     * 收尾; 正常尺寸的窗口走下面原来的镜面反射。
     */
    if (max_x < min_x) { *x = min_x; *vx = 0.0; }
    else if (*x < min_x) { *x = min_x; *vx = fabs(*vx) * restitution; }
    else if (*x > max_x) { *x = max_x; *vx = -fabs(*vx) * restitution; }
    if (max_y < min_y) { *y = min_y; *vy = 0.0; }
    else if (*y < min_y) { *y = min_y; *vy = fabs(*vy) * restitution; }
    else if (*y > max_y) { *y = max_y; *vy = -fabs(*vy) * restitution; }
}

/* 立刻结束松手滑行。抓住宠物、关掉开关、窗口隐藏/最小化、退出都会走这里 —— 幂等,
   没在滑行时调用也安全。"关闭动量衰减"的滑行不会自己停下, 所以这些停止条件是必需的。 */
void bongo_cat_window_stop_inertia(BongoCatApp *app) {
    if (!app) return;
    app->slide_active = false;
    app->slide_velocity_x = 0.0;
    app->slide_velocity_y = 0.0;
    app->slide_frame = 0;
}

/* 主循环每帧调用: 拖着 window 沿惯性方向移动。初速在松手时由 window_drag.c 算好。
   每个渲染帧按一个 15.6ms 标称拍推进 (x += v*0.0156); 普通模式每拍乘一次衰减, 撞到
   屏幕边缘会弹回, 位移停滞 (整数像素不再前进)/超帧数/窗口不可见就停下并夹回显示器。
   打开"关闭动量衰减"后放缓值为 0、反弹不损失速度, 于是永不自行停止 —— 停止只能来自抓住
   宠物、关掉这一个或上一个开关、窗口隐藏/最小化、程序退出 (见上面的 stop_inertia)。 */
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
     * 速度衰减只有这一套机制: window.slide_slowdown_percent (每拍乘 (100-值)/100, 0 =
     * 不衰减即恒速)。"关闭动量衰减"不另起一套速度状态, 它只是把**本帧生效的**放缓值
     * 压成 0 —— 用户自己拖的那个百分比仍留在配置里, 关掉这一项立刻恢复原来的衰减。
     * 于是两种模式共享同一条推进/判停路径, 只有"撞边丢不丢速度"与"有没有超时兜底"不同。
     * 注意这里的"不衰减"在任何默认值下都成立: 放缓百分比的默认值怎么调都不影响它
     * (0 就是恒速, 这条语义是这一个机制的固有含义)。
     */
    bool no_decay = app->settings.window.slide_no_decay;
    int slowdown = no_decay ? 0 : app->settings.window.slide_slowdown_percent;
    if (slowdown < 0) slowdown = 0;
    else if (slowdown > BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT)
        slowdown = BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT;
    /* 每帧顶一个 15.6ms 标称拍: 位移固定按 v*0.0156 推进 (不是 v*真实dt), 衰减也是每拍
       只乘一次 —— 这样"整数像素判停"的阈值与刷新率无关; 代价是刷新率越高滑行越快。 */
    double decay = (100.0 - slowdown) / 100.0;
    int prev_x = (int)lround(app->slide_x);
    int prev_y = (int)lround(app->slide_y);
    app->slide_x += app->slide_velocity_x * SLIDE_NOMINAL_FRAME_S;
    app->slide_y += app->slide_velocity_y * SLIDE_NOMINAL_FRAME_S;
    int width = 0, height = 0, x = 0, y = 0;
    if (!SDL_GetWindowSize(app->window, &width, &height) ||
        width <= 0 || height <= 0) {
        bongo_cat_window_stop_inertia(app);
        return;
    }
    x = (int)lround(app->slide_x);
    y = (int)lround(app->slide_y);
    /* 判停比较"位移": 整数像素没变就停, 不用速度阈值。亚像素阶段整数位置
       每几帧才跳 1px, 速度门槛会在滑行还没走完时把它提前掐断。这条对"关闭动量衰减"
       同样成立, 而且是那种模式唯一的自然出口 (速度小到推不动一个像素就没必要空转)。 */
    if (x == prev_x && y == prev_y) {
        bongo_cat_window_stop_inertia(app);
        if (app->settings.window.keep_in_screen) bongo_cat_window_clamp_to_display(app);
        return;
    }
    slide_bounce(app, &x, &y, width, height,
        &app->slide_velocity_x, &app->slide_velocity_y,
        no_decay ? SLIDE_BOUNCE_NO_LOSS : SLIDE_BOUNCE_RESTITUTION);
    app->slide_x = (double)x;
    app->slide_y = (double)y;
    /* 反弹之后才按本拍衰减: 先移动 -> 撞边反弹 -> 再衰减。关闭动量衰减时 decay 是 1.0
       (放缓值为 0), 速度一点不损失, 所以它会一直撞下去。 */
    app->slide_velocity_x *= decay;
    app->slide_velocity_y *= decay;
    if (!SDL_SetWindowPosition(app->window, x, y)) {
        bongo_cat_window_stop_inertia(app);
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
    /* 超时兜底 (SLIDE_MAX_FRAME 拍) 只对"会自己衰减停下"的普通滑行有效: 关闭动量衰减
       时本来就是永不停止, 这里不能掐断 —— 那时只有抓住宠物/关开关/隐藏窗口/退出才停。 */
    if (!no_decay && app->slide_frame >= SLIDE_MAX_FRAME) {
        bongo_cat_window_stop_inertia(app);
        if (app->settings.window.keep_in_screen) bongo_cat_window_clamp_to_display(app);
    }
}
