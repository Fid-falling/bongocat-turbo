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
    const BongoCatSettings *settings = &app->settings;
    size_t expression_count = 0;
    size_t alternate_count = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_EXPRESSION ||
            bongo_cat_settings_random_disabled(settings, entry->id)) continue;
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
            bongo_cat_settings_random_disabled(settings, entry->id) ||
            (choose_alternate && entry->index == current)) continue;
        if (choice--) continue;
        if (bongo_cat_live2d_set_expression(app->live2d, entry->index))
            app->dirty = true;
        return;
    }
}

/* 随机动作：和随机表情同一套排期/随机数/候选过滤逻辑，只是按 MOTION 行为挑。
 * 动作必须仍可见才算候选（持久动作被选中时再次触发即关掉它，与手动切换一致），
 * 因此不排除当前动作。真正触发（或切走）由 run_behavior 处理。 */
void bongo_cat_random_motion_update(BongoCatApp *app, uint64_t now) {
    if (app && app->window_snapshot) return;
    if (!app || !app->settings.window.random_motion || !app->live2d) {
        bongo_cat_random_motion_reset(app);
        return;
    }
    float seconds = app->settings.window.random_motion_interval_seconds;
    if (!(seconds >= 1.0f && seconds <= 3600.0f))
        seconds = BONGO_CAT_DEFAULT_RANDOM_MOTION_SECONDS;
    if (!app->random_motion_due_ns ||
        app->random_motion_interval_seconds != seconds) {
        app->random_motion_interval_seconds = seconds;
        app->random_motion_due_ns = now +
            (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);
        return;
    }
    if (now < app->random_motion_due_ns) return;
    app->random_motion_due_ns = now +
        (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);

    size_t motion_count = 0;
    const BongoCatSettings *settings = &app->settings;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_MOTION ||
            bongo_cat_settings_random_disabled(settings, entry->id) ||
            !bongo_cat_live2d_motion_visible(app->live2d, entry->group,
                entry->index)) continue;
        motion_count++;
    }
    if (!motion_count) return;
    size_t choice = random_expression_next(app, now) % motion_count;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_MOTION ||
            bongo_cat_settings_random_disabled(settings, entry->id) ||
            !bongo_cat_live2d_motion_visible(app->live2d, entry->group,
                entry->index)) continue;
        if (choice--) continue;
        bongo_cat_app_run_behavior(app, entry);
        return;
    }
}

void bongo_cat_random_motion_reset(BongoCatApp *app) {
    if (!app) return;
    app->random_motion_due_ns = 0;
    app->random_motion_interval_seconds = 0.0f;
}

/* 随机声音：和随机动作同一套排期/随机数/候选过滤逻辑，只是按 SOUND 行为挑。
 * 参与判定走同一份 random_disabled 参与集合；只挑有实际音效文件(sound 非空)的
 * 条目，触发委托给 run_behavior（含绑定音效、状态回写）。 */
void bongo_cat_random_sound_update(BongoCatApp *app, uint64_t now) {
    if (app && app->window_snapshot) return;
    if (!app || !app->settings.window.random_sound || !app->audio) {
        bongo_cat_random_sound_reset(app);
        return;
    }
    float seconds = app->settings.window.random_sound_interval_seconds;
    if (!(seconds >= 1.0f && seconds <= 3600.0f))
        seconds = BONGO_CAT_DEFAULT_RANDOM_SOUND_SECONDS;
    if (!app->random_sound_due_ns ||
        app->random_sound_interval_seconds != seconds) {
        app->random_sound_interval_seconds = seconds;
        app->random_sound_due_ns = now +
            (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);
        return;
    }
    if (now < app->random_sound_due_ns) return;
    app->random_sound_due_ns = now +
        (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);

    size_t sound_count = 0;
    const BongoCatSettings *settings = &app->settings;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_SOUND || !entry->sound[0] ||
            bongo_cat_settings_random_disabled(settings, entry->id)) continue;
        sound_count++;
    }
    if (!sound_count) return;
    size_t choice = random_expression_next(app, now) % sound_count;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_SOUND || !entry->sound[0] ||
            bongo_cat_settings_random_disabled(settings, entry->id)) continue;
        if (choice--) continue;
        bongo_cat_app_run_behavior(app, entry);
        return;
    }
}

void bongo_cat_random_sound_reset(BongoCatApp *app) {
    if (!app) return;
    app->random_sound_due_ns = 0;
    app->random_sound_interval_seconds = 0.0f;
}

/* 顺序模型：按目录顺序切换到下一个参与且未被隐藏的模型，到末尾回绕。
 * 参与判定对齐 BongoCat-X: 每个模型配一个合成行为 id "switch:<模型id>"，默认全参
 * 与；只要这个 id 出现在 random_disabled 参与集合里(或模型本身在隐藏列表里)，就跳过
 * 它。只有一个候选模型时什么都不做。 */
void bongo_cat_sequential_model_update(BongoCatApp *app, uint64_t now) {
    if (app && app->window_snapshot) return;
    if (!app || !app->settings.window.sequential_model || !app->live2d) {
        bongo_cat_sequential_model_reset(app);
        return;
    }
    float seconds = app->settings.window.sequential_model_interval_seconds;
    if (!(seconds >= 1.0f && seconds <= 3600.0f))
        seconds = BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS;
    if (!app->sequential_model_due_ns ||
        app->sequential_model_interval_seconds != seconds) {
        app->sequential_model_interval_seconds = seconds;
        app->sequential_model_due_ns = now +
            (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);
        return;
    }
    if (now < app->sequential_model_due_ns) return;
    app->sequential_model_due_ns = now +
        (uint64_t)(seconds * (float)NANOSECONDS_PER_SECOND);

    const BongoCatSettings *settings = &app->settings;
    const char *active = app->session.active_model_id;
    const BongoCatModelEntry *first = NULL;
    const BongoCatModelEntry *next = NULL;
    bool after_active = false;
    for (size_t i = 0; i < app->models.count; ++i) {
        const BongoCatModelEntry *entry = &app->models.entries[i];
        if (bongo_cat_settings_model_hidden(settings, entry->id)) continue;
        char switch_id[BONGO_CAT_BEHAVIOR_ID_CAP];
        snprintf(switch_id, sizeof(switch_id), "switch:%s", entry->id);
        if (bongo_cat_settings_random_disabled(settings, switch_id)) continue;
        if (active[0] && !strcmp(entry->id, active)) {
            after_active = true;
            continue;
        }
        if (!first) first = entry;
        if (after_active) { next = entry; break; }
    }
    if (!next) next = first;
    if (!next || !strcmp(next->id, active)) return;
    if (bongo_cat_app_select_model(app, next->id)) app->dirty = true;
}

void bongo_cat_sequential_model_reset(BongoCatApp *app) {
    if (!app) return;
    app->sequential_model_due_ns = 0;
    app->sequential_model_interval_seconds = 0.0f;
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
