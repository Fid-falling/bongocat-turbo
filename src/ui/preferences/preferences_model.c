/*
 * "模型"页 (page 1): 多宠开关 + 模型卡片网格 + 导入卡片 + 附近的模型。
 *
 * 本文件的重点在加载动画, 它是"假进度"而不是真进度: 真实加载是同步的, 结束时
 * 会直接跳完成, 看起来像闪一下。所以模型加载走两段曲线 (见 visual_wait_progress):
 * 先按 RAMP 快速爬到 80%, 之后极慢地趋近 WAIT_CAP(95%), 加载真结束时再用
 * COMPLETE 那段把 95% -> 100% 补完。任何一段都不许超过 100%, 也不许出现回退。
 *
 * 另一条容易踩的坑 (见 process_model_selection 末尾): 选择模型之后不能在这里
 * 同步重绘 —— 嵌套渲染会把"点击卡片"那一次点击重放一遍, 于是刚选上的模型立刻
 * 又被切回去。所以只标脏, 让下一帧正常画。
 */
#include "preferences_state.h"
#include "preferences_internal.h"
#include "preferences_model_card.h"
#include "preferences_model_cover.h"
#include "preferences_notice.h"
#include "preferences_widgets.h"
#include "model_import.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_paint.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <string.h>

/*
 * 卡片高度与加载动画的最小重绘间隔 (30Hz): 加载期间动画不必每帧都画, 这里限流,
 * 但输入事件仍然照常触发渲染 —— 限的是"定时重绘", 不是交互响应。
 */
#define MODEL_CARD_HEIGHT 254
#define MODEL_LOAD_RENDER_INTERVAL_NS 33333333ull

static const char *tr(BongoCatApp *app, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/*
 * 两段式等待曲线: 前 RAMP 段线性到 80% (让用户立刻看到反馈), 之后减速逼近
 * WAIT_CAP。永远不返回 100% —— 100% 只能由"真的加载完"来给, 否则进度条会撒谎。
 */
static float visual_wait_progress(uint64_t elapsed) {
    if (elapsed <= BONGO_CAT_MODEL_LOAD_VISUAL_RAMP_NS)
        return .8f * (float)((double)elapsed /
            BONGO_CAT_MODEL_LOAD_VISUAL_RAMP_NS);
    uint64_t slow_elapsed = elapsed - BONGO_CAT_MODEL_LOAD_VISUAL_RAMP_NS;
    uint64_t slow_duration = BONGO_CAT_MODEL_LOAD_VISUAL_DURATION_NS -
        BONGO_CAT_MODEL_LOAD_VISUAL_RAMP_NS;
    float slow = slow_duration ? (float)((double)slow_elapsed / slow_duration) : 1.0f;
    return .8f + (BONGO_CAT_MODEL_LOAD_VISUAL_WAIT_CAP - .8f) *
        NK_CLAMP(0.0f, slow, 1.0f);
}

/*
 * 开始一段加载动画。completion_ns 清零表示"还没结束", 结束时刻由
 * model_visual_progress 第一次发现"不再 loading"时补上。
 */
void bongo_cat_preferences_model_visual_begin(BongoCatPreferences *value,
    const char *model_id) {
    if (!value || !model_id || !model_id[0]) return;
    value->model_load_visual_active = true;
    value->model_load_visual_started_ns = SDL_GetTicksNS();
    value->model_load_visual_completion_ns = 0;
    value->model_load_progress = 0.0f;
    snprintf(value->model_load_visual_id,
        sizeof(value->model_load_visual_id), "%s", model_id);
}

/*
 * 加载进度回调 (由加载器在同步加载过程中反复调用)。
 * 参数 progress 被刻意忽略: 真正的进度不可靠也不好映射到这条曲线上, 界面显示的
 * 是"已经等了多久"。这里只做两件事: 用时间推进动画, 以及按 30Hz 限流地重绘 ——
 * 加载本身占着主线程, 不节流会让加载更慢。
 */
void bongo_cat_preferences_model_load_progress(BongoCatPreferences *value,
    float progress) {
    if (!value || !value->model_loading) return;
    (void)progress;
    uint64_t now = SDL_GetTicksNS();
    uint64_t elapsed = now - value->model_load_visual_started_ns;
    value->model_load_progress = visual_wait_progress(elapsed);
    value->render_dirty = true;
    bool due = !value->model_load_render_ns ||
        now - value->model_load_render_ns >= MODEL_LOAD_RENDER_INTERVAL_NS;
    if (bongo_cat_preferences_visible(value) && due) {
        value->model_load_render_progress = progress;
        value->model_load_render_ns = now;
        bongo_cat_preferences_render(value);
    }
}

/*
 * 查询某个模型卡片当前的加载进度 (卡片每帧调一次)。
 * completion_ns 是"发现加载结束"的那一刻, 不是加载器给的时刻 —— 只有在这里才知道
 * 加载已经结束, 所以补完动画也从这里起算, 保证 95%->100% 看起来是连续的一小段。
 * 只有 id 匹配当前正在加载的模型才返回进度, 其它卡片一律 0。
 * 全部动画结束后把 active 清掉, 这是 needs_frame 停止拉帧的信号之一。
 */
float bongo_cat_preferences_model_visual_progress(BongoCatPreferences *value,
    const char *model_id) {
    if (!value || !model_id || !value->model_load_visual_active ||
        strcmp(value->model_load_visual_id, model_id)) return 0.0f;
    uint64_t now = SDL_GetTicksNS();
    uint64_t elapsed = now - value->model_load_visual_started_ns;
    if (!value->model_loading && !value->model_selection_pending) {
        if (!value->model_load_visual_completion_ns)
            value->model_load_visual_completion_ns = now;
        uint64_t completed = now - value->model_load_visual_completion_ns;
        float amount = NK_CLAMP(0.0f, (float)((double)completed /
            BONGO_CAT_MODEL_LOAD_VISUAL_COMPLETE_NS), 1.0f);
        float start = visual_wait_progress(elapsed);
        value->model_load_progress = start + (1.0f - start) * amount;
        if (amount >= 1.0f) {
            value->model_load_visual_active = false;
            value->model_load_visual_completion_ns = 0;
            value->model_load_progress = 1.0f;
        }
        value->render_dirty = true;
        return value->model_load_progress;
    }
    value->model_load_progress = visual_wait_progress(elapsed);
    value->render_dirty = true;
    return value->model_load_progress;
}

/* 加载成功时把进度直接推到 1.0: 已经出结果了就不必再演那段补完动画 */
static void finish_model_load_progress(BongoCatPreferences *value) {
    if (!value) return;
    if (value->model_load_visual_active) {
        uint64_t now = SDL_GetTicksNS();
        value->model_load_visual_completion_ns = now;
        value->model_load_progress = visual_wait_progress(now -
            value->model_load_visual_started_ns);
    } else value->model_load_progress = 1.0f;
    value->render_dirty = true;
}

/*
 * 在帧末真正执行"用户选了某个模型"这件事。
 * 为什么不在点击那一帧做: 卡片点击是在 UI 遍历里发生的, 在那里改状态会让同一帧
 * 之后的卡片看到不一致的模型列表。所以点击只写 pending_* 标志, 这里统一消费。
 * 先把 pending 快照到局部变量并清掉标志, 再干活 —— 干活过程中会重绘, 重绘可能又
 * 触发一次选择流程, 标志不清就会出现递归。
 * 注意它不重绘 (见函数末尾的英文注释): 只标脏, 由下一帧正常画。
 */
void bongo_cat_preferences_process_model_hidden(BongoCatPreferences *value) {
    if (!value || !value->pending_model_hidden_toggle) return;
    char id[BONGO_CAT_ID_CAP];
    snprintf(id, sizeof(id), "%s", value->pending_model_id);
    value->pending_model_hidden_toggle = false;
    value->pending_model_id[0] = '\0';
    bool hidden = bongo_cat_settings_model_hidden(&value->app->settings, id);
    bongo_cat_settings_set_model_hidden(&value->app->settings, id, !hidden);
    value->render_dirty = true;
}

void bongo_cat_preferences_process_model_selection(BongoCatPreferences *value) {
    if (!value || !value->model_selection_pending || value->model_loading)
        return;
    char id[BONGO_CAT_ID_CAP];
    snprintf(id, sizeof(id), "%s", value->pending_model_id);
    bool multiple = value->pending_model_multiple;
    bool active = value->pending_model_active;
    value->pending_model_id[0] = '\0';
    value->model_selection_pending = false;
    value->pending_model_multiple = false;
    value->model_loading = true;
    value->model_load_progress = 0.0f;
    value->model_load_render_progress = 0.0f;
    value->model_load_render_ns = 0;
    if (!value->model_load_visual_active || strcmp(value->model_load_visual_id, id))
        bongo_cat_preferences_model_visual_begin(value, id);
    snprintf(value->loading_model_id, sizeof(value->loading_model_id), "%s", id);
    BongoCatError error = {0};
    bool selected = multiple ? bongo_cat_app_set_model_active(
        value->app, id, active, &error) :
        bongo_cat_app_select_model_with_error(value->app, id, &error);
    if (selected) finish_model_load_progress(value);
    else {
        value->model_load_progress = 0.0f;
        value->model_load_visual_active = false;
        value->model_load_visual_id[0] = '\0';
    }
    value->model_loading = false;
    value->loading_model_id[0] = '\0';
    if (!selected) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "Model selection failed: id=%s error=%s", id,
        error.message[0] ? error.message : "Unable to display this model");
    bongo_cat_preferences_invalidate(value);
    /* Render on the next input frame. A nested render here would replay the
       card click that queued this selection and immediately toggle it back. */
}

/*
 * 冒烟: 自动选一个非预置模型, 等选择与字体重建都落定后再打开行为设置对话框。
 * 必须等 font_reload_pending / model_selection_pending / model_loading 全为假 ——
 * 提前开对话框会撞上字体图集重建, 量出来的布局和真实使用不一致。
 */
static void smoke_model_behavior(BongoCatPreferences *value) {
    BongoCatApp *app = value->app;
    if (app->smoke_preference_model_select) {
        for (size_t i = 0; i < app->models.count; ++i) {
            const BongoCatModelEntry *entry = &app->models.entries[i];
            if (entry->preset || !strcmp(entry->id,
                app->session.active_model_id)) continue;
            app->smoke_preference_model_select = false;
            value->smoke_behavior_open_pending = true;
            SDL_Log("Preferences smoke selecting model %s", entry->id);
            bongo_cat_preferences_model_select(value, entry);
            return;
        }
    }
    if (value->smoke_behavior_open_pending && !value->font_reload_pending &&
        !value->model_selection_pending && !value->model_loading) {
        value->smoke_behavior_open_pending = false;
        bongo_cat_preferences_behavior_dialog_open(value);
    }
}

/* managed=false 是本机模型, true 是"附近的模型"(同局域网/同目录发现的);
   presets 区分预置模型 (与导入模型分开成段, 排到页面下方)。 */
static size_t model_count(const BongoCatPreferences *value, bool managed,
    bool presets) {
    size_t count = 0;
    for (size_t i = 0; i < value->app->models.count; ++i) {
        const BongoCatModelEntry *entry = &value->app->models.entries[i];
        if (entry->managed == managed && entry->preset == presets) count++;
    }
    return count;
}

/*
 * 预置模型的展示顺序 (标准/键盘/手柄), 返回 3 表示"不是预置"或"未知预置"。
 * 排序是按 order 值分几趟画而不是排序数组: 模型列表是共享数据, 在这里改动顺序会
 * 影响别处 (托盘菜单、会话恢复), 所以顺序只体现在绘制上。
 */
static int preset_model_order(const BongoCatModelEntry *entry) {
    if (!entry || !entry->preset) return 3;
    if (!strcmp(entry->id, "standard")) return 0;
    if (!strcmp(entry->id, "keyboard")) return 1;
    if (!strcmp(entry->id, "gamepad")) return 2;
    return 3;
}

static void draw_models(BongoCatPreferences *value,
    struct nk_context *context, bool managed, bool presets,
    bool storage_busy) {
    if (!presets) {
        for (size_t i = 0; i < value->app->models.count; ++i) {
            const BongoCatModelEntry *entry = &value->app->models.entries[i];
            if (entry->managed != managed || entry->preset) continue;
            bongo_cat_preferences_model_card(value, context, entry,
                storage_busy);
        }
        return;
    }
    for (int order = 0; order < 3; ++order) {
        for (size_t i = 0; i < value->app->models.count; ++i) {
            const BongoCatModelEntry *entry = &value->app->models.entries[i];
            if (entry->managed != managed || preset_model_order(entry) != order)
                continue;
            bongo_cat_preferences_model_card(value, context, entry,
                storage_busy);
        }
    }
}

/*
 * 页面的组装顺序有依赖: covers_begin 要在任何卡片绘制之前调 (它负责本帧的封面
 * 缓存预热), covers_prune 必须在所有卡片画完之后调 —— 反了就会把这一帧还要用的
 * 封面当垃圾回收掉, 表现是封面一闪一闪。
 * 列数按内容宽度而不是窗口宽度算, 因为侧栏宽度本身也随宽度变化。
 * 卡片间距是临时改的全局样式, 用完必须还原 (Nuklear 没有样式作用域)。
 */
/*
 * 模型页顶部的分段标签 (类型 X 的模型商店): "模型" 是商店/网格, "随机" 收纳
 * 随机表情/随机动作/顺序切换。点某个标签只登记 model_page_tab 并让滚动回顶,
 * 内容切换在 page_model 里按标签分发, 不触发嵌套渲染。
 */
static void model_page_centered(struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *text, const struct nk_user_font *font,
    struct nk_color color) {
    float width = font->width(font->userdata, font->height, text,
        nk_strlen(text));
    nk_draw_text(canvas, nk_rect(bounds.x + (bounds.w - width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f, width + 1, font->height),
        text, nk_strlen(text), font, nk_rgba(0, 0, 0, 0), color);
}

static void model_page_segments(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    const char *labels[2] = {
        tr(app, "pages.preference.model.tabs.models", "Models"),
        tr(app, "pages.preference.model.tabs.random", "Random")};
    int count = 2;
    nk_layout_row_dynamic(context, 44, 1);
    struct nk_rect wrapper;
    nk_widget(&wrapper, context);
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, wrapper, 10, p.field);
    float width = (wrapper.w - 6) / (float)count;
    for (int i = 0; i < count; ++i) {
        struct nk_rect button = nk_rect(wrapper.x + 3 + width * i,
            wrapper.y + 3, width, wrapper.h - 6);
        bool hover = nk_input_is_mouse_hovering_rect(&context->input, button);
        char id[32]; snprintf(id, sizeof(id), "model-tab-%d", i);
        float active = bongo_cat_ui_animate_eased(context, id,
            value->model_page_tab == i ? 1.0f : 0.0f, 200,
            BONGO_CAT_UI_EASE_STANDARD);
        struct nk_color background = bongo_cat_ui_color_mix(
            hover ? p.hover : p.field, p.accent, active);
        if (active > .01f && p.effects) bongo_cat_ui_paint_shadow(context,
            button, 8, 0, 2, 10, 0,
            nk_rgba(p.accent.r, p.accent.g, p.accent.b, 89));
        nk_fill_rect(canvas, button, 8, background);
        model_page_centered(canvas, button, labels[i], value->ui.caption_font,
            bongo_cat_ui_color_mix(p.muted, nk_rgb(255, 255, 255), active));
        if (hover) bongo_cat_ui_cursor_hover_rect(context, button,
            BONGO_CAT_UI_CURSOR_POINTER);
        if (hover && nk_input_is_mouse_click_in_rect(&context->input,
            NK_BUTTON_LEFT, button) && value->model_page_tab != i) {
            value->model_page_tab = i;
            value->scroll_target[1] = 0.0f;
            value->render_dirty = true;
        }
    }
}

void bongo_cat_preferences_page_model(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    model_page_segments(value, context);
    if (value->model_page_tab != 0) {
        bongo_cat_preferences_page_model_random(value, context);
        return;
    }
    smoke_model_behavior(value);
    bongo_cat_preferences_model_covers_begin(app);
    bool multiple = app->settings.model.multiple_pets;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    if (bongo_cat_pref_toggle(context, "multiple-pets", tr(app,
            "native.multiplePets",
            "Display multiple"), "", &multiple))
        bongo_cat_app_set_multiple_pets(app, multiple);
    if (bongo_cat_preferences_model_section(value, context) &&
        !SDL_OpenURL("https://bongocat.pet/models"))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot open model library: %s", SDL_GetError());
    float width = nk_window_get_content_region(context).w;
    int columns = width >= 780 ? 4 : width >= 620 ? 3 : width >= 400 ? 2 : 1;
    struct nk_vec2 old_spacing = context->style.window.spacing;
    context->style.window.spacing = nk_vec2(14, 17);
    nk_layout_row_dynamic(context, MODEL_CARD_HEIGHT, columns);
    if (bongo_cat_preferences_model_import_card(value, context))
        bongo_cat_preferences_request_model_import(app->preferences);
    bool storage_busy = bongo_cat_preferences_import_status(
        value->import_dialog, NULL, NULL, NULL) ||
        bongo_cat_app_model_refresh_busy(app);
    draw_models(value, context, false, false, storage_busy);
    if (model_count(value, false, true)) {
        bongo_cat_pref_section(context,
            tr(app, "pages.preference.model.builtinsTitle",
                "Built-in models"));
        nk_layout_row_dynamic(context, MODEL_CARD_HEIGHT, columns);
        draw_models(value, context, false, true, storage_busy);
    }
    if (model_count(value, true, false)) {
        bongo_cat_pref_section(context,
            tr(app, "pages.preference.model.nearbyTitle", "Nearby models"));
        nk_layout_row_dynamic(context, MODEL_CARD_HEIGHT, columns);
        draw_models(value, context, true, false, storage_busy);
    }
    context->style.window.spacing = old_spacing;
    bongo_cat_preferences_model_covers_prune(app);
}