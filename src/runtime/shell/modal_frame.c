/* 模态循环里的"一帧": 拖动/托盘/菜单/设置窗口的模态循环会挡住主循环, 但宠物必须继续动,
 * 这些地方就通过本文件的 tick 回调手动跑一帧 (drain 输入 -> 快照 -> hover -> 步进 live2d
 * -> 渲染)。dt 由 last_tick_ns 与 app->last_frame_ns 里较新的那个算起, 避免模态循环与主循环
 * 互相把 dt 吃掉或算重; 单帧 dt 上限 0.25s。 */
#include "modal_frame.h"

/* 计算本帧 dt 并推进基准: 起点取 state->last_tick_ns 与主循环的 app_frame_ns 中较新者 ——
 * 两个循环交替运行时不能重复消费同一段时间; now 不大于基准 (时钟未前进或回退) 返回 0。
 * 会写 state->last_tick_ns, 单帧上限 0.25s (休眠/卡顿后不让动画瞬移)。 */
static float modal_elapsed(BongoCatModalFrame *state, uint64_t app_frame_ns,
    uint64_t now) {
    uint64_t baseline = state->last_tick_ns;
    if (app_frame_ns > baseline) baseline = app_frame_ns;
    state->last_tick_ns = now;
    if (now <= baseline) return 0.0f;
    float elapsed = (float)((now - baseline) / 1000000000.0);
    return elapsed > 0.25f ? 0.25f : elapsed;
}

void bongo_cat_modal_frame_init(BongoCatModalFrame *state, BongoCatApp *app) {
    if (!state) return;
    *state = (BongoCatModalFrame){.app = app,
        .last_tick_ns = SDL_GetTicksNS()};
}

/* 模态循环每帧调一次 (签名即平台的 BongoCatModalTick): 自己取时钟推进一帧, 无返回值。
 * 它会把 app->last_frame_ns 推到当前时刻, 主循环恢复后不会把模态期间整段当成一个巨大 dt。 */
void bongo_cat_modal_frame_tick(void *userdata) {
    BongoCatModalFrame *state = userdata;
    if (!state || !state->app) return;
    BongoCatApp *app = state->app;
    uint64_t now = SDL_GetTicksNS();
    float elapsed = modal_elapsed(state, app->last_frame_ns, now);
    app->last_frame_ns = now;
    state->tick_count++;
    bongo_cat_app_drain_input(app, true);
    bongo_cat_window_snapshot_update(app, SDL_GetTicksNS());
    now = SDL_GetTicksNS();
    bongo_cat_app_update_hover(app, now);
    bongo_cat_app_update_hover_fade(app, now);
    if (!app->smoke_freeze_model && elapsed > 0.0f)
        bongo_cat_app_step_live2d(app, elapsed);
    bongo_cat_app_render_now(app);
}

bool bongo_cat_modal_frame_self_test(void) {
    BongoCatModalFrame state = {0};
    float first = modal_elapsed(&state, 983333333ull, 1000000000ull);
    float second = modal_elapsed(&state, 0, 1020000000ull);
    state.last_tick_ns = 1000000000ull;
    float main_loop_newer = modal_elapsed(&state, 1040000000ull, 1050000000ull);
    float capped = modal_elapsed(&state, 0, 1550000000ull);
    float backwards = modal_elapsed(&state, 0, 1500000000ull);
    return SDL_fabsf(first - 1.0f / 60.0f) < 0.0001f &&
        SDL_fabsf(second - 0.02f) < 0.0001f &&
        SDL_fabsf(main_loop_newer - 0.01f) < 0.0001f &&
        SDL_fabsf(capped - 0.25f) < 0.0001f && backwards == 0.0f;
}
