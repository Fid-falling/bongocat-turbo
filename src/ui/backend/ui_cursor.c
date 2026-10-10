/*
 * 鼠标指针形状。绘制代码只知道 nk_context, 所以用 active_backend 这个全局指针反查
 * (和 ui_backend.c 的 context_backend 同一套路, 但这里再存一份是为了让 cursor 模块
 * 不依赖 backend 的初始化顺序)。
 *
 * 工作方式是"按帧请求再统一应用": 绘制过程中通过 hover_* 记录想要的形状, 帧末由
 * bongo_cat_ui_cursor_apply() 设一次。这样同一个控件被画多次、或前后顺序变化时,
 * 结果都是确定的; 直接调 SDL_SetCursor 则会变成"最后画的那个说了算"。
 * 光标对象的创建是惰性的 (cursor_begin), 销毁时必须先把当前光标切回系统默认 ——
 * 否则会留下一个指向已释放 SDL_Cursor 的悬垂指针。
 */
#include "ui_backend.h"

static BongoCatUIBackend *active_backend;

static void apply_requested(BongoCatUIBackend *ui) {
    SDL_Cursor *cursor = ui->default_cursor ? ui->default_cursor : SDL_GetDefaultCursor();
    if (ui->requested_cursor == BONGO_CAT_UI_CURSOR_POINTER && ui->pointer_cursor)
        cursor = ui->pointer_cursor;
    else if (ui->requested_cursor == BONGO_CAT_UI_CURSOR_TEXT && ui->text_cursor)
        cursor = ui->text_cursor;
    else if (ui->requested_cursor == BONGO_CAT_UI_CURSOR_RESIZE_EW &&
        ui->resize_ew_cursor) cursor = ui->resize_ew_cursor;
    if (cursor && SDL_GetCursor() != cursor) SDL_SetCursor(cursor);
}

static BongoCatUIBackend *backend_for(const struct nk_context *context) {
    return active_backend && context == &active_backend->context
        ? active_backend : NULL;
}

void bongo_cat_ui_cursor_begin(BongoCatUIBackend *ui) {
    if (!ui) return;
    active_backend = ui;
    if (!ui->default_cursor)
        ui->default_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    if (!ui->pointer_cursor)
        ui->pointer_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    if (!ui->text_cursor)
        ui->text_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    if (!ui->resize_ew_cursor)
        ui->resize_ew_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    ui->requested_cursor = BONGO_CAT_UI_CURSOR_DEFAULT;
}

void bongo_cat_ui_cursor_reset(struct nk_context *context) {
    BongoCatUIBackend *ui = backend_for(context);
    if (ui) ui->requested_cursor = BONGO_CAT_UI_CURSOR_DEFAULT;
}

static void request_cursor(struct nk_context *context, BongoCatUICursor cursor) {
    BongoCatUIBackend *ui = backend_for(context);
    if (!ui) return;
    if (cursor == BONGO_CAT_UI_CURSOR_TEXT &&
        ui->requested_cursor == BONGO_CAT_UI_CURSOR_POINTER) return;
    ui->requested_cursor = cursor;
}

void bongo_cat_ui_cursor_hover_rect(struct nk_context *context,
    struct nk_rect bounds, BongoCatUICursor cursor) {
    if (context && nk_input_is_mouse_hovering_rect(&context->input, bounds))
        request_cursor(context, cursor);
}

void bongo_cat_ui_cursor_hover_widget(struct nk_context *context,
    BongoCatUICursor cursor) {
    if (context && nk_widget_is_hovered(context)) request_cursor(context, cursor);
}

void bongo_cat_ui_cursor_apply(BongoCatUIBackend *ui) {
    if (ui) apply_requested(ui);
}

void bongo_cat_ui_cursor_destroy(BongoCatUIBackend *ui) {
    if (!ui) return;
    SDL_Cursor *current = SDL_GetCursor();
    if (current == ui->default_cursor || current == ui->pointer_cursor ||
        current == ui->text_cursor || current == ui->resize_ew_cursor)
        SDL_SetCursor(SDL_GetDefaultCursor());
    if (ui->default_cursor) SDL_DestroyCursor(ui->default_cursor);
    if (ui->pointer_cursor) SDL_DestroyCursor(ui->pointer_cursor);
    if (ui->text_cursor) SDL_DestroyCursor(ui->text_cursor);
    if (ui->resize_ew_cursor) SDL_DestroyCursor(ui->resize_ew_cursor);
    ui->default_cursor = NULL;
    ui->pointer_cursor = NULL;
    ui->text_cursor = NULL;
    ui->resize_ew_cursor = NULL;
    if (active_backend == ui) active_backend = NULL;
}
