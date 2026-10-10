/*
 * 悬停隐藏的执行侧: 指针位置 -> "是否压在模型可见像素上" -> 窗口不透明度与穿透状态。
 * 主循环每轮固定调用 update_hover + update_hover_fade (顺序不能反), 模态/预览循环也各调
 * 一次, 所以这里不能依赖"一帧只调一次", 内部按 8ms 自己节流。
 * 对外副作用只有三个: 改窗口不透明度、结束窗口贴图快照 (hover_opacity 里)、刷新穿透状态。
 * 命中判定用的坐标是窗口内逻辑坐标, 由平台层换算, 与 pointer_x/y 的屏幕坐标不能混用。
 * 最容易被改错的地方: 不透明度写入必须走 hover_opacity —— 失败时它会把能力标记为不可用并
 * 做一次兜底恢复, 绕过这一步会让宠物永久半透明或者反复失败刷日志。
 */
#include "runtime.h"
#include "hover_fade.h"

#include <SDL3/SDL.h>

/* Window-facing pointer state: hover hiding and hit-test scheduling. The
   sampling pipeline, display bounds and model output have their own modules. */
/* 一次指针更新: 记原始屏幕坐标, 排一次命中重算, 再立刻推进悬停状态机 (不要等下一帧,
   否则快速划过模型时隐藏会慢一拍)。 */
void bongo_cat_app_track_hover(BongoCatApp *app, double x, double y) {
    app->pointer_known = true;
    app->pointer_x = x;
    app->pointer_y = y;
    bongo_cat_window_schedule_pointer_hit(app);
    bongo_cat_app_update_hover(app, SDL_GetTicksNS());
}

/* 写窗口不透明度的唯一出口。返回 false 表示平台不支持或被拒: 置 hover_hide_unavailable
   永久停掉这条特性, 并尽量把不透明度恢复成用户设定值 (会话里的 opacity_percent), 再同步
   一次穿透。先结束窗口贴图快照: 快照会顶替实时渲染, 不先撤掉就会看到旧画面。 */
static bool hover_opacity(BongoCatApp *app, float opacity) {
    bongo_cat_window_snapshot_end(app);
    if (bongo_cat_platform_set_opacity(&app->platform, opacity)) return true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Hover hiding disabled after opacity update failed: %s", SDL_GetError());
    app->platform.hover_hide_unavailable = true;
    app->hover_fade_active = false;
    app->hover_hidden = app->hover_inside = false;
    /* 兜底恢复只是 best effort: 平台连恢复都失败时保持现状, 不再递归重试。 */
    /* Best effort: retain the successful cached opacity if restoration fails. */
    bongo_cat_platform_set_opacity(&app->platform,
        app->session.window.opacity_percent / 100.0f);
    bongo_cat_window_sync_click_through(app);
    return false;
}

/* Direction changes retrace the same curve, without restarting its easing.
   Integrating elapsed time also makes duration edits continuous. */
/* 推进淡入淡出。相位是"过渡进行到几分之几", 不透明度 = 本次淡入淡出的峰值 × 曲线(相位),
   所以用户设定的不透明度在过渡中途变化也能平滑收敛到新目标。
   时间由调用方传入 (模态循环与主循环的时间源不同), 不在这里读时钟。 */
void bongo_cat_app_update_hover_fade(BongoCatApp *app, uint64_t now) {
    if (!app || !app->hover_fade_active) return;
    float duration = app->settings.window.hide_fade_seconds;
    /* 8ms 节流: 指针高频移动会以远高于帧率的频率唤醒这里, 不节流就会把不透明度写爆 ——
       每次写入在平台上都是一次真实调用。 */
    if (duration > 0.0f && now < app->hover_fade_next_ns) return;
    /* 用真实流逝时间而不是固定步长积分: 主循环卡顿或掉帧时动画进度不会走慢。 */
    float seconds = now > app->hover_fade_tick_ns ?
        (float)((double)(now - app->hover_fade_tick_ns) / 1000000000.0) : 0.0f;
    app->hover_fade_tick_ns = now;
    app->hover_fade_next_ns = now + 8000000ull;
    bool decreasing = app->hover_fade_target < app->hover_fade_phase;
    float next = bongo_cat_hover_fade_step(app->hover_fade_phase,
        decreasing, seconds, duration);
    /* 方向中途反转时按同一个目标钳制, 相位不会越过目标再弹回来。 */
    app->hover_fade_phase = decreasing ? SDL_max(next, app->hover_fade_target) :
        SDL_min(next, app->hover_fade_target);
    float phase = app->hover_fade_phase;
    /* 只有走到目标才用用户设定值 (此时必须精确等于 opacity_percent), 途中一律走曲线;
       这个三元表达式是"终值不经过曲线"的唯一实现处。 */
    float opacity = phase == app->hover_fade_target ?
        (app->hover_hidden ? 0.0f : app->session.window.opacity_percent / 100.0f) :
        app->hover_fade_opacity * bongo_cat_hover_fade_curve(phase);
    if (hover_opacity(app, opacity))
        app->hover_fade_active = phase != app->hover_fade_target;
}

/* 取消动画但不改当前不透明度 (隐藏/显示窗口、切模型、开菜单时调用): 之后一次 hover 状态
   变化会用平台当前值重新定位相位, 所以这里只清"正在动画"的标志与截止时间。 */
void bongo_cat_app_cancel_hover_fade(BongoCatApp *app) {
    if (app) {
        app->hover_fade_active = false;
        app->hover_fade_next_ns = 0;
    }
}

/* 开始一次隐藏/显示过渡。now 由调用方给, 与 update_hover_fade 用同一个时间源。
   相位由"平台当前不透明度"反解而来 —— 必须用平台值而不是缓存值, 否则和外部改过的
   不透明度 (设置界面/托盘) 会不同步, 起手就跳一下。 */
static void hover_start_fade(BongoCatApp *app, bool hidden, uint64_t now) {
    float target = hidden ? 0.0f :
        app->session.window.opacity_percent / 100.0f;
    app->hover_hidden = hidden;
    /* 关闭动画时直接落到目标值, 走同一条写入路径, 保证失败处理一致。 */
    if (app->settings.window.hide_fade_seconds <= 0.0f) {
        app->hover_fade_active = false;
        hover_opacity(app, target);
        return;
    }
    /* 峰值取 max(当前, 用户设定值): 从半透明状态开始淡出时, 曲线仍能到达用户设定值;
       因为相位是按"当前值 / 峰值"反解的, 峰值变化不会造成画面跳变。 */
    float current = bongo_cat_platform_get_opacity(&app->platform);
    app->hover_fade_opacity = SDL_max(current,
        app->session.window.opacity_percent / 100.0f);
    app->hover_fade_phase = bongo_cat_hover_fade_phase(
        app->hover_fade_opacity > 0.0f ? current / app->hover_fade_opacity : 0.0f);
    app->hover_fade_target = bongo_cat_hover_fade_phase(
        app->hover_fade_opacity > 0.0f ? target / app->hover_fade_opacity : 0.0f);
    app->hover_fade_tick_ns = now;
    app->hover_fade_next_ns = now + 8000000ull;
    app->hover_fade_active = current != target;
}

/* 悬停状态机: 判断指针是否压在模型可见像素上, 状态翻转时启动过渡。now 由调用方传入
   (主循环用 SDL_GetTicksNS, 测试用可注入的时间)。
   前置条件: 窗口已存在; 关闭悬停隐藏、窗口不可见/最小化、启动首帧未呈现时都会直接把
   inside 判为假 —— 这也是"启动时不要先隐藏再显示"的实现方式。 */
void bongo_cat_app_update_hover(BongoCatApp *app, uint64_t now) {
    if (!app || !app->window) return;
    /* 全部条件都必须在位才采样: 悬停隐藏只在"窗口始终置顶 + 点击穿透"时才有意义,
       否则隐藏会让窗口既看不见又点不到。 */
    bool enabled = !app->platform.hover_hide_unavailable &&
        app->settings.window.hide_on_hover &&
        app->settings.window.pass_through && app->settings.window.always_on_top &&
        app->session.window.visible && !app->window_minimized &&
        !app->startup_visibility_pending;
    float local_x = 0.0f, local_y = 0.0f;
    /* Window opacity does not change the frame alpha, so hidden pets can
       still detect the pointer leaving their visible model pixels. */
    /* 这里用的是窗口内逻辑坐标 (平台层给), 不是 pointer_x/y 的屏幕坐标。 */
    bool inside = enabled && app->pointer_known &&
        bongo_cat_platform_pointer_local(&app->platform,
            app->pointer_x, app->pointer_y, &local_x, &local_y);
    /* 限流 GPU 回读: 这个检测在 Vulkan 下要读整帧缓存下来的 alpha, 高频指针事件下没有
       上限会明显掉帧。到期前直接沿用上一次的结论 (hover_inside 不变)。 */
    if (inside) {
        /* Bound GPU readbacks on backends without a CPU alpha cache, even
           when high-rate input wakes the event loop faster than rendering. */
        if (now < app->hover_deadline_ns) return;
        app->hover_deadline_ns = now + 8000000ull;
        inside = bongo_cat_window_visible_at_pointer(app, local_x, local_y);
    /* 关掉特性时清掉截止时间, 下次开启立刻重新采样而不是等一个节流周期。 */
    } else if (!enabled) app->hover_deadline_ns = 0;

    app->hover_inside = inside;
    /* 状态没变就什么都不做: 否则每帧都会重启过渡, 淡入淡出永远走不完。 */
    if (app->hover_hidden == inside) return;
    hover_start_fade(app, inside, now);
    bongo_cat_window_sync_click_through(app);
}
