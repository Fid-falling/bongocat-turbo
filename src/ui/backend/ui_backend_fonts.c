/*
 * 字号查询的统一入口。为什么要绕一层而不是直接用 ui->xxx_font: 这些函数拿到的
 * 只有 nk_context, 需要先反查回 backend (context_backend 全局指针, 见
 * ui_backend.c)。查不到时 (例如宠物窗口那条路径传进来的 context) 一律回退到
 * Nuklear 的默认字体, 保证任何调用点都不会拿到 NULL。
 */
#include "ui_backend.h"

static const BongoCatUIBackend *backend(const struct nk_context *context) {
    return bongo_cat_ui_backend_for_context(context);
}

const struct nk_user_font *bongo_cat_ui_caption_font(
    const struct nk_context *context) {
    const BongoCatUIBackend *ui = backend(context);
    return ui && ui->caption_font ? ui->caption_font : context->style.font;
}

const struct nk_user_font *bongo_cat_ui_body_font(
    const struct nk_context *context) {
    const BongoCatUIBackend *ui = backend(context);
    return ui && ui->body_font ? ui->body_font : context->style.font;
}

const struct nk_user_font *bongo_cat_ui_label_font(
    const struct nk_context *context) {
    const BongoCatUIBackend *ui = backend(context);
    return ui && ui->label_font ? ui->label_font : context->style.font;
}

const struct nk_user_font *bongo_cat_ui_heading_font(
    const struct nk_context *context) {
    const BongoCatUIBackend *ui = backend(context);
    return ui && ui->heading_font ? ui->heading_font : context->style.font;
}

const struct nk_user_font *bongo_cat_ui_hero_font(
    const struct nk_context *context) {
    const BongoCatUIBackend *ui = backend(context);
    return ui && ui->hero_font ? ui->hero_font : context->style.font;
}
