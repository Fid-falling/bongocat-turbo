/*
 * 模型卡片: 封面 + 名字 + 三个操作 (行为/目录/删除), 以及选中与加载两套动画。
 *
 * 卡片里的动画全部走 bongo_cat_ui_animate_* 那套按 id 的补间, 所以每张卡片必须
 * 用带 entry->id 的唯一 id (model-hover-<id> / model-selected-<id>) —— 用固定名字
 * 会让所有卡片共用一个动画值, 表现为"鼠标划过一张, 所有卡片一起动"。
 *
 * 点击的优先级也是刻意的: 名字区域 > 操作条 > 整卡选中。名字可能正在被重命名编辑,
 * 那时点击归文本编辑; 操作条上的点击不能穿透到"选中整个模型"。
 * 只读依赖: 本文件不直接改 settings, 一律通过 bongo_cat_preferences_model_select
 * 等入口写 pending 状态, 由 process_model_selection 在帧末统一执行。
 */
#include "preferences_model_card.h"
#include "preferences_model_card_paint.h"
#include "preferences_model_cover.h"
#include "preferences_notice.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_icons.h"
#include "ui_paint.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* 选中态淡入时长 (毫秒), 传给补间函数 */
#define MODEL_SELECTION_FADE_MS 240.0f

static const char *tr(BongoCatApp *app, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static void text(struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *value, struct nk_color color,
    const struct nk_user_font *font) {
    if (!font) font = context->style.font;
    nk_draw_text(canvas, bounds, value, nk_strlen(value), font,
        nk_rgba(0, 0, 0, 0), color);
}

/*
 * "点这里导入"的空卡片。返回真表示用户点了它 (调用方再去开文件对话框)。
 * 两处细节: import_drop_active 时也当作 hover —— 文件正被拖到窗口上, 这张卡要
 * 提前高亮, 让用户知道往哪儿放; 导入进行中则沿卡片描边走一段循环的进度光带
 * (时间取模, 所以是匀速循环而不是真实进度 —— 真实进度由页面上的状态读)。
 */
bool bongo_cat_preferences_model_import_card(BongoCatPreferences *value,
    struct nk_context *context) {
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return false;
    bounds.y += 5.0f; bounds.h += 1.0f;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    bool pointer_hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    bool hover = pointer_hover || value->import_drop_active;
    float lift = bongo_cat_ui_animate_eased(context, "model-import-hover",
        hover ? 1.0f : 0.0f, 250.0f, BONGO_CAT_UI_EASE_STANDARD);
    bounds.y -= 2.0f * lift;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    if (hover && p.effects) bongo_cat_ui_paint_shadow(context, bounds, 14,
        0, 8, 24, 0, nk_rgba(p.pink.r, p.pink.g, p.pink.b, 89));
    nk_fill_rect(canvas, bounds, 14, hover ? p.hover_pink : p.hover);
    bongo_cat_ui_paint_dashed_rounded(context, bounds, 14, 2, 5, 5,
        hover ? p.pink : p.accent);
    uint64_t import_started = 0;
    bool importing = bongo_cat_preferences_import_status(value->import_dialog,
        &import_started, NULL, NULL);
    if (importing && import_started) {
        const double lap_ns = 5000000000.0;
        double elapsed = (double)(SDL_GetTicksNS() - import_started);
        float progress = (float)fmod(elapsed / lap_ns, 1.0);
        struct nk_rect outline =
            bongo_cat_preferences_model_card_outline_bounds(bounds, 2.0f);
        bongo_cat_preferences_model_card_draw_progress(canvas, outline,
            13.0f, 2.0f, progress, p.pink);
        value->render_dirty = true;
    }
    float cx = bounds.x + bounds.w * .5f, cy = bounds.y + 91;
    bongo_cat_preferences_icon_draw(value, canvas,
        BONGO_CAT_UI_ICON_UPLOAD, nk_rect(cx - 20, cy - 20, 40, 40), p.pink);
    const char *label = tr(value->app,
        "pages.preference.model.hints.clickOrDragToImport",
        "Click or drag a model folder");
    float width = value->ui.caption_font->width(value->ui.caption_font->userdata,
        value->ui.caption_font->height, label, nk_strlen(label));
    text(context, canvas, nk_rect(cx - width * .5f, cy + 25,
        NK_MIN(width + 1, bounds.w - 20), 24), label, p.accent,
        value->ui.caption_font);
    const char *support = tr(value->app,
        "pages.preference.model.hints.supportedImportFormats",
        "Supports Mver and Tauri formats");
    width = value->ui.caption_font->width(value->ui.caption_font->userdata,
        value->ui.caption_font->height, support, nk_strlen(support));
    text(context, canvas, nk_rect(cx - width * .5f, cy + 47,
        NK_MIN(width + 1, bounds.w - 20), 24), support, p.pink,
        value->ui.caption_font);
    if (pointer_hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return pointer_hover && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, bounds);
}

static void action_icon(BongoCatPreferences *value,
    struct nk_command_buffer *canvas, struct nk_rect bounds, int icon,
    struct nk_color color) {
    bongo_cat_preferences_icon_draw(value, canvas, icon,
        nk_rect(bounds.x + (bounds.w - 18) * .5f,
            bounds.y + (bounds.h - 18) * .5f, 18, 18), color);
}

/*
 * 请求选中/取消选中一个模型。
 * 关键: 这里只写 pending_* 标志, 不动 settings, 也不重绘 —— 真正执行在
 * process_model_selection (帧末)。原因见那边注释: 在点击当帧改状态会被嵌套渲染
 * 重放这次点击, 结果刚选上的模型立刻被切回去。
 * 多宠模式下有数量上限, 超了只弹提示不改状态; 同一个模型正在加载时忽略重复点击。
 */
void bongo_cat_preferences_model_select(BongoCatPreferences *value,
    const BongoCatModelEntry *entry) {
    if (!value || !entry) return;
    if (value->model_loading && !strcmp(entry->id, value->loading_model_id))
        return;
    bool multiple = value->app->settings.model.multiple_pets;
    bool selected = bongo_cat_app_model_active(value->app, entry->id);
    if (multiple && !selected && bongo_cat_app_active_model_count(value->app) >=
            BONGO_CAT_ACTIVE_MODEL_CAP) {
        bongo_cat_preferences_notice_show(value->app, tr(value->app,
            "native.multiplePetsLimit",
            "Up to 8 desktop pets can be displayed at once"), true);
        return;
    }
    snprintf(value->pending_model_id, sizeof(value->pending_model_id),
        "%s", entry->id);
    value->pending_model_multiple = multiple;
    value->pending_model_active = !selected;
    if (!multiple && strcmp(entry->id, value->app->session.active_model_id))
        bongo_cat_preferences_model_visual_begin(value, entry->id);
    value->model_selection_pending = true;
    value->model_load_progress = 0.0f;
    bongo_cat_preferences_invalidate(value);
}

/*
 * 优先用"安装到本地的目录", 退回模型自己的源目录; 都打不开就明确报错,
 * 不能静默失败 —— 用户点了"打开目录"却什么都没发生是最容易被当成 bug 的行为。
 */
static void open_model_directory(BongoCatPreferences *value,
    const BongoCatModelEntry *entry) {
    const char *directory = entry->storage_directory[0] ?
        entry->storage_directory : entry->directory;
    if (bongo_cat_path_is_dir(directory) &&
        bongo_cat_platform_open_directory(directory)) return;
    bongo_cat_preferences_notice_show(value->app, tr(value->app,
        "pages.preference.model.hints.openDirectoryFailed",
        "Unable to open model directory"), true);
}

/*
 * 画封面。请求的封面尺寸按 raster_scale 放大 (高 DPI 下要的是物理像素), 拿回来
 * 再等比缩放进预览框 —— cover 的宽高比和预览框不一定一致, 所以用统一的 scale
 * 而不是拉伸, 否则封面会变形。
 * 拿不到封面时画一个占位图标, 不能留空: 空卡片看起来像加载失败。
 */
static void draw_cover(BongoCatPreferences *value,
    struct nk_command_buffer *canvas, struct nk_rect preview,
    const BongoCatModelEntry *entry, BongoCatUIPalette p) {
    float raster_scale = value->ui.raster_scale > 0.0f ?
        value->ui.raster_scale : 1.0f;
    const BongoCatModelCover *cover = bongo_cat_preferences_model_cover(
        value->app, entry, NK_MAX(1, (int)lroundf(preview.w * raster_scale)),
        NK_MAX(1, (int)lroundf(preview.h * raster_scale)));
    if (!cover) {
        bongo_cat_preferences_icon_draw(value, canvas, BONGO_CAT_UI_ICON_CAT,
            nk_rect(preview.x + (preview.w - 34) * .5f,
                preview.y + (preview.h - 34) * .5f, 34, 34), p.muted);
        return;
    }
    float scale = NK_MIN(preview.w / cover->width, preview.h / cover->height);
    struct nk_rect image = nk_rect(
        preview.x + (preview.w - cover->width * scale) * .5f,
        preview.y + (preview.h - cover->height * scale) * .5f,
        cover->width * scale, cover->height * scale);
    struct nk_image texture = nk_image_id((int)cover->texture);
    nk_draw_image(canvas, image, &texture, nk_rgb(255, 255, 255));
}

/*
 * 卡片底部的操作条: 行为设置 / 打开目录 / 删除。
 * 三格的宽度是按"可删除与否"两套算的 —— 预置模型和"附近的模型"不可删, 只分两格,
 * 硬留三格会出现一个永远点不动的空档。
 * behavior_enabled 的规则: 只有当前主模型能直接进行为设置; 多宠模式下其他宠物
 * 要先切成主模型 (primary 判定), 否则改的是错的宠物。禁用态用 region_hover 和
 * hover 分开表达, 是为了让"看起来可点但实际不可点"的项不显示悬停效果。
 * action_hover 回传给调用方, 避免操作条上的悬停又触发整卡选中。
 */
static void draw_actions(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, const BongoCatModelEntry *entry,
    BongoCatUIPalette p, bool selected, bool primary, bool storage_busy,
    bool *action_hover) {
    struct nk_rect actions = nk_rect(bounds.x + 1, bounds.y + bounds.h - 39,
        bounds.w - 2, 38);
    nk_fill_rect(canvas, actions, 0, p.surface);
    bool deletable = !entry->preset && !entry->managed;
    bool delete_enabled = deletable && !storage_busy;
    float width = actions.w / (deletable ? 3.0f : 2.0f);
    struct nk_rect items[3] = {
        nk_rect(actions.x, actions.y, width, actions.h),
        nk_rect(actions.x + width, actions.y, width, actions.h),
        nk_rect(actions.x + width * 2, actions.y, width, actions.h)};
    bool behavior_enabled = primary ||
        !value->app->settings.model.multiple_pets;
    bool region_hover[3] = {
        nk_input_is_mouse_hovering_rect(&context->input, items[0]),
        nk_input_is_mouse_hovering_rect(&context->input, items[1]),
        deletable && nk_input_is_mouse_hovering_rect(
            &context->input, items[2])};
    bool hover[3] = {behavior_enabled && region_hover[0],
        region_hover[1], delete_enabled && region_hover[2]};
    for (int i = 1; i < (deletable ? 3 : 2); ++i)
        nk_stroke_line(canvas, items[i].x, items[i].y + 10,
            items[i].x, items[i].y + items[i].h - 10, 1, p.border_subtle);
    for (int i = 0; i < 3; ++i)
        if (hover[i]) nk_fill_rect(canvas, items[i], 0, p.hover_pink);
    action_icon(value, canvas, items[0], BONGO_CAT_UI_ICON_SMILE,
        hover[0] ? p.pink : behavior_enabled ? p.muted : p.border_subtle);
    action_icon(value, canvas, items[1], BONGO_CAT_UI_ICON_FOLDER,
        hover[1] ? p.pink : p.muted);
    if (deletable) action_icon(value, canvas, items[2], BONGO_CAT_UI_ICON_TRASH,
        hover[2] ? p.danger : delete_enabled ? p.muted : p.border_subtle);
    *action_hover = region_hover[0] || region_hover[1] || region_hover[2];
    if (hover[0] && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, items[0])) {
        if (selected) bongo_cat_preferences_behavior_dialog_open(value);
        else bongo_cat_preferences_model_select(value, entry);
    } else if (hover[1] && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, items[1])) open_model_directory(value, entry);
    else if (hover[2] && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, items[2]))
        bongo_cat_preferences_remove_dialog_open(value->app, entry->id);
}

/*
 * 一张模型卡片。
 * 三个状态要分清: selected (在多宠集合里) / primary (主模型) / visual_target
 * (正在加载的那张)。selected_visual 特意排除了正在加载的卡片: 加载中的卡片由
 * 进度环表达状态, 再叠一层"已选中"的粉框会让两种反馈打架。
 * 描边与阴影都按动画量插值, 所以卡片切换是渐变而不是硬切。
 */
void bongo_cat_preferences_model_card(BongoCatPreferences *value,
    struct nk_context *context, const BongoCatModelEntry *entry,
    bool storage_busy) {
    BongoCatApp *app = value->app;
    bool selected = bongo_cat_app_model_active(app, entry->id);
    bool primary = !strcmp(entry->id, app->session.active_model_id);
    bool visual_target = value->model_load_visual_active &&
        !strcmp(entry->id, value->model_load_visual_id);
    bool selected_visual = selected && !visual_target;
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return;
    bounds.y += 5.0f; bounds.h += 1.0f;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    char animation_id[BONGO_CAT_ID_CAP + 24];
    snprintf(animation_id, sizeof(animation_id), "model-hover-%s", entry->id);
    float lift = bongo_cat_ui_animate_eased(context, animation_id,
        hover ? 1.0f : 0.0f, 250.0f, BONGO_CAT_UI_EASE_SWIFT);
    char selection_animation_id[BONGO_CAT_ID_CAP + 28];
    snprintf(selection_animation_id, sizeof(selection_animation_id),
        "model-selected-%s", entry->id);
    float selection_amount = bongo_cat_ui_animate_eased(context,
        selection_animation_id, selected_visual ? 1.0f : 0.0f,
        MODEL_SELECTION_FADE_MS, BONGO_CAT_UI_EASE_STANDARD);
    bounds.y -= 3.0f * lift;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    if (selection_amount > .001f && p.effects)
        bongo_cat_ui_paint_shadow(context, bounds, 14, 0, 8, 20, 0,
            bongo_cat_ui_color_alpha(nk_rgba(p.pink.r, p.pink.g,
                p.pink.b, 89), selection_amount));
    else if (!selected_visual && hover && p.effects)
        bongo_cat_ui_paint_shadow(context, bounds, 14, 0, 10, 28, 0,
            nk_rgba(p.accent.r, p.accent.g, p.accent.b, 46));
    nk_fill_rect(canvas, bounds, 14, p.surface);
    float preview_height = NK_MIN(128.0f, bounds.w * 354.0f / 612.0f);
    struct nk_rect preview = nk_rect(bounds.x + 1, bounds.y + 1,
        bounds.w - 2, preview_height);
    nk_fill_rect(canvas, preview, 12, p.surface);
    draw_cover(value, canvas, preview, entry, p);
    if (app->settings.model.multiple_pets)
        bongo_cat_preferences_model_card_draw_selection_badge(
            value, canvas, preview, p, selection_amount);
    float info_y = preview.y + preview.h + 8;
    struct nk_rect name_bounds = nk_rect(bounds.x + 9, info_y + 1,
        bounds.w - 18, 30);
    bool name_hover = bongo_cat_preferences_model_name_draw(value,
        context, canvas, entry, name_bounds, p);
    bool action_hover = false;
    draw_actions(value, context, canvas, bounds, entry, p, selected, primary,
        storage_busy, &action_hover);
    float outline_width = 1.0f + selection_amount;
    struct nk_rect outline = bongo_cat_preferences_model_card_outline_bounds(
        bounds, outline_width);
    float progress = visual_target ?
        bongo_cat_preferences_model_visual_progress(value, entry->id) :
        selected_visual ? 1.0f : 0.0f;
    struct nk_color outline_color = progress >= .999f ? p.pink :
        bongo_cat_ui_color_mix(
            bongo_cat_ui_color_mix(p.border_subtle, p.accent, lift),
            p.pink, selection_amount);
    nk_stroke_rect(canvas, outline, 14.0f - outline_width * .5f,
        outline_width, outline_color);
    bongo_cat_preferences_model_card_draw_progress(canvas, outline,
        14.0f - outline_width * .5f, 2.0f, progress, p.pink);
    if (!name_hover && (action_hover || hover))
        bongo_cat_ui_cursor_hover_rect(context, bounds,
            BONGO_CAT_UI_CURSOR_POINTER);
    if (!name_hover && !action_hover && hover &&
        nk_input_is_mouse_click_in_rect(&context->input,
            NK_BUTTON_LEFT, bounds))
        bongo_cat_preferences_model_select(value, entry);
}
