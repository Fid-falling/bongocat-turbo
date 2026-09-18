#include "overlay_internal.h"

#include "bongo_cat/image.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL_log.h>

#ifndef BONGO_CAT_HAS_CUBISM
/* 非 Cubism 构建里叠加层画的是合成预览图: 基图 + 按下按键的左右手图。 */
static void refresh_composite(BongoCatOverlay *value) {
    if (!value->composite_dirty) return;
    value->composite_dirty = false;
    if (!value->left && !value->right) return;
    BongoCatError ignored = {0};
    BongoCatImage image = {0};
    if (bongo_cat_image_composite(value->background_path, value->left_path,
            value->right_path, value->clean_paws && value->left,
            value->clean_paws && value->right, &image, &ignored) != BONGO_CAT_OK) {
        if (ignored.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", ignored.message);
        return;
    }
    bongo_cat_texture_from_image(&value->composite, &image, false, &ignored);
    bongo_cat_image_free(&image);
}
#else
static void refresh_composite(BongoCatOverlay *value) { (void)value; }
#endif

static bool has_composite(const BongoCatOverlay *value) {
    return bongo_cat_texture_ready(&value->composite);
}

static void draw(BongoCatOverlay *value, const BongoCatTexture *texture,
    bool mirror, bool blend) {
    if (!value || !bongo_cat_texture_ready(texture)) return;
    const bool erase_left = !blend && value->composed_cover &&
        !has_composite(value) && value->left != 0;
    const bool erase_right = !blend && value->composed_cover &&
        !has_composite(value) && value->right != 0;
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        bongo_cat_vk_draw_overlay_quad(bongo_cat_vk_current(), texture->vk,
            mirror, value->reference_width, value->reference_height,
            erase_left, erase_right);
        return;
    }
    if (blend) {
        glEnable(GL_BLEND);
        value->gl.blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else glDisable(GL_BLEND);
    value->gl.use_program(value->program);
    value->gl.uniform_1i(value->mirror_location, mirror);
    value->gl.uniform_1i(value->image_location, 0);
    value->gl.uniform_1i(value->reference_width_location, value->reference_width);
    value->gl.uniform_1i(value->reference_height_location, value->reference_height);
    value->gl.uniform_1i(value->erase_left_location, erase_left);
    value->gl.uniform_1i(value->erase_right_location, erase_right);
    value->gl.active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture->gl);
    value->gl.bind_vertex_array(value->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    value->gl.bind_vertex_array(0);
}

void bongo_cat_overlay_draw_background(BongoCatOverlay *value, bool mirror) {
    if (!value) return;
    refresh_composite(value);
    bool active = value->left || value->right;
    draw(value, active && has_composite(value) ? &value->composite :
        &value->background, mirror, false);
}

void bongo_cat_overlay_draw_keys(BongoCatOverlay *value, bool mirror) {
    if (!value) return;
#ifdef BONGO_CAT_HAS_CUBISM
    /* Cubism 构建里左右手是独立的贴图 (GLuint), 这里仍是 OpenGL 专用路径。 */
    BongoCatTexture left = {.gl = value->left};
    BongoCatTexture right = {.gl = value->right};
    BongoCatTexture effect = {.gl = value->effect};
    draw(value, &left, mirror, true);
    draw(value, &right, mirror, true);
    (void)effect;
#else
    (void)mirror;
#endif
}

void bongo_cat_overlay_draw_pointer_before_keys(BongoCatOverlay *value) {
    if (bongo_cat_overlay_mver_pointer_enabled(value))
        bongo_cat_mver_pointer_overlay_draw_before_keys(value->mver_pointer);
}

void bongo_cat_overlay_draw_effect(BongoCatOverlay *value, bool mirror) {
    if (!value) return;
#ifdef BONGO_CAT_HAS_CUBISM
    BongoCatTexture effect = {.gl = value->effect};
    draw(value, &effect, mirror, true);
#else
    (void)mirror;
#endif
}

void bongo_cat_overlay_draw_pointer_after_keys(BongoCatOverlay *value) {
    if (bongo_cat_overlay_mver_pointer_enabled(value))
        bongo_cat_mver_pointer_overlay_draw_after_keys(value->mver_pointer);
}
