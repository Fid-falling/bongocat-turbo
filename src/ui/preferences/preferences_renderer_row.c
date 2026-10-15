/*
 * 设置页 [模型] 区里的"渲染后端"分段选择器 (Auto / OpenGL / Vulkan)。
 *
 * 为什么单独一个文件: preferences_pages.c 已经贴着 500 行的策略上限 (见
 * cmake/CheckLines.cmake), 而这一行从"点了之后干什么"到"被拒也要留痕"都自成一块,
 * 正好搬出来 —— 与"透明背景"那一行 (preferences_window_transparency.c) 同一套做法。
 *
 * 这一行是**托盘那一项的另一个入口**, 两者必须完全一致: 写配置 -> 自动重启 -> 失效时
 * 提示。真正的实现只有一份 (runtime/lifecycle/renderer_switch.c), 这里只负责
 * 把"用户点在哪一段上"翻译成一次切换请求。
 *
 * 三条容易踩空的细节, 改之前先看:
 *   1. Vulkan 段可选与否**只看"这台机器 + 这次构建"支不支持** (探测在 render_backend.c),
 *      与"现在是不是已经在用 Vulkan"无关: 在 OpenGL 模式下它必须照样可选, 否则
 *      "GL -> VK"这条路根本没入口。结论每次变化都留一条日志, 用户说"点不动"时先看它。
 *   2. 判据是"点在哪一段", 不是"配置值有没有变"。配置值和实际在用的后端可能不同
 *      (启动时 Vulkan 初始化失败回退到 OpenGL 之后就是这样): 只比配置值的话, 用户
 *      重新点一次当前显示为选中的那一段会**静默无事发生**, 而同样的动作在托盘菜单里
 *      却能切 —— 两个入口行为不一致正是用户报告的那类"点了没反应"。
 *   3. 点到的就是当前后端时也要留一条日志: 用户觉得"没反应"的时候, 日志里必须有
 *      "不需要切"这一条, 否则排查只能靠猜。
 */
#include "preferences_internal.h"
#include "preferences_live2d_setup.h"
#include "preferences_theme.h"
#include "preferences_widgets.h"
#include "runtime.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/*
 * 可用性的结论只在"变了"的时候记一条: 设置窗口每帧都会问一次可用性, 每帧都记会刷屏。
 * 被禁用时原因必须写进日志 —— 用户反馈"点不动 / 灰的"时, 这一行是唯一的第一手证据
 * (上一版这里没有日志, 于是"Vulkan 被禁用"在日志里完全看不出来)。
 */
static void log_availability_once(bool selectable) {
    static int logged = -1;
    const int state = selectable ? 1 : 0;
    if (logged == state) return;
    logged = state;
    if (selectable)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 设置里的 Vulkan 段可选: 探测通过 (这台机器 + 本次构建都支持)");
    else
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 设置里的 Vulkan 段已禁用 (%s); 可用性只看这台机器 + 本次构建, "
            "与当前正在用的后端无关",
            bongo_cat_renderer_unavailable_reason(BONGO_CAT_RENDERER_VULKAN));
}

void bongo_cat_preferences_renderer_row(BongoCatApp *app,
    struct nk_context *context) {
    if (!app || !context) return;
    /* Live2D 的 Core 状态行与导入入口就放在"渲染后端"旁边: 两者回答的是同一个问题
       ("现在这台机器能怎么渲染"), 分开放用户会找不到。 */
    bongo_cat_preferences_live2d_setup_row(app, context);
    BongoCatModelPreferences *model = &app->settings.model;
    const bool vulkan_selectable =
        bongo_cat_renderer_available(BONGO_CAT_RENDERER_VULKAN);
    log_availability_once(vulkan_selectable);
    int clicked = -1;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
    /*
     * 刻意**不**把控件返回值写回 model->renderer: 配置的写入统一由共享切换路径完成
     * (它要先记"配置 X -> Y"、再落盘、再问用户)。控件在点击那一刻就改自己的选中值,
     * 如果这里顺手写回配置, 那条日志里的 X 就会变成用户点之后的新值, 事后完全看不出
     * 原来是什么。控件只在点到可用段时才改选中值, 而"点在哪一段"由 clicked 上报。
     */
    bongo_cat_pref_renderer(context,
        "renderer", tr(app, "pages.preference.cat.labels.renderer", "Renderer"),
        (int)model->renderer, vulkan_selectable, &clicked);
    if (clicked < 0) return;
    const BongoCatRenderer target = (BongoCatRenderer)clicked;
    /*
     * 只在"配置里就是它、而且它确实已经在跑"的时候才什么都不做 (仍然留一条日志)。
     * effective 用 resolve 算出来是为了照顾 Auto: 配置=auto 且正在跑 Vulkan 时, 点 Auto
     * 不该再触发一次重启; 但配置=auto 而用户点了 OpenGL 时必须真的切。
     * 目标不可用时不去问 resolve —— 那会多打一条误导性的"请求的后端不可用"警告, 拒绝
     * 路径自己会说明原因。
     */
    const bool selectable = target == BONGO_CAT_RENDERER_AUTO ||
        bongo_cat_renderer_available(target);
    if (selectable && target == model->renderer &&
        bongo_cat_renderer_resolve(target) == bongo_cat_renderer_active()) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 设置里点到的就是当前后端 %s, 无需切换",
            bongo_cat_renderer_name(bongo_cat_renderer_active()));
        return;
    }
    /* 禁用段点到的也是这里: 共享实现会把它当成"被拒"处理 —— 记日志 + 用户可见提示,
       和托盘那条路径一模一样 (文件头细节 2)。 */
    bongo_cat_app_switch_renderer(app, target,
        BONGO_CAT_RENDERER_ENTRY_SETTINGS);
}