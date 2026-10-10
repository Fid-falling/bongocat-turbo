#include "ui_font_atlas_internal.h"

#include <SDL3/SDL_opengl.h>

/*
 * 把烘好的图集 (NK_FONT_ATLAS_ALPHA8, 也就是单通道 8 位) 上传成纹理, 并把
 * nk_handle_id(id) 交给 Nuklear 当作所有字形纹理的句柄。
 *
 * 两条路径的"id"含义不同但用法一致:
 *  - Vulkan: 先注册进 UI 纹理表, id = 表下标 + 1 (0 是保留的失败值), 之后渲染时
 *    用 bongo_cat_ui_texture_lookup(id) 反查 BongoCatVkTexture;
 *  - OpenGL: id 就是 GL 纹理名。
 * 这就是为什么 ui->font_texture 的类型是 GLuint 却在 Vulkan 下存下标 —— 字段名
 * 有点误导, 但它始终表示"Nuklear 语义的纹理 id"。
 *
 * GL 路径必须设 SWIZZLE: 图集是单通道, 采样出来只有 .r 有值, 而 Nuklear 的
 * 着色器按 (color * texture) 算, 期望的是"白色 + alpha"。把 R/G/B 常置 1、A 取
 * RED, 就等价于随手得到一张白色带 alpha 的纹理, 不必在 CPU 侧展开成 RGBA。
 * 漏掉这一步的表现是: 文字只剩下红分量、变成一片暗红 (因为 G/B 采样出来是 0)。
 */
bool bongo_cat_ui_font_upload_atlas(BongoCatUIBackend *ui) {
    int width = 0, height = 0;
    const void *pixels = nk_font_atlas_bake(&ui->atlas, &width, &height,
        NK_FONT_ATLAS_ALPHA8);
    if (!pixels || width < 1 || height < 1) return false;
    if (bongo_cat_ui_uses_vulkan(ui)) {
        /* Vulkan: 单通道图集交给 vk_backend, 拿到 id 后告诉 Nuklear */
        BongoCatError error = {0};
        BongoCatVkTexture *texture = bongo_cat_vk_texture_from_alpha(ui->vk,
            pixels, width, height, "ui-font-atlas", &error);
        if (!texture) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Preferences font atlas upload failed: %s", error.message);
            return false;
        }
        int id = bongo_cat_ui_texture_register(ui, texture);
        if (!id) {
            bongo_cat_vk_texture_destroy(ui->vk, texture);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Preferences font atlas texture table is full");
            return false;
        }
        ui->font_texture = (GLuint)id;
        ui->font_atlas_width = width;
        ui->font_atlas_height = height;
        nk_font_atlas_end(&ui->atlas, nk_handle_id(id), &ui->null_texture);
        SDL_Log("Preferences font atlas ready (vulkan): id=%d size=%dx%d",
            id, width, height);
        return true;
    }
    SDL_Window *window = SDL_GL_GetCurrentWindow();
    SDL_GLContext context = SDL_GL_GetCurrentContext();
    SDL_Log("Preferences font atlas upload starting: %dx%d window=%u "
        "context=%p", width, height,
        window ? (unsigned)SDL_GetWindowID(window) : 0, (void *)context);
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (width > maximum || height > maximum) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preferences font atlas %dx%d exceeds GPU limit %d", width, height,
            maximum);
        return false;
    }
    ui->font_atlas_width = width;
    ui->font_atlas_height = height;
    SDL_Log("Preferences font atlas ready: %dx%d", width, height);
    bongo_cat_gl_clear_errors();
    glGenTextures(1, &ui->font_texture);
    glBindTexture(GL_TEXTURE_2D, ui->font_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
        GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLenum upload_error = glGetError();
    if (!ui->font_texture || upload_error != GL_NO_ERROR) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "Preferences font atlas upload failed (0x%x)",
            (unsigned)upload_error);
        if (ui->font_texture) glDeleteTextures(1, &ui->font_texture);
        ui->font_texture = 0;
        ui->font_atlas_width = 0;
        ui->font_atlas_height = 0;
        return false;
    }
    nk_font_atlas_end(&ui->atlas, nk_handle_id((int)ui->font_texture),
        &ui->null_texture);
    SDL_Log("Preferences font atlas upload completed: texture=%u size=%dx%d",
        ui->font_texture, width, height);
    return true;
}
