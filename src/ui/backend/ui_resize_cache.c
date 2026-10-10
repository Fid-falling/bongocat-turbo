/*
 * 拖动窗口边时的"帧缓存"快路径: 把上一帧的前缓冲拷成纹理, 尺寸变化期间直接拉伸
 * 呈现, 省掉整帧重建 (设置界面在实时缩放时最容易掉帧)。
 *
 * 只在 GL 路径可用: Vulkan 那边没有"回拷交换链图像"这条现成路径, capture() 直接
 * 返回 false 让调用方走正常重绘。
 * 两个计数 (presentations / failures) 是给自检用的: 前者说明快路径确实生效,
 * 后者说明 glCopyTexImage2D 在这台机器上不可用 (某些虚拟 GPU / 远程桌面), 此时
 * 功能自动退化, 不算错误。
 */
#include "ui_backend.h"

#include <stddef.h>

typedef struct CachedVertex {
    float position[2];
    float uv[2];
    nk_byte color[4];
} CachedVertex;

void bongo_cat_ui_resize_cache_destroy(BongoCatUIBackend *ui) {
    if (!ui) return;
    if (ui->resize_cache_texture)
        glDeleteTextures(1, &ui->resize_cache_texture);
    ui->resize_cache_texture = 0;
    ui->resize_cache_width = ui->resize_cache_height = 0;
}

bool bongo_cat_ui_resize_cache_capture(BongoCatUIBackend *ui) {
    if (!ui || !ui->window) return false;
    /* Vulkan 模式还没有帧缓存回拷这条路: 让调用方走正常的重绘 */
    if (bongo_cat_ui_uses_vulkan(ui)) return false;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(ui->window, &width, &height);
    if (width < 1 || height < 1) return false;
    bongo_cat_ui_resize_cache_destroy(ui);
    bongo_cat_gl_clear_errors();
    glGenTextures(1, &ui->resize_cache_texture);
    glBindTexture(GL_TEXTURE_2D, ui->resize_cache_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint previous = GL_BACK;
    glGetIntegerv(GL_READ_BUFFER, &previous);
    glReadBuffer(GL_FRONT);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0,
        width, height, 0);
    glReadBuffer((GLenum)previous);
    if (glGetError() != GL_NO_ERROR) {
        ui->resize_cache_failures++;
        bongo_cat_ui_resize_cache_destroy(ui);
        return false;
    }
    ui->resize_cache_width = width;
    ui->resize_cache_height = height;
    return true;
}

bool bongo_cat_ui_resize_cache_present(BongoCatUIBackend *ui) {
    if (!ui || !ui->window || !ui->resize_cache_texture) return false;
    int pixel_width = 0, pixel_height = 0;
    float width = 0.0f, height = 0.0f;
    SDL_GetWindowSizeInPixels(ui->window, &pixel_width, &pixel_height);
    bongo_cat_ui_logical_size(ui, &width, &height);
    if (pixel_width < 1 || pixel_height < 1 || width < 1 || height < 1)
        return false;
    const CachedVertex vertices[] = {
        {{0, 0}, {0, 1}, {255, 255, 255, 255}},
        {{width, 0}, {1, 1}, {255, 255, 255, 255}},
        {{width, height}, {1, 0}, {255, 255, 255, 255}},
        {{0, height}, {0, 0}, {255, 255, 255, 255}}
    };
    const nk_draw_index indices[] = {0, 1, 2, 0, 2, 3};
    float projection[4][4] = {{2.0f / width, 0, 0, 0},
        {0, -2.0f / height, 0, 0}, {0, 0, -1, 0}, {-1, 1, 0, 1}};
    bongo_cat_gl_clear_errors();
    glViewport(0, 0, pixel_width, pixel_height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    ui->gl.blend_equation(GL_FUNC_ADD);
    ui->gl.blend_func_separate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    ui->gl.use_program(ui->program);
    ui->gl.uniform_1i(ui->texture_location, 0);
    ui->gl.uniform_matrix_4fv(ui->projection_location, 1, GL_FALSE,
        &projection[0][0]);
    ui->gl.active_texture(GL_TEXTURE0);
    ui->gl.bind_vertex_array(ui->vao);
    ui->gl.bind_buffer(GL_ARRAY_BUFFER, ui->vbo);
    ui->gl.buffer_data(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
        GL_STREAM_DRAW);
    ui->gl.bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ui->ebo);
    ui->gl.buffer_data(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
        GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_2D, ui->resize_cache_texture);
    glDrawElements(GL_TRIANGLES, 6,
        sizeof(nk_draw_index) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);
    glDisable(GL_BLEND);
    if (glGetError() != GL_NO_ERROR) {
        ui->resize_cache_failures++;
        return false;
    }
    ui->resize_cache_presentations++;
    return true;
}
