/* 菜单行为的自测 (smoke): 由 bongo_cat_window_menu_self_test 串进启动自检, 失败会记日志并
 * 汇成 CI 致命错误 (见 lifecycle/app.c), 不是"仅供参考"的检查。
 * 这些用例直接在真实 app 上跑, 会改 live2d 的选中动作/表情与窗口状态, 所以每个子用例都必须
 * 先备份、结束前还原 —— 还原范围和顺序是这里最容易出错的地方。
 * 另外: 断言用的是"独立复现"的序号换算与标签拼接, 有意不调用产品代码里的私有实现,
 * 否则产品实现错了测试也会跟着错。 */
#include "window_menu.h"
#include "bongo_cat/shortcut.h"

#include <stdio.h>
#include <string.h>

/* 与 window_menu_behavior.c 的 nth_behavior 保持同样的过滤条件, 但刻意分开实现:
 * 计数走独立复现, 产品代码改了过滤规则时这里应该失败, 而不是跟着一起错。 */
static const BongoCatBehaviorEntry *test_nth_behavior(BongoCatApp *app,
    BongoCatBehaviorKind kind, size_t position) {
    if (!app) return NULL;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != kind) continue;
        if (kind == BONGO_CAT_BEHAVIOR_MOTION &&
            !bongo_cat_live2d_motion_visible(app->live2d,
                entry->group, entry->index)) continue;
        if (!position) return entry;
        position--;
    }
    return NULL;
}

static const char *test_behavior_shortcut(const BongoCatApp *app,
    const char *id) {
    if (!app || !id) return NULL;
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i) {
        const BongoCatBehaviorShortcut *binding =
            &app->settings.behavior_shortcuts[i];
        if (strcmp(binding->id, id) == 0) return binding->shortcut;
    }
    return NULL;
}

/* 表情用例: labels 导出的表情条数要与可见表情一致; 选中后 current_expression 指向正确
 * 下标; 预览-取消必须回到原表情, 预览-确认必须保留新表情 (见 preview_applied)。
 * 开头先清空表情以便验证"没有选中"的哨兵, 结束前必须把原表情写回, 否则污染后续用例。 */
static bool expression_self_test(BongoCatApp *app) {
    size_t count = 0;
    while (test_nth_behavior(app, BONGO_CAT_BEHAVIOR_EXPRESSION, count)) count++;
    if (!count) return true;
    int original = bongo_cat_live2d_expression(app->live2d);
    char motions[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
    bool motion_checked[BONGO_CAT_BEHAVIOR_CAP] = {false};
    char expressions[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
    size_t motion_count, expression_count, current_expression;
    bool passed = bongo_cat_live2d_set_expression(app->live2d, -1);
    bongo_cat_window_behavior_labels(app, motions, motion_checked, &motion_count,
        expressions, &expression_count, &current_expression);
    passed = passed && expression_count == count &&
        current_expression == BONGO_CAT_BEHAVIOR_CAP;
    size_t selected_position = count > 2 ? 2 : count - 1;
    const BongoCatBehaviorEntry *selected = test_nth_behavior(app,
        BONGO_CAT_BEHAVIOR_EXPRESSION, selected_position);
    passed = selected && bongo_cat_window_behavior_action(app,
        BONGO_CAT_MENU_EXPRESSION_FIRST + selected_position) &&
        bongo_cat_live2d_expression(app->live2d) == selected->index && passed;
    bongo_cat_window_behavior_labels(app, motions, motion_checked, &motion_count,
        expressions, &expression_count, &current_expression);
    passed = passed && expression_count == count &&
        current_expression == selected_position;
    if (passed && count > 1) {
        size_t alternate_position = selected_position ? 0 : 1;
        const BongoCatBehaviorEntry *alternate = test_nth_behavior(app,
            BONGO_CAT_BEHAVIOR_EXPRESSION, alternate_position);
        BongoCatWindowMenuPreview preview;
        bongo_cat_window_menu_preview_init(&preview, app);
        bongo_cat_window_menu_preview(&preview,
            BONGO_CAT_MENU_EXPRESSION_FIRST + alternate_position);
        passed = alternate && bongo_cat_live2d_expression(app->live2d) ==
            alternate->index;
        bongo_cat_window_menu_restore(&preview, BONGO_CAT_MENU_NONE);
        passed = passed && bongo_cat_live2d_expression(app->live2d) ==
            selected->index;
        bongo_cat_window_menu_preview_init(&preview, app);
        bongo_cat_window_menu_preview(&preview,
            BONGO_CAT_MENU_EXPRESSION_FIRST + alternate_position);
        bongo_cat_window_menu_restore(&preview,
            BONGO_CAT_MENU_EXPRESSION_FIRST + alternate_position);
        passed = passed && bongo_cat_live2d_expression(app->live2d) ==
            alternate->index && bongo_cat_window_menu_preview_applied(&preview,
                BONGO_CAT_MENU_EXPRESSION_FIRST + alternate_position);
    }
    if (passed) {
        bongo_cat_live2d_set_expression(app->live2d, selected->index);
        BongoCatWindowMenuPreview preview;
        bongo_cat_window_menu_preview_init(&preview, app);
        BongoCatMenuAction selected_action =
            BONGO_CAT_MENU_EXPRESSION_FIRST + selected_position;
        bongo_cat_window_menu_preview(&preview, selected_action);
        passed = bongo_cat_live2d_expression(app->live2d) == -1;
        bongo_cat_window_menu_restore(&preview, selected_action);
        passed = passed && bongo_cat_live2d_expression(app->live2d) == -1 &&
            bongo_cat_window_menu_preview_applied(&preview, selected_action);
    }
    bool restored = bongo_cat_live2d_set_expression(app->live2d, original);
    if (!passed || !restored) SDL_Log("Expression menu self-test failed: "
        "count=%llu mapped=%llu current=%llu original=%d restored=%d",
        (unsigned long long)count, (unsigned long long)expression_count,
        (unsigned long long)current_expression, original, restored);
    return restored && passed;
}

/* 用固定步长 (0.25s) 推进 live2d 而不是等真实时钟, 动作生命周期才可复现;
 * 调用方自己决定步数, 所以下面的等待都是相对时间的循环。 */
static void advance_motion(BongoCatApp *app, unsigned steps) {
    for (unsigned i = 0; i < steps; ++i)
        bongo_cat_app_step_live2d(app, 0.25f);
}

/* 动作生命周期不变量: 常驻动作 (persistent) 不会被时间推进清掉; 一次性动作播完会自己
 * 取消勾选, 而且不会顺带清掉常驻动作。模型缺少某一类时跳过对应断言。
 * 末尾尽量把常驻动作重新拉起来, 让退出时状态接近进入时。 */
static bool motion_lifecycle_self_test(BongoCatApp *app) {
    const BongoCatBehaviorEntry *persistent = NULL, *one_shot = NULL;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_MOTION ||
            !bongo_cat_live2d_motion_visible(app->live2d,
                entry->group, entry->index)) continue;
        if (bongo_cat_live2d_motion_persistent(app->live2d,
            entry->group, entry->index)) {
            if (!persistent) persistent = entry;
        } else if (!one_shot) one_shot = entry;
    }
    bool persistent_kept = true, checked = true, cleared = true;
    bool isolated = true, started = true;
    if (persistent) {
        if (bongo_cat_live2d_motion_selected(app->live2d,
            persistent->group, persistent->index)) {
            started = bongo_cat_live2d_start_motion(app->live2d,
                persistent->group, persistent->index) && started;
            advance_motion(app, 8);
        }
        started = bongo_cat_live2d_start_motion(app->live2d,
            persistent->group, persistent->index) && started;
        advance_motion(app, 240);
        persistent_kept = bongo_cat_live2d_motion_selected(app->live2d,
            persistent->group, persistent->index);
    }
    unsigned steps = 0;
    if (one_shot) {
        started = bongo_cat_live2d_start_motion(app->live2d,
            one_shot->group, one_shot->index) && started;
        checked = bongo_cat_live2d_motion_selected(app->live2d,
            one_shot->group, one_shot->index);
        while (steps++ < 1200 && bongo_cat_live2d_motion_selected(app->live2d,
            one_shot->group, one_shot->index)) advance_motion(app, 1);
        cleared = !bongo_cat_live2d_motion_selected(app->live2d,
            one_shot->group, one_shot->index);
        isolated = !persistent || bongo_cat_live2d_motion_selected(app->live2d,
            persistent->group, persistent->index);
    }
    if (persistent && bongo_cat_live2d_motion_selected(app->live2d,
        persistent->group, persistent->index))
        bongo_cat_live2d_start_motion(app->live2d,
            persistent->group, persistent->index);
    bool passed = started && persistent_kept && checked && cleared && isolated;
    SDL_Log("Motion lifecycle self-test: persistent=%d one_shot=%d "
        "checked=%d cleared=%d isolated=%d steps=%u result=%d",
        persistent != NULL, one_shot != NULL, checked, cleared, isolated,
        steps, passed);
    return passed;
}

/* 行为自测入口 (smoke 流程调用): 依次验证动作菜单的勾选态与标签、预览期间勾选态稳定、
 * 预览取消要回到原动作、确认要保留、两个动作可以并存、配对项联动关掉 (只有配对时才要求),
 * 然后跑动作生命周期与表情两组子用例。会在真实 app 上改 live2d 状态, 各子用例自己负责
 * 还原; 模型里没有动作/表情时跳过对应部分 (跳过算通过)。返回 false 会让整轮 smoke 自检失败
 * (见 lifecycle/app.c 的 smoke_menu 分支), 所以每个子结果都要汇进 passed。 */
bool bongo_cat_window_behavior_self_test(BongoCatApp *app) {
    if (!app) return false;
    bool passed = true;
    if (test_nth_behavior(app, BONGO_CAT_BEHAVIOR_MOTION, 0)) {
        const BongoCatBehaviorEntry *first = test_nth_behavior(app,
            BONGO_CAT_BEHAVIOR_MOTION, 0);
        /*
         * 一次性动作的勾选态按设计只在播放期间存在 (见 cubism_motion_playback.cpp 的
         * expire_motion_runs / clear_motion_runs): 预览取消会 StopAllMotions 并清记账,
         * 所以"预览前选中的一次性动作"取消后必须变成未选中; 持久型动作则必须保留。
         * 下面的断言按这个语义分开, 而不是假定所有模型的首个动作都是持久型。
         */
        const bool first_persistent = bongo_cat_live2d_motion_persistent(
            app->live2d, first->group, first->index);
        passed = bongo_cat_window_behavior_action(app,
            BONGO_CAT_MENU_MOTION_FIRST) && passed;
        char motions[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
        bool checked[BONGO_CAT_BEHAVIOR_CAP] = {false};
        bool before[BONGO_CAT_BEHAVIOR_CAP] = {false};
        char expressions[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
        size_t motion_count, expression_count, current_expression;
        bongo_cat_window_behavior_labels(app, motions, checked, &motion_count,
            expressions, &expression_count, &current_expression);
        passed = motion_count > 0 && checked[0] && passed;
        bool initial_passed = passed;
        const char *first_shortcut = test_behavior_shortcut(app, first->id);
        if (first_shortcut && first_shortcut[0]) {
            char shortcut_label[BONGO_CAT_SHORTCUT_CAP * 2];
            bongo_cat_shortcut_format(first_shortcut, shortcut_label, sizeof(shortcut_label));
            passed = strstr(motions[0], shortcut_label) != NULL && passed;
        }
        /*
         * 预览用例需要有"另一个可见动作"才有意义: 只有一个动作时, 预览它等于切换当前
         * 动作, "勾选态在预览期间不变"这个前提本身不成立。只有 1 个动作时跳过这一组
         * (不是跳过整个自检), 后面的选中与开关断言仍然会跑。
         */
        bool preview_stable = true;
        if (motion_count > 1) {
            const size_t preview_position = 1;
            const BongoCatBehaviorEntry *preview_entry = test_nth_behavior(app,
                BONGO_CAT_BEHAVIOR_MOTION, preview_position);
            const bool preview_persistent = preview_entry &&
                bongo_cat_live2d_motion_persistent(app->live2d,
                    preview_entry->group, preview_entry->index);
            /* hover 预览期间勾选态不能变: 预览是"试穿", 只有确认才写回会话。 */
            memcpy(before, checked, motion_count * sizeof(before[0]));
            BongoCatWindowMenuPreview preview;
            bongo_cat_window_menu_preview_init(&preview, app);
            bongo_cat_window_menu_preview(&preview,
                BONGO_CAT_MENU_MOTION_FIRST + preview_position);
            bongo_cat_window_behavior_labels(app, motions, checked, &motion_count,
                expressions, &expression_count, &current_expression);
            preview_stable = memcmp(before, checked,
                motion_count * sizeof(before[0])) == 0;
            passed = preview_stable && passed;
            bongo_cat_window_menu_restore(&preview, BONGO_CAT_MENU_NONE);
            const bool first_after_restore =
                bongo_cat_live2d_motion_selected(app->live2d, first->group,
                    first->index);
            passed = (first_persistent ? first_after_restore
                : !first_after_restore) && passed;
            passed = bongo_cat_window_behavior_action(app,
                BONGO_CAT_MENU_MOTION_FIRST + preview_position) && passed;
            bongo_cat_window_behavior_labels(app, motions, checked, &motion_count,
                expressions, &expression_count, &current_expression);
            passed = checked[preview_position] && passed;
            /* 确认预览 (restore 传动作本身 = commit): 事务必须落定; 持久型动作还必须
               保持选中。一次性动作确认后是否仍选中取决于它是否已经播完, 不能一概而论。 */
            BongoCatWindowMenuPreview commit_preview;
            bongo_cat_window_menu_preview_init(&commit_preview, app);
            bongo_cat_window_menu_preview(&commit_preview,
                BONGO_CAT_MENU_MOTION_FIRST + preview_position);
            bongo_cat_window_menu_restore(&commit_preview,
                BONGO_CAT_MENU_MOTION_FIRST + preview_position);
            const bool commit_applied = preview_entry &&
                bongo_cat_window_menu_preview_applied(&commit_preview,
                    BONGO_CAT_MENU_MOTION_FIRST + preview_position);
            const bool commit_selected = preview_entry &&
                bongo_cat_live2d_motion_selected(app->live2d,
                    preview_entry->group, preview_entry->index);
            passed = commit_applied &&
                (!preview_persistent || commit_selected) && passed;
        }
        bool independent = false;
        for (size_t i = 1; i < motion_count && !independent; ++i) {
            passed = bongo_cat_window_behavior_action(app,
                BONGO_CAT_MENU_MOTION_FIRST) && passed;
            passed = bongo_cat_window_behavior_action(app,
                BONGO_CAT_MENU_MOTION_FIRST + i) && passed;
            bool first_selected = bongo_cat_live2d_motion_selected(app->live2d,
                first->group, first->index);
            const BongoCatBehaviorEntry *candidate = test_nth_behavior(app,
                BONGO_CAT_BEHAVIOR_MOTION, i);
            bool candidate_selected = bongo_cat_live2d_motion_selected(app->live2d,
                candidate->group, candidate->index);
            independent = independent || (first_selected && candidate_selected);
        }
        /* 只有 CAT_motion_lock 这个名字的模型承诺多个动作可以同时选中, 其它模型不一定,
         * 所以只在此时断言 independent。 */
        if (motion_count > 2 && strcmp(first->group, "CAT_motion_lock") == 0)
            passed = independent && passed;
        /* 开关断言要求"点击前它是选中的": 上面的预览/独立性用例可能已经把它切走。 */
        if (!bongo_cat_live2d_motion_selected(app->live2d, first->group,
                first->index))
            passed = bongo_cat_window_behavior_action(app,
                BONGO_CAT_MENU_MOTION_FIRST) && passed;
        passed = bongo_cat_window_behavior_action(app,
            BONGO_CAT_MENU_MOTION_FIRST) && passed;
        bool paired = false;
        for (size_t i = 0; i < app->behaviors.count; ++i) {
            const BongoCatBehaviorEntry *item = &app->behaviors.entries[i];
            if (item->kind == BONGO_CAT_BEHAVIOR_MOTION &&
                !bongo_cat_live2d_motion_visible(app->live2d,
                    item->group, item->index) &&
                bongo_cat_live2d_motion_same_toggle(app->live2d,
                    first->group, first->index,
                    item->group, item->index)) paired = true;
        }
        bool toggled_off = !bongo_cat_live2d_motion_selected(app->live2d,
            first->group, first->index);
        /* 配对项与持久型动作点击后必须取消选中; 一次性非配对动作按设计会重播并保持
           选中, 这种语义下不断言"取消"。 */
        bool expects_off = paired || first_persistent;
        passed = (!expects_off || toggled_off) && passed;
        if (!passed) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Motion menu self-test failed: initial=%d preview=%d count=%llu "
            "independent=%d paired=%d persistent=%d off=%d",
            initial_passed, preview_stable, (unsigned long long)motion_count,
            independent, paired, first_persistent, toggled_off);
    }
    passed = motion_lifecycle_self_test(app) && passed;
    passed = expression_self_test(app) && passed;
    return passed;
}
