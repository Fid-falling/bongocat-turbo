/* 主循环的每帧更新：随机表情计时与 Live2D 物理推进。两者都用调用方传入的单调纳秒时间戳
 * （不自己取时钟），便于测试与固定步长；也都带原生预览窗口接管期间直接返回的守卫 —— 拖动
 * 与缩放时预览窗口拥有模型状态，主循环不能同时推进它，否则会跳帧。返回值语义不同：随机表
 * 情无返回值，step_live2d 返回本帧是否真的变了。 */
#include "runtime.h"

#define NANOSECONDS_PER_SECOND 1000000000ull

/* xorshift32 PRNG，状态就存在 app 里。state 为 0 是未播种哨兵：由当前时间与模型 id 的
 * FNV-1a 风格混合算出种子，这样不同模型的首次随机序列不同，重启后也不会总是一样。返回值兼
 * 作新状态与随机数；播种后再次得到 0 的可能已被常数兜底排除。 */
static uint32_t random_expression_next(BongoCatApp *app, uint64_t now) {
    uint32_t state = app->random_expression_state;
    if (!state) {
        state = (uint32_t)now ^ (uint32_t)(now >> 32) ^ 0x9e3779b9u;
        const unsigned char *text = (const unsigned char *)app->loaded_model;
        while (*text) state = (state ^ *text++) * 16777619u;
        if (!state) state = 0x6d2b79f5u;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    app->random_expression_state = state;
    return state;
}

/* 清零到期时间：关掉功能或换模型后必须重新计满一整个间隔，而不是立刻弹一次表情。 */
void bongo_cat_random_expression_reset(BongoCatApp *app) {
    if (!app) return;
    app->random_expression_due_ns = 0;
    app->random_expression_interval_seconds = 0.0f;
}

/* now 是调用方的单调纳秒时钟。间隔设置非法（不在 1~3600 秒）时回落到默认值；
 * random_expression_due_ns 为 0 或间隔值变化时只重新排期并返回，保证改设置不会立刻弹表情。
 * 到期后从行为目录里挑表情：存在非当前候选时优先换一个，只有全目录都是当前表情时才重挑自
 * 己；真正切换成功才置 dirty。 */
void bongo_cat_random_expression_update(BongoCatApp *app, uint64_t now) {
    if (app && app->window_snapshot) return;
    if (!app || !app->settings.window.random_expression || !app->live2d) {
        bongo_cat_random_expression_reset(app);
        return;
    }
    float seconds = app->settings.window.random_expression_interval_seconds;
    if (!(seconds >= 1.0f && seconds <= 3600.0f))
        seconds = BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS;
    if (!app->random_expression_due_ns ||
        app->random_expression_interval_seconds != seconds) {
        app->random_expression_interval_seconds = seconds;
        app->random_expression_due_ns = now +
            (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);
        return;
    }
    if (now < app->random_expression_due_ns) return;
    app->random_expression_due_ns = now +
        (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);

    int current = bongo_cat_live2d_expression(app->live2d);
    size_t expression_count = 0;
    size_t alternate_count = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_EXPRESSION) continue;
        expression_count++;
        if (entry->index != current) alternate_count++;
    }
    if (!expression_count) return;

    bool choose_alternate = alternate_count > 0;
    size_t candidate_count = choose_alternate ? alternate_count : expression_count;
    size_t choice = random_expression_next(app, now) % candidate_count;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_EXPRESSION ||
            (choose_alternate && entry->index == current)) continue;
        if (choice--) continue;
        if (bongo_cat_live2d_set_expression(app->live2d, entry->index))
            app->dirty = true;
        return;
    }
}

/* 把一帧的时间片交给 Live2D 物理。delta 先夹到 0.25 秒（窗口被挂起或卡顿后的补帧上限），
 * 再细分成不超过 8 步、每步不超过 1/30 秒，避免大步长让物理发散。返回本帧状态是否变化，并
 * 据此置 dirty 触发重绘。 */
bool bongo_cat_app_step_live2d(BongoCatApp *app, float elapsed_seconds) {
    if (app && app->window_snapshot) return false;
    if (!app || !app->live2d || elapsed_seconds <= 0.0f) return false;
    if (elapsed_seconds > 0.25f) elapsed_seconds = 0.25f;
    unsigned steps = 1;
    while (steps < 8 && elapsed_seconds / steps > 1.0f / 30.0f) steps++;
    float step = elapsed_seconds / steps;
    bool changed = false;
    for (unsigned i = 0; i < steps; ++i)
        changed = bongo_cat_live2d_update(app->live2d, step) || changed;
    if (changed) app->dirty = true;
    return changed;
}
