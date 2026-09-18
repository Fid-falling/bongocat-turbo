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
