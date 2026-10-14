/*
 * --ci-model-switch= 的自测驱动器: 无人值守地跑"连点好几个模型"与"切到坏模型"。
 *
 * 为什么需要它 (而不是在单测里手工调 request/step): 这两条结论都只在真实主循环里才成立,
 *   1. "连点 3~5 个模型, 最终只应用最后一次" —— 关键是**后来的请求落在上一次加载进行到一半
 *      的时候**。手工调一次 request + step 到结束覆盖不到这条路径 (中途没有一个半成品可丢);
 *   2. "切到坏模型时回退原模型且主循环继续出帧" —— 需要主循环按帧预算推进 + 每帧真的呈现
 *      一帧, 才能量到帧间隔 (见 frame_interval_probe.c)。
 * 它只登记请求, 不碰任何模型状态, 用的入口与设置页点卡片完全相同
 * (bongo_cat_app_request_model_switch), 所以它不是"另一条实现"。
 *
 * 三个参数 (都只在这一次运行里有效, 不写回配置):
 *   --ci-model-switch=id1,id2,...   要依次登记的模型 id (最多 BONGO_CAT_SMOKE_SWITCH_CAP 个);
 *   --ci-model-switch-delay-ms=N    第一帧之后等 N 毫秒再开始登记 (默认 2500, 让启动先收尾);
 *   --ci-model-switch-gap-ms=N      两次登记之间隔 N 毫秒 (默认 120)。
 * gap 默认取 120ms 而不是 0, 是因为 0 只是"同一帧里连发", 而 120ms 时上一次切换已经跑了
 * 十几帧半成品, 下一个请求必须走 supersede 那条丢弃路径 —— 那才是用户连点时真正发生的。
 *
 * 结束时会打一行摘要 (LIFECYCLE 类别, 一定会落进日志):
 *   [runtime] Smoke model switch complete: requested=N applied=<id>
 * 它就是脚本的断言锚点: applied 应该是这一串里的最后一个 (除非那一个是坏的)。
 */
#include "runtime.h"
#include "model_switch.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

void bongo_cat_smoke_model_switch_update(BongoCatApp *app, uint64_t now) {
    if (!app || !app->smoke_switch.enabled) return;
    if (!app->smoke_switch.armed) {
        app->smoke_switch.armed = true;
        app->smoke_switch.due_ns = now + app->smoke_switch.delay_ns;
        return;
    }
    /* 时间到就登记下一个; gap 为 0 时这一轮会把剩下的全部登记完 (同一帧连发), 否则每次
     * 只登记一个 —— while 而不是 if 是为了让"循环里已经过掉了几个间隔"这件事自然生效。 */
    while (app->smoke_switch.next < app->smoke_switch.count &&
        now >= app->smoke_switch.due_ns) {
        const char *id = app->smoke_switch.ids[app->smoke_switch.next];
        app->smoke_switch.next++;
        app->smoke_switch.due_ns += app->smoke_switch.gap_ns;
        BongoCatError error = {0};
        if (bongo_cat_app_request_model_switch(app, id, &error)) {
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] Smoke model switch request: index=%u id=%s",
                app->smoke_switch.next - 1, id);
            continue;
        }
        /* 请求被拒 (模型不存在) 只记一行: 这是脚本自己写错了 id, 不该让整轮自测崩掉。 */
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Smoke model switch request refused: index=%u id=%s error=%s",
            app->smoke_switch.next - 1, id,
            error.message[0] ? error.message : "unknown error");
    }
    if (app->smoke_switch.next < app->smoke_switch.count || app->smoke_switch.issued)
        return;
    /* 请求发完了, 但队列可能还在跑 (最后一个模型可能正在加载)。等它彻底静下来再报摘要,
     * 这样 applied 就是最终生效的那个模型 —— 也就是脚本要断言的"最后一次选择"。 */
    if (bongo_cat_app_model_switch_active(app)) return;
    app->smoke_switch.issued = true;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] Smoke model switch complete: requested=%u applied=%s",
        app->smoke_switch.count,
        app->loaded_model[0] ? app->loaded_model : "none");
}
