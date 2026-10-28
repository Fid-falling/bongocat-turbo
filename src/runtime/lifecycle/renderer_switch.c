/*
 * "切换渲染后端"的唯一实现 —— 设置页的分段选择器与托盘菜单都必须走这里。
 *
 * 为什么必须只有一份: 这两个入口以前各写各的, 于是同一件事有两种脾气 ——
 *   - 托盘按"当前实际在用的后端"取反面, 被拒时留日志 + 弹窗;
 *   - 设置页只比"配置值有没有变", 而且后端不可用时那一段被画成禁用态, 点击直接被
 *     控件吞掉, 连一条日志都没有。
 * 用户看到的都是"点了没反应", 但日志里一个字的线索都没有, 只能靠猜。
 *
 * 顺序是本文件的核心不变量, 不能调换:
 *   校验可用性 -> 写内存 -> **立刻落盘** -> 记日志 -> 自动重启/提示
 * 落盘排在"尝试"那条日志**之后**、启动重启**之前**: config_store 的延迟写盘跑在主循环
 * 里, 而真正的重启动作 (bongo_cat_app_update_restart) 也发生在主循环末尾并会先 flush。
 * 一旦确认框被移除, 主循环就不会被模态框停住, 但先落盘永远是对的 —— 即便无人值守下
 * 不能重启, 用户也已经拿到了他要的后端偏好。
 *
 * 重启的确认框已经不含了: 切换渲染后端必然要重启才生效, 用户点分段控件即代表同意立即
 * 重启, 不再弹框确认。切换是用户主动操作, 反复切换也能反复重启; 无人值守的约束仍由
 * request_restart 内部处理。
 *
 * 不写入不可用的后端: 把它写进配置只会让用户重启后得到一次回退日志和一个没变化的
 * 界面, 不如当场说清楚 (日志 + 用户可见提示)。
 */
#include "runtime.h"
#include "preferences_notice.h"
#include "renderer_fallback_state.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"
#include "bongo_cat/tray.h"

#include <SDL3/SDL.h>

/* 日志里要能看出用户点的是哪个入口: 两条路径行为一致, 但排查时第一步就是分辨入口。 */
static const char *entry_name(BongoCatRendererEntry entry) {
    return entry == BONGO_CAT_RENDERER_ENTRY_TRAY ? "托盘" : "设置";
}

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/*
 * 目标后端不可用时的统一拒绝路径。三件事一件都不能少:
 *   1) 日志 (WARN): 用户点了没反应时, 日志里必须有原因 —— 这是托盘那条路径原本就有的
 *      "被拒也要留痕", 现在设置页共用同一份;
 *   2) 设置窗口开着: 窗口里的浮动提示, 用户当场就能看到为什么没反应;
 *   3) 设置窗口关着 (典型场景是从托盘点的): 模态框, 否则用户点了托盘什么都没有, 只能去
 *      翻日志。无人值守运行 (--ci-*) 一律不弹模态框, 免得把脚本挂死。
 * 任何写入都不做: 见文件头最后一段。
 */
static void renderer_rejected(BongoCatApp *app, BongoCatRenderer target,
    BongoCatRendererEntry entry) {
    const char *reason = bongo_cat_renderer_unavailable_reason(target);
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[render] %s切换被拒: %s 在这台机器上不可用 (%s), 配置未改动",
        entry_name(entry), bongo_cat_renderer_name(target),
        reason && reason[0] ? reason : "未知原因");
    const char *message = tr(app,
        "composables.useAppMenu.labels.rendererUnavailable",
        "Vulkan is not available on this machine.");
    bongo_cat_preferences_notice_show(app, message, true);
    if (app->smoke || app->automated || SDL_getenv("BONGO_CAT_TEST_NO_DIALOG"))
        return;
    /* 窗口开着时上面那条浮动提示已经够了, 再弹一个模态框只是烦人。 */
    if (app->preferences && bongo_cat_preferences_visible(app->preferences))
        return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, BONGO_CAT_NAME, message,
        app->window);
}

bool bongo_cat_app_switch_renderer(BongoCatApp *app, BongoCatRenderer target,
    BongoCatRendererEntry entry) {
    if (!app) return false;
    if (target != BONGO_CAT_RENDERER_AUTO && target != BONGO_CAT_RENDERER_VULKAN &&
        target != BONGO_CAT_RENDERER_OPENGL)
        return false;
    /*
     * Auto 也是合法选择, 而且要原样写进配置: 不能在切换的这一刻把它解析成某个具体
     * 后端 —— 那会把用户的"自动"悄悄变成"钉死一个后端"。它由下次启动自己解析。
     */
    if (target != BONGO_CAT_RENDERER_AUTO &&
        !bongo_cat_renderer_available(target)) {
        renderer_rejected(app, target, entry);
        return false;
    }
    /*
     * 用户手动把后端设为 Vulkan = 明确要求"再试一次": 清空自动回退计数与暂停标志。
     * 必须排在可用性检查之后 —— 被拒的切换不能把证据也一起清掉。
     * 只认 Vulkan: 选 AUTO 不等于"再试一次"(AUTO 的自动尝试恰恰是被暂停标志拦下的那
     * 一条), 要重试就得明确点 Vulkan/提示条上的按钮。清空要发生在落盘与重启之前,
     * 新进程才不会又读到"已暂停"。
     */
    if (target == BONGO_CAT_RENDERER_VULKAN)
        bongo_cat_renderer_fallback_note_manual_retry(app, entry_name(entry));
    const BongoCatRenderer previous = app->settings.model.renderer;
    const BongoCatRenderer active = bongo_cat_renderer_active();
    app->settings.model.renderer = target;
    /* 托盘那一项的文案要马上跟着新值走 (设置页的分段控件每帧读 app->settings, 不用管)。 */
    if (app->tray) bongo_cat_tray_sync(app->tray);
    bongo_cat_preferences_invalidate(app->preferences);
    /*
     * 先落盘: config_store 的延迟写盘在主循环里, 而重启动作发生在主循环末尾并会先
     * flush。落盘失败会由 config_store 自己重试并留日志, 这里不需要额外处理。
     */
    bongo_cat_config_store_flush(app);
    /*
     * 配置值和实际在用的后端可能不同 (启动时 Vulkan 初始化失败回退到 OpenGL 之后就是
     * 这样): 这时"配置没变"也要说清楚, 否则重新点一次当前后端会被误读成"没反应"。
     */
    if (previous == target)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[render] %s切换后端: 配置仍是 %s, 但当前实际在用 %s, 需要重启重试",
            entry_name(entry), bongo_cat_renderer_name(target),
            bongo_cat_renderer_name(active));
    else
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[render] %s切换后端: 配置 %s -> %s (当前实际在用 %s), 立即重启生效",
            entry_name(entry), bongo_cat_renderer_name(previous),
            bongo_cat_renderer_name(target), bongo_cat_renderer_name(active));
    /*
     * 直接自动重启, 不再弹确认框: 用户点分段控件即代表同意立即重启。切换是用户主动
     * 操作, 反复切换每次都能重启。只有无人值守运行 (--ci-* / 冒烟) 会被 request_restart
     * 拦下, 那时它返回 false 并让调用方只给提示。
     */
    const bool restarting = bongo_cat_app_request_restart(app);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[render] 渲染后端偏好改为 %s, %s",
        bongo_cat_renderer_name(target),
        restarting ? "将自动重启" : "已保存, 稍后手动重启生效");
    /* 重启已经安排好了就不该再提示: 用户看到提示的时候进程正在退出。 */
    if (restarting) return true;
    /*
     * 无人值守运行里不能自动重启: 只给提示。
     */
    bongo_cat_preferences_notice_show(app, tr(app,
        "composables.useAppMenu.labels.rendererRestart",
        "Restart BongoCat to apply the new render backend."), false);
    return true;
}