/*
 * 帧节奏与事件等待超时。两个概念要分清:
 *   - frame_interval_ns(): 模型动画的推进间隔, 由设置里的"最大帧率"决定
 *     (无限 -> 0, 表示每轮循环都该出新帧);
 *   - bongo_cat_window_wait_timeout(): 这次 SDL_WaitEventTimeout 最多睡多久 —— 取所有
 *     "必须在那之前醒来"的 deadline 的最小值 (帧预算、滚轮/悬停动画、命中检测、设置
 *     窗口、快照、点击穿透), 250ms 兜底。
 * 这里的任何改动都直接影响 CPU 占用与手感: 睡短了空转, 睡长了掉帧/卡交互。
 * 没编译 Cubism 时 (诊断后端) 没有真实动画, 固定 10 FPS 推进, 好让冒烟结果可复现。
 */
#include "runtime.h"
#include "window_wheel_internal.h"
#include "bongo_cat/config.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/vk_backend.h"
#include <stdlib.h>

static uint64_t frame_interval_ns(const BongoCatApp *app) {
#ifdef BONGO_CAT_HAS_CUBISM
    int fps = app ? app->settings.model.max_fps : BONGO_CAT_DEFAULT_MAX_FPS;
    /* 无限制: 间隔为 0 -> 每轮循环都判定为「该出新帧」, 不再做节流 */
    if (fps == BONGO_CAT_MAX_FPS_UNLIMITED) return 0;
    if (fps <= 0) fps = BONGO_CAT_DEFAULT_MAX_FPS;
    return 1000000000ull / (uint64_t)fps;
#else
    (void)app;
    /* 诊断后端没有真实模型动画, 保持固定的 10 FPS 推进节奏 */
    return 100000000ull;
#endif
}

static int remaining_ms(uint64_t deadline, uint64_t now) {
    if (deadline <= now) return 0;
    uint64_t ns = deadline - now;
    uint64_t ms = ns / 1000000ull + (ns % 1000000ull != 0);
    return ms > 250 ? 250 : (int)ms;
}

bool bongo_cat_model_frame_due(const BongoCatApp *app, uint64_t now) {
    return app && app->session.window.visible && !app->window_minimized &&
        (!app->last_frame_ns || now >= app->last_frame_ns +
            frame_interval_ns(app));
}

/*
 * 把"最大帧率"和"垂直同步"两个设置下发给后端的呈现路径。
 *
 * 语义 (界面上"垂直同步"那行的提示文案必须和这里一致):
 *   - 垂直同步开 -> VK 走 FIFO / GL 走 swap interval 1, 呈现被锁在显示器刷新率上;
 *   - 垂直同步关 -> VK 走 IMMEDIATE / GL 走 swap interval 0, 由"最大帧率"限帧;
 *   - 两种情况下"最大帧率"都仍然生效: 它管的是 model 动画的推进间隔(frame_interval_ns),
 *     是帧率的天花板, 而垂直同步决定"送出去的帧要不要等垂直回扫"。所以 30 FPS + 开同步
 *     依然是 30 FPS(不会因为屏幕是 120Hz 就变快), 只是每帧不再撕裂;
 *     "无限" + 开同步 = 按刷新率跑, "无限" + 关同步 = 能跑多快跑多快。
 *
 * 为什么要按"开关是否被用户显式设置过"分两种来源: 老版本里同步不是一个设置, 而是从
 * 最大帧率推出来的(限帧 -> 开, 无限 -> 关); 配置里没有 vsync 键时 struct 里留着
 * sentinel, 读取阶段由 bongo_cat_settings_validate() 按那条老规则收口, 于是老用户
 * 升级后行为不变。见 config.h 的三态注释。
 *
 * 帧率上限本身由 frame_interval_ns() 决定, 这个函数只负责呈现侧的同步策略。
 * 设置变化时 Vulkan 会重建交换链 (呈现模式是创建时定下的), 所以只在值变化时下发。
 */
void bongo_cat_app_apply_frame_pacing(BongoCatApp *app) {
    if (!app) return;
    int fps = app->settings.model.max_fps;
    /* 结构体里可能还留着 sentinel(例如本次运行没走过配置读取), 这里按同一条规则兜底 */
    int setting = app->settings.model.vsync;
    bool vsync = setting == BONGO_CAT_VSYNC_DEFAULT ?
        BONGO_CAT_VSYNC_FOR_MAX_FPS(fps) : setting != 0;
    bool first = app->applied_max_fps < 0;
    if (!first && fps == app->applied_max_fps && vsync == app->applied_vsync)
        return;
    /*
     * 结论行每次"应用设置"都打。为什么要单独做这件事: VK 路径上真正换模式的
     * bongo_cat_vk_set_vsync() 只在值变化时动作, 而新建的交换链本来就是 FIFO ——
     * "开关开着"这一半的配置**不会**引起任何变化, 于是那半边一行日志都没有, 用户
     * 按文档去日志里找证据会一无所获。
     *
     * 唯一的例外是本次运行第一次应用: 默认(或老配置)的 vsync 是 sentinel、按"限帧就
     * 开同步"解出来, 那时用户并没有做过选择, 打一行"垂直同步已开启"只会让人以为
     * 自己开过它。所以第一次应用且来源是 sentinel 时不打。
     */
    bool silent = first && setting == BONGO_CAT_VSYNC_DEFAULT;
    app->applied_max_fps = fps;
    app->applied_vsync = vsync;
    if (app->vk) {
        bongo_cat_vk_set_vsync(app->vk, vsync);
        /*
         * 只报"设置层面的结论" (开了还是关了、上限多少), 不报 present_mode:
         * BongoCatVk 在这里是不完整类型, 摸内部字段就得把 vk_internal.h 拉进 runtime。
         * 呈现模式那一半由重建路径自己记 —— 开关翻动时 vk_swapchain.c 会打一行
         * "[vulkan] 交换链 … present_mode=N" (2=FIFO, 0=IMMEDIATE, 1=MAILBOX),
         * 两行合起来才是完整证据链。
         */
        if (!silent)
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] 垂直同步已%s (最大帧率=%d)", vsync ? "开启" : "关闭",
                fps);
        return;
    }
    if (!app->window || !app->gl_context) return;
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Frame pacing could not activate the main GL context: %s",
            SDL_GetError());
        return;
    }
    /* GL 没有"交换链"这个概念, 呈现侧的同步状态就是 swap interval 本身, 所以这里
       连它一起报, 用户不必再猜这个数字。 */
    if (!SDL_GL_SetSwapInterval(vsync ? 1 : 0))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Frame pacing could not set vertical sync (%d): %s", vsync ? 1 : 0,
            SDL_GetError());
    if (!silent)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 垂直同步已%s (最大帧率=%d swap_interval=%d)",
            vsync ? "开启" : "关闭", fps, vsync ? 1 : 0);
}

/* 只算"睡多久", 不改任何状态: 每个分支都是"取更小的等待", 所以新增唤醒源时注意别
   写成覆盖 (那样会把帧预算吃掉)。 */
int bongo_cat_window_wait_timeout(const BongoCatApp *app, uint64_t now) {
    if (!app) return 250;
    uint64_t frame_deadline = app->last_frame_ns + frame_interval_ns(app);
    int wait_ms = app->session.window.visible && !app->window_minimized ? remaining_ms(
        frame_deadline, now) : 250;
    if (bongo_cat_preferences_needs_frame(app->preferences) && wait_ms > 4)
        wait_ms = 4;
    if (app->wheel_animation_active) {
        int wheel_wait = remaining_ms(app->wheel_animation_ns +
            BONGO_CAT_WHEEL_FRAME_INTERVAL_NS, now);
        if (wait_ms > wheel_wait) wait_ms = wheel_wait;
    }
    if (app->hover_fade_active && wait_ms > 8) wait_ms = 8;
    if (app->window_snapshot && wait_ms > 16) wait_ms = 16;
    if (app->session.window.visible && !app->window_minimized &&
        (app->click_through_applied || (app->settings.window.pass_through &&
            app->settings.window.always_on_top && app->settings.window.hide_on_hover)) &&
        wait_ms > 16) wait_ms = 16;
    bool pending_hit = app->session.window.visible && !app->window_minimized &&
        app->pointer_hit_dirty &&
        app->pointer_hit_deadline_ns && !app->settings.window.pass_through &&
        !app->hover_hidden && !app->left_mouse_down && !app->right_mouse_down;
    if (!pending_hit) return wait_ms;
    int hit_wait = remaining_ms(app->pointer_hit_deadline_ns, now);
    return hit_wait < wait_ms ? hit_wait : wait_ms;
}

/* SDL_WaitEventTimeout 在部分平台上会提前返回 (窗口消息等), 这里循环等到真正的
   deadline; 返回 false 表示超时, 调用方据此走"没有事件也要更新"的路径。 */
bool bongo_cat_wait_event(SDL_Event *event, int timeout_ms) {
    if (!event) return false;
    if (timeout_ms <= 0) return SDL_PollEvent(event);
    uint64_t deadline = SDL_GetTicksNS() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        int remaining = remaining_ms(deadline, SDL_GetTicksNS());
        if (!remaining) return false;
        if (SDL_WaitEventTimeout(event, remaining)) return true;
        uint64_t now = SDL_GetTicksNS();
        if (now >= deadline) return false;
        SDL_Delay(1);
    }
}

/* --ci-menu 用的自测: 用 calloc 造一个假 app 直接验算各条 deadline, 不需要真实窗口。
   改动上面的等待策略时要同步改这里, 否则 CI 会先挂。 */
bool bongo_cat_window_wait_timeout_self_test(void) {
    const uint64_t now = 1000000000ull;
    BongoCatApp *app = calloc(1, sizeof(*app));
    if (!app) return false;
    bool passed = false;
    app->session.window.visible = true;
    app->settings.model.max_fps = 60;
    app->last_frame_ns = now;
#ifdef BONGO_CAT_HAS_CUBISM
    app->settings.model.max_fps = 30;
    uint64_t interval_30 = frame_interval_ns(app);
    app->settings.model.max_fps = 60;
    uint64_t interval_60 = frame_interval_ns(app);
    app->settings.model.max_fps = 120;
    uint64_t interval_120 = frame_interval_ns(app);
    if (!(interval_30 > interval_60 && interval_60 > interval_120) ||
        bongo_cat_model_frame_due(app, now + interval_120 - 1) ||
        !bongo_cat_model_frame_due(app, now + interval_120)) goto done;
    app->settings.model.max_fps = BONGO_CAT_MAX_FPS_UNLIMITED;
    if (frame_interval_ns(app) != 0 ||
        !bongo_cat_model_frame_due(app, now) ||
        bongo_cat_window_wait_timeout(app, now) != 0) goto done;
    app->settings.model.max_fps = 60;
#endif
    int frame_wait = remaining_ms(now + frame_interval_ns(app), now);
    if (bongo_cat_window_wait_timeout(app, now) != frame_wait ||
        bongo_cat_model_frame_due(app, now)) goto done;
    app->pointer_hit_dirty = true;
    app->pointer_hit_deadline_ns = now + 8000000ull;
    if (bongo_cat_window_wait_timeout(app, now) != 8) goto done;
    app->pointer_hit_deadline_ns = now;
    if (bongo_cat_window_wait_timeout(app, now) != 0) goto done;
    app->pointer_hit_dirty = false;
#ifdef BONGO_CAT_HAS_CUBISM
    app->settings.model.max_fps = 1;
#endif
    app->click_through_applied = true;
    if (bongo_cat_window_wait_timeout(app, now) != 16) goto done;
    app->click_through_applied = false;
    app->settings.window.pass_through = true;
    app->settings.window.always_on_top = true;
    app->settings.window.hide_on_hover = true;
    if (bongo_cat_window_wait_timeout(app, now) != 16) goto done;
    app->settings.window.always_on_top = false;
    if (bongo_cat_window_wait_timeout(app, now) !=
        remaining_ms(now + frame_interval_ns(app), now)) goto done;
    app->settings.window.pass_through = false;
    app->settings.window.hide_on_hover = false;
#ifdef BONGO_CAT_HAS_CUBISM
    app->settings.model.max_fps = 60;
#endif
    if (!bongo_cat_model_frame_due(app,
        now + frame_interval_ns(app))) goto done;
    app->window_minimized = true;
    if (bongo_cat_model_frame_due(app, now + frame_interval_ns(app)) ||
        bongo_cat_window_wait_timeout(app, now) != 250) goto done;
    app->window_minimized = false;
    app->session.window.visible = false;
    if (bongo_cat_window_wait_timeout(app, now) != 250) goto done;
    app->wheel_animation_active = true;
    app->wheel_animation_ns = now;
    if (bongo_cat_window_wait_timeout(app, now) != 8 ||
        bongo_cat_window_wait_timeout(app, now + 3000000ull) != 5 ||
        bongo_cat_window_wait_timeout(app, now + 8000000ull) != 0) goto done;
    passed = true;
done:
    free(app);
    return passed;
}
