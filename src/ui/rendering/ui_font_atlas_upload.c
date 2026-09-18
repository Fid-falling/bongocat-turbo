#include "ui_font_atlas_internal.h"

#include <SDL3/SDL_opengl.h>

bool bongo_cat_ui_font_upload_atlas(BongoCatUIBackend *ui) {
    int width = 0, height = 0;
    const void *pixels = nk_font_atlas_bake(&ui->atlas, &width, &height,
        NK_FONT_ATLAS_ALPHA8);
    if (!pixels || width < 1 || height < 1) return false;
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
