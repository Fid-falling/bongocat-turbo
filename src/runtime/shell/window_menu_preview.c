/* 菜单预览事务: init 记录原始值, 菜单里每次 hover 走 preview "试穿", 关闭时 restore(选中项)
 * 决定保留还是回滚。分组切换必须先整体回滚再应用新组, 否则上一组的临时值会留在窗口上。
 * 三个回调都由平台菜单的模态循环驱动 (主循环此时停摆), 所以这里要自己管交互快照与补帧;
 * 预览回调画的是宠物窗口, GL 路径下的上下文切换由平台侧负责 (见 dial_window.c)。
 * 最容易被改错的地方: 把"预览"当成"已生效" —— 只有 applied 才是已落定。 */
#include "window_menu.h"

/* action 落在模型区间且下标有效时返回借用的模型条目, 否则 NULL (模型列表可能已变)。 */
static const BongoCatModelEntry *menu_model(const BongoCatApp *app,
    BongoCatMenuAction action) {
    if (!app || action < BONGO_CAT_MENU_MODEL_FIRST ||
        action >= BONGO_CAT_MENU_MODEL_FIRST + BONGO_CAT_MODEL_CAP) return NULL;
    size_t index = (size_t)(action - BONGO_CAT_MENU_MODEL_FIRST);
    return index < app->models.count ? &app->models.entries[index] : NULL;
}

/* 哪些动作值得试穿: 缩放、不透明度与动作/表情 (见 behavior_menu_action)。
 * 其余动作 (设置、退出、切模型、隐藏) 在 hover 时不该有任何副作用。 */
static bool previewable(BongoCatMenuAction action) {
    return (action >= BONGO_CAT_MENU_SCALE_50 &&
        action <= BONGO_CAT_MENU_OPACITY_100) ||
        bongo_cat_window_behavior_menu_action(action);
}

/* 分组 id: 缩放=1, 不透明度=2, 动作=4, 表情=5, 0 = 不属于任何可预览组。
 * 换组意味着"上一组试穿的值要先整体回滚", 所以这个划分必须跟 previewable 保持一致。 */
static int preview_group(BongoCatMenuAction action) {
    if (action >= BONGO_CAT_MENU_SCALE_50 &&
        action <= BONGO_CAT_MENU_SCALE_200) return 1;
    if (action >= BONGO_CAT_MENU_OPACITY_10 &&
        action <= BONGO_CAT_MENU_OPACITY_100) return 2;
    if (action >= BONGO_CAT_MENU_MOTION_FIRST &&
        action < BONGO_CAT_MENU_MOTION_FIRST + BONGO_CAT_BEHAVIOR_CAP) return 4;
    if (action >= BONGO_CAT_MENU_EXPRESSION_FIRST &&
        action < BONGO_CAT_MENU_EXPRESSION_FIRST + BONGO_CAT_BEHAVIOR_CAP) return 5;
    return 0;
}

/* 记录菜单打开瞬间的原始值 (缩放/不透明度/表情) 并清空 last/applied, 同时初始化模态帧
 * 计时。app 允许为 NULL (自测这么用), 此时用 100/100/-1 的默认值。 */
void bongo_cat_window_menu_preview_init(BongoCatWindowMenuPreview *state,
    BongoCatApp *app) {
    if (!state) return;
    *state = (BongoCatWindowMenuPreview){.app = app,
        .scale = app ? app->session.window.scale_percent : 100.0f,
        .opacity = app ? app->session.window.opacity_percent : 100.0f,
        .expression = app ? bongo_cat_live2d_expression(app->live2d) : -1,
        .last = BONGO_CAT_MENU_NONE, .applied = BONGO_CAT_MENU_NONE};
    bongo_cat_modal_frame_init(&state->modal_frame, app);
}

/* hover 预览 (平台菜单每帧报当前高亮项): 同一动作重复调用直接返回。切换分组前必须先
 * snapshot_end 并回滚上一组 —— 缩放/不透明度/动作/表情改的是同一个窗口的不同状态。
 * 缩放走共享的 bongo_cat_window_set_scale (它自己会补画一帧), 所以提前 return;
 * 不透明度没有渲染依赖, 这里自己设值再补画。表情的"再选一次即取消"语义由
 * bongo_cat_window_behavior_preview 负责; 已经落定的项只推动画不重复试穿。 */
void bongo_cat_window_menu_preview(void *userdata, BongoCatMenuAction action) {
    BongoCatWindowMenuPreview *state = userdata;
    if (!state || !state->app || action == state->last) return;
    BongoCatApp *app = state->app;
    int previous_group = preview_group(state->last);
    int next_group = preview_group(action);
    if (state->last != BONGO_CAT_MENU_NONE && previous_group != next_group)
        bongo_cat_window_menu_restore(state, BONGO_CAT_MENU_NONE);
    if (!previewable(action)) {
        bongo_cat_window_snapshot_end(app);
        if (previous_group == 0)
            bongo_cat_window_menu_restore(state, BONGO_CAT_MENU_NONE);
        state->last = action;
        return;
    }
    state->last = action;
    if (action >= BONGO_CAT_MENU_SCALE_50 &&
        action <= BONGO_CAT_MENU_SCALE_200) {
        bongo_cat_window_set_scale(app,
            (float)(50 + 10 * (action - BONGO_CAT_MENU_SCALE_50)));
        /* The shared scale path already presents the resized frame. */
        return;
    } else if (action >= BONGO_CAT_MENU_OPACITY_10 &&
        action <= BONGO_CAT_MENU_OPACITY_100) {
        bongo_cat_window_snapshot_end(app);
        app->session.window.opacity_percent =
            (float)(10 * (action - BONGO_CAT_MENU_OPACITY_10 + 1));
        if (!app->hover_hidden) {
            bongo_cat_app_cancel_hover_fade(app);
            bongo_cat_platform_set_opacity(&app->platform,
                app->session.window.opacity_percent / 100.0f);
        }
    /* 已经落定过的项再次 hover: 不重复试穿, 但仍然推进模态动画。 */
    } else if (action == state->applied &&
        bongo_cat_window_behavior_menu_action(action)) {
        bongo_cat_modal_frame_tick(&state->modal_frame);
        return;
    } else {
        bongo_cat_window_snapshot_end(app);
        if (next_group == 5 &&
            bongo_cat_live2d_expression(app->live2d) != state->expression)
            bongo_cat_live2d_set_expression(app->live2d, state->expression);
        if (!bongo_cat_window_behavior_preview(app, action)) {
            bongo_cat_app_render_now(app);
            return;
        }
        state->applied = action;
        bongo_cat_modal_frame_tick(&state->modal_frame);
        return;
    }
    bongo_cat_app_render_now(app);
}

void bongo_cat_window_menu_preview_tick(void *userdata) {
    BongoCatWindowMenuPreview *state = userdata;
    if (!state) return;
    bongo_cat_modal_frame_tick(&state->modal_frame);
}

/* 菜单关闭时的落定: selected 决定每一类设置是保留还是回滚 —— 缩放/不透明度看它是否落在
 * 对应区间, 模型看它是否指向某个模型, 动作尝试 commit (提交失败就回滚预览), 表情的保留
 * 条件与模型联动 (原因见下方行内注释)。
 * 保证返回前交互快照已结束 (snapshot_end), 只有真的改了东西才补画一帧。 */
void bongo_cat_window_menu_restore(void *userdata, BongoCatMenuAction selected) {
    BongoCatWindowMenuPreview *state = userdata;
    if (!state || !state->app) return;
    BongoCatApp *app = state->app;
    bool changed = false;
    bool keep_motion = selected >= BONGO_CAT_MENU_MOTION_FIRST &&
        selected < BONGO_CAT_MENU_MOTION_FIRST + BONGO_CAT_BEHAVIOR_CAP;
    bool committed_motion = keep_motion &&
        bongo_cat_window_behavior_commit_preview(app, selected);
    if (!committed_motion && bongo_cat_live2d_restore_motion_preview(app->live2d)) {
        state->applied = BONGO_CAT_MENU_NONE;
        changed = true;
    }
    if (committed_motion) { state->applied = selected; changed = true; }
    if (selected == BONGO_CAT_MENU_NONE)
        state->applied = BONGO_CAT_MENU_NONE;
    bool keep_model = menu_model(app, selected) != NULL;
    bool keep_scale = selected >= BONGO_CAT_MENU_SCALE_50 &&
        selected <= BONGO_CAT_MENU_SCALE_200;
    bool keep_opacity = selected >= BONGO_CAT_MENU_OPACITY_10 &&
        selected <= BONGO_CAT_MENU_OPACITY_100;
    /* Expressions belong to the model active when the menu opened. */
    bool keep_expression = keep_model ||
        (selected >= BONGO_CAT_MENU_EXPRESSION_FIRST &&
        selected < BONGO_CAT_MENU_EXPRESSION_FIRST + BONGO_CAT_BEHAVIOR_CAP);
    bool committed_expression = selected >= BONGO_CAT_MENU_EXPRESSION_FIRST &&
        selected < BONGO_CAT_MENU_EXPRESSION_FIRST + BONGO_CAT_BEHAVIOR_CAP;
    if (!keep_scale &&
        SDL_fabsf(app->session.window.scale_percent - state->scale) > .01f) {
        bongo_cat_window_set_scale(app, state->scale);
        changed = true;
    }
    if (!keep_opacity &&
        SDL_fabsf(app->session.window.opacity_percent - state->opacity) > .01f) {
        bongo_cat_window_snapshot_end(app);
        app->session.window.opacity_percent = state->opacity;
        if (!app->hover_hidden) {
            bongo_cat_app_cancel_hover_fade(app);
            bongo_cat_platform_set_opacity(&app->platform,
                state->opacity / 100.0f);
        }
        changed = true;
    }
    if (!keep_expression &&
        bongo_cat_live2d_expression(app->live2d) != state->expression &&
        bongo_cat_live2d_set_expression(app->live2d, state->expression)) {
        bongo_cat_app_step_live2d(app, 1.0f / 60.0f);
        changed = true;
    }
    if (committed_expression)
        bongo_cat_app_capture_behavior_state(app);
    bongo_cat_window_snapshot_end(app);
    if (changed) bongo_cat_app_render_now(app);
}

/* 判断某动作是否已在预览事务里落定 (applied 记录)。调用方据此避免重复执行同一次点击,
 * 见 window_menu_actions.c 结尾的分支。 */
bool bongo_cat_window_menu_preview_applied(
    const BongoCatWindowMenuPreview *state, BongoCatMenuAction selected) {
    return state && bongo_cat_window_behavior_menu_action(selected) &&
        state->applied == selected;
}
