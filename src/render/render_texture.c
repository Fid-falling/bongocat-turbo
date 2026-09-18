#include "render_texture.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <string.h>

bool bongo_cat_texture_from_file(BongoCatTexture *texture, const char *path,
    BongoCatError *error) {
    if (!texture || !path) return false;
    memset(texture, 0, sizeof(*texture));
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        texture->vk = bongo_cat_vk_texture_load(bongo_cat_vk_current(), path,
            &texture->width, &texture->height, error);
        return texture->vk != NULL;
    }
    texture->gl = bongo_cat_image_texture(path, &texture->width,
        &texture->height, error);
    return texture->gl != 0;
}

bool bongo_cat_texture_from_image(BongoCatTexture *texture,
    const BongoCatImage *image, bool premultiply, BongoCatError *error) {
    if (!texture || !image || image->width <= 0 || image->height <= 0) return false;
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        /* 合成结果每次都整块替换: Vulkan 版本重新建一份纹理, 旧的就地释放 */
        BongoCatVkTexture *replacement = bongo_cat_vk_texture_from_pixels(
            bongo_cat_vk_current(), image->pixels, image->width, image->height,
            "overlay-composite", error);
        if (!replacement) return false;
        BongoCatVk *vk = bongo_cat_vk_current();
        if (texture->vk) bongo_cat_vk_texture_destroy(vk, texture->vk);
        texture->vk = replacement;
        texture->width = image->width;
        texture->height = image->height;
        return true;
    }
    texture->gl = bongo_cat_image_upload_texture(image, texture->gl, premultiply);
    texture->width = image->width;
    texture->height = image->height;
    return texture->gl != 0;
}

void bongo_cat_texture_release(BongoCatTexture *texture) {
    if (!texture) return;
    if (texture->vk) {
        bongo_cat_vk_texture_destroy(bongo_cat_vk_current(), texture->vk);
        texture->vk = NULL;
    }
    if (texture->gl) {
        /* OpenGL 纹理的删除必须发生在当前上下文里, 调用方负责保证这一点 */
        bongo_cat_image_release_texture(texture->gl);
        texture->gl = 0;
    }
    texture->width = texture->height = 0;
}

bool bongo_cat_texture_ready(const BongoCatTexture *texture) {
    return texture && (texture->vk != NULL || texture->gl != 0);
}
