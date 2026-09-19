#include "ui_backend.h"
#include "bongo_cat/image.h"
#include "ui_font_atlas.h"
#include "ui_paint.h"
#include "ui_paint_cache.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define UI_COMMAND_BUFFER_TRIM_LIMIT (1024u * 1024u)

typedef struct UIVertex {
    float position[2];
    float uv[2];
    nk_byte color[4];
} UIVertex;

static const char *vertex_source =
    "#version 330 core\n"
    "layout(location=0) in vec2 Position;layout(location=1) in vec2 TexCoord;"
    "layout(location=2) in vec4 Color;uniform mat4 Projection;"
    "out vec2 FragUV;out vec4 FragColor;"
    "void main(){FragUV=TexCoord;FragColor=Color;gl_Position=Projection*vec4(Position,0,1);}";
static const char *fragment_source =
    "#version 330 core\n"
    "in vec2 FragUV;in vec4 FragColor;uniform sampler2D Texture;out vec4 OutColor;"
    "void main(){OutColor=FragColor*texture(Texture,FragUV);}";
static BongoCatUIBackend *context_backend;

static void clipboard_copy(nk_handle user, const char *text, int length) {
    (void)user;
    if (!text || length <= 0) return;
    char *copy = malloc((size_t)length + 1);
    if (!copy) return;
    memcpy(copy, text, (size_t)length);
    copy[length] = '\0';
    SDL_SetClipboardText(copy);
    free(copy);
}

static void clipboard_paste(nk_handle user, struct nk_text_edit *edit) {
    (void)user;
    char *text = SDL_GetClipboardText();
    if (text) {
        nk_textedit_paste(edit, text, nk_strlen(text));
        SDL_free(text);
    }
}

bool bongo_cat_ui_uses_vulkan(const BongoCatUIBackend *ui) {
    return ui && ui->vk != NULL;
}

int bongo_cat_ui_texture_register(BongoCatUIBackend *ui,
    BongoCatVkTexture *texture) {
    if (!ui || !texture) return 0;
    for (int i = 0; i < BONGO_CAT_UI_VK_TEXTURES; ++i) {
        if (ui->vk_textures[i]) continue;
        ui->vk_textures[i] = texture;
        return i + 1;
    }
    return 0;
}

void bongo_cat_ui_texture_unregister(BongoCatUIBackend *ui, int id) {
    if (!ui || id <= 0 || id > BONGO_CAT_UI_VK_TEXTURES) return;
    BongoCatVkTexture *texture = ui->vk_textures[id - 1];
    ui->vk_textures[id - 1] = NULL;
    if (texture) bongo_cat_vk_texture_destroy(ui->vk, texture);
}

BongoCatVkTexture *bongo_cat_ui_texture_lookup(const BongoCatUIBackend *ui,
    int id) {
    if (!ui || id <= 0 || id > BONGO_CAT_UI_VK_TEXTURES) return NULL;
    return ui->vk_textures[id - 1];
}

void bongo_cat_ui_texture_move(BongoCatUIBackend *dst, BongoCatUIBackend *src,
    int id) {
    if (!dst || !src || id <= 0 || id > BONGO_CAT_UI_VK_TEXTURES) return;
    dst->vk_textures[id - 1] = src->vk_textures[id - 1];
    src->vk_textures[id - 1] = NULL;
}

int bongo_cat_ui_texture_from_file(BongoCatUIBackend *ui, const char *path,
    int max_width, int max_height, int *width, int *height, BongoCatError *error) {
    if (!ui || !path) return 0;
    if (bongo_cat_ui_uses_vulkan(ui)) {
        BongoCatImage image = {0};
        if (bongo_cat_image_fit(path, max_width, max_height, &image, error) !=
            BONGO_CAT_OK) return 0;
        BongoCatVkTexture *texture = bongo_cat_vk_texture_from_pixels(ui->vk,
            image.pixels, image.width, image.height, path, error);
        int id = texture ? bongo_cat_ui_texture_register(ui, texture) : 0;
        if (texture && !id) bongo_cat_vk_texture_destroy(ui->vk, texture);
        if (width) *width = image.width;
        if (height) *height = image.height;
        bongo_cat_image_free(&image);
        return id;
    }
    return (int)bongo_cat_image_texture_thumbnail(path, max_width, max_height,
        width, height, error);
}

void bongo_cat_ui_texture_release_id(BongoCatUIBackend *ui, int id) {
    if (!ui || id <= 0) return;
    if (bongo_cat_ui_uses_vulkan(ui)) {
        bongo_cat_ui_texture_unregister(ui, id);
        return;
    }
    GLuint name = (GLuint)id;
    glDeleteTextures(1, &name);
}

static bool create_device(BongoCatUIBackend *ui, BongoCatError *error) {
    /* Vulkan 模式下界面纹理与管线由 vk_backend 提供, 这里不碰 OpenGL */
    if (bongo_cat_ui_uses_vulkan(ui)) return true;
    if (!bongo_cat_gl_load(&ui->gl, error)) return false;
    ui->program = bongo_cat_gl_program(&ui->gl, vertex_source, fragment_source, error);
    if (!ui->program) return false;
    ui->texture_location = ui->gl.uniform_location(ui->program, "Texture");
    ui->projection_location = ui->gl.uniform_location(ui->program, "Projection");
    ui->gl.gen_vertex_arrays(1, &ui->vao);
    ui->gl.bind_vertex_array(ui->vao);
    ui->gl.gen_buffers(1, &ui->vbo);
    ui->gl.gen_buffers(1, &ui->ebo);
    ui->gl.bind_buffer(GL_ARRAY_BUFFER, ui->vbo);
    ui->gl.bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ui->ebo);
    ui->gl.enable_attribute(0);
    ui->gl.attribute_pointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex),
        (void *)offsetof(UIVertex, position));
    ui->gl.enable_attribute(1);
    ui->gl.attribute_pointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex),
        (void *)offsetof(UIVertex, uv));
    ui->gl.enable_attribute(2);
    ui->gl.attribute_pointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UIVertex),
        (void *)offsetof(UIVertex, color));
    return true;
}

static void trim_commands(BongoCatUIBackend *ui) {
    if (!ui || ui->commands.allocated <= UI_COMMAND_BUFFER_TRIM_LIMIT) return;
    nk_buffer_free(&ui->commands);
    nk_buffer_init_default(&ui->commands);
}

bool bongo_cat_ui_init(BongoCatUIBackend *ui, SDL_Window *window,
    BongoCatVk *vk,
    const char *body_font_path, const char *body_fallback_path,
    const char *body_korean_fallback_path, const char *heading_font_path,
    const char *heading_fallback_path,
    const char *heading_korean_fallback_path,
    const nk_rune *glyph_ranges, float layout_scale, float raster_scale,
    BongoCatError *error) {
    memset(ui, 0, sizeof(*ui));
    ui->window = window;
    ui->vk = vk;
    ui->layout_scale = layout_scale;
    ui->raster_scale = raster_scale;
    ui->vertex_capacity = 256 * 1024;
    ui->element_capacity = 96 * 1024;
    ui->vertices = malloc(ui->vertex_capacity);
    ui->elements = malloc(ui->element_capacity);
    if (!ui->vertices || !ui->elements || !nk_init_default(&ui->context, NULL) ||
        !create_device(ui, error) || !bongo_cat_ui_font_atlas_create(ui,
            body_font_path, body_fallback_path, body_korean_fallback_path,
            heading_font_path, heading_fallback_path,
            heading_korean_fallback_path, glyph_ranges, raster_scale)) {
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_MEMORY, "Preferences UI resources could not be created");
        bongo_cat_ui_destroy(ui);
        return false;
    }
    nk_buffer_init_default(&ui->commands);
    context_backend = ui;
    ui->context.clip.copy = clipboard_copy;
    ui->context.clip.paste = clipboard_paste;
    return true;
}

static void release(BongoCatUIBackend *ui, bool release_gl) {
    if (!ui) return;
    if (context_backend == ui) context_backend = NULL;
    if (bongo_cat_ui_uses_vulkan(ui)) {
        if (release_gl) {
            bongo_cat_ui_paint_destroy(ui);
            bongo_cat_ui_resize_cache_destroy(ui);
        } else {
            bongo_cat_ui_paint_cache_abandon(ui);
        }
        if (ui->atlas.permanent.alloc) bongo_cat_ui_font_atlas_destroy(ui);
        nk_buffer_free(&ui->commands);
        nk_free(&ui->context);
        /* 剩下的登记纹理统一释放 (字体图集等) */
        for (int i = 0; i < BONGO_CAT_UI_VK_TEXTURES; ++i) {
            if (!ui->vk_textures[i]) continue;
            bongo_cat_vk_texture_destroy(ui->vk, ui->vk_textures[i]);
            ui->vk_textures[i] = NULL;
        }
        free(ui->vertices);
        free(ui->elements);
        memset(ui, 0, sizeof(*ui));
        return;
    }
    if (release_gl) {
        bongo_cat_ui_paint_destroy(ui);
        bongo_cat_ui_resize_cache_destroy(ui);
    } else {
        bongo_cat_ui_paint_cache_abandon(ui);
        ui->resize_cache_texture = 0;
        ui->resize_cache_width = ui->resize_cache_height = 0;
    }
    if (ui->atlas.permanent.alloc) bongo_cat_ui_font_atlas_destroy(ui);
    nk_buffer_free(&ui->commands);
    nk_free(&ui->context);
    if (release_gl) {
        if (ui->font_texture) glDeleteTextures(1, &ui->font_texture);
        if (ui->vbo) ui->gl.delete_buffers(1, &ui->vbo);
        if (ui->ebo) ui->gl.delete_buffers(1, &ui->ebo);
        if (ui->vao) ui->gl.delete_vertex_arrays(1, &ui->vao);
        if (ui->program) ui->gl.delete_program(ui->program);
    }
    free(ui->vertices);
    free(ui->elements);
    memset(ui, 0, sizeof(*ui));
}

void bongo_cat_ui_destroy(BongoCatUIBackend *ui) { release(ui, true); }

void bongo_cat_ui_abandon(BongoCatUIBackend *ui) { release(ui, false); }

static void convert(BongoCatUIBackend *ui) {
    static const struct nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(UIVertex, position)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(UIVertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(UIVertex, color)},
        {NK_VERTEX_LAYOUT_END}
    };
    struct nk_convert_config config = {0};
    config.vertex_layout = layout;
    config.vertex_size = sizeof(UIVertex);
    config.vertex_alignment = NK_ALIGNOF(UIVertex);
    config.tex_null = ui->null_texture;
    config.global_alpha = 1.0f;
    config.circle_segment_count = 36;
    config.curve_segment_count = 36;
    config.arc_segment_count = 36;
    config.shape_AA = NK_ANTI_ALIASING_ON;
    config.line_AA = NK_ANTI_ALIASING_ON;
    struct nk_buffer vertices, elements;
    nk_buffer_init_fixed(&vertices, ui->vertices, ui->vertex_capacity);
    nk_buffer_init_fixed(&elements, ui->elements, ui->element_capacity);
    ui->last_convert_result = nk_convert(&ui->context,
        &ui->commands, &vertices, &elements, &config);
    ui->last_vertex_bytes = vertices.allocated;
    ui->last_element_bytes = elements.allocated;
    if (vertices.allocated >= sizeof(UIVertex)) {
        const UIVertex *sample = ui->vertices;
        size_t count = vertices.allocated / sizeof(*sample);
        ui->nonzero_alpha_vertices = 0; ui->max_alpha = 0;
        for (size_t i = 0; i < count; ++i) {
            if (sample[i].color[3]) ui->nonzero_alpha_vertices++;
            if (sample[i].color[3] > ui->max_alpha) ui->max_alpha = sample[i].color[3];
        }
    }
}

/* OpenGL 路径把这一帧的顶点/索引上传到流式缓冲 */
static void upload_buffers_gl(BongoCatUIBackend *ui) {
    ui->gl.bind_buffer(GL_ARRAY_BUFFER, ui->vbo);
    ui->gl.buffer_data(GL_ARRAY_BUFFER, (GLsizeiptr)ui->last_vertex_bytes,
        ui->vertices, GL_STREAM_DRAW);
    ui->gl.bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ui->ebo);
    ui->gl.buffer_data(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ui->last_element_bytes,
        ui->elements, GL_STREAM_DRAW);
}

/*
 * Vulkan 路径: 顶点整块上传一次, 之后每条绘制命令只提交索引与裁剪矩形。
 * 顶点在缓冲里的下标与索引一致, 所以只要上传一次就能被所有命令引用。
 */
static void render_vulkan(BongoCatUIBackend *ui) {
    int pixel_width = 0, pixel_height = 0;
    float width = 0.0f, height = 0.0f;
    bongo_cat_ui_logical_size(ui, &width, &height);
    SDL_GetWindowSizeInPixels(ui->window, &pixel_width, &pixel_height);
    if (width <= 0.0f || height <= 0.0f || pixel_width <= 0 || pixel_height <= 0)
        return;
    convert(ui);
    float scale_x = pixel_width / width, scale_y = pixel_height / height;
    const size_t vertex_count = ui->last_vertex_bytes / sizeof(UIVertex);
    if (!bongo_cat_vk_ui_upload_vertices(ui->vk, ui->vertices,
            (unsigned int)vertex_count)) return;
    const struct nk_draw_command *command;
    /*
     * 索引的起点必须是元素缓冲本身。OpenGL 那边 offset 从 NULL 开始, 因为
     * glDrawElements 的最后一个参数是 "EBO 里的字节偏移"; Vulkan 这里 offset 是
     * 真的内存指针, 从 NULL 起步会直接崩在拷贝里 (MoveWithYMM 读 NULL+偏移)。
     */
    const nk_draw_index *offset = (const nk_draw_index *)ui->elements;
    ui->last_draw_commands = 0;
    ui->last_draw_elements = 0;
    nk_draw_foreach(command, &ui->context, &ui->commands) {
        if (!command->elem_count) continue;
        ui->last_draw_commands++;
        ui->last_draw_elements += command->elem_count;
        BongoCatVkTexture *texture =
            bongo_cat_ui_texture_lookup(ui, (int)command->texture.id);
        if (!texture) {
            offset += command->elem_count;
            continue;
        }
        bongo_cat_vk_set_scissor(ui->vk,
            (int)(command->clip_rect.x * scale_x),
            (int)(command->clip_rect.y * scale_y),
            (int)(command->clip_rect.w * scale_x),
            (int)(command->clip_rect.h * scale_y));
        bongo_cat_vk_ui_draw(ui->vk, texture, offset, command->elem_count,
            scale_x, scale_y);
        offset += command->elem_count;
    }
    nk_clear(&ui->context);
    nk_buffer_clear(&ui->commands);
    if (ui->last_draw_commands < 256) trim_commands(ui);
    ui->last_gl_error = GL_NO_ERROR;
}

void bongo_cat_ui_render(BongoCatUIBackend *ui) {
    if (bongo_cat_ui_uses_vulkan(ui)) {
        render_vulkan(ui);
        return;
    }
    int pixel_width, pixel_height;
    float width = 0.0f, height = 0.0f;
    bongo_cat_ui_logical_size(ui, &width, &height);
    SDL_GetWindowSizeInPixels(ui->window, &pixel_width, &pixel_height);
    float projection[4][4] = {{2.0f / width, 0, 0, 0}, {0, -2.0f / height, 0, 0},
        {0, 0, -1, 0}, {-1, 1, 0, 1}};
    bongo_cat_gl_clear_errors();
    convert(ui);
    upload_buffers_gl(ui);
    glViewport(0, 0, pixel_width, pixel_height);
    glEnable(GL_BLEND);
    ui->gl.blend_equation(GL_FUNC_ADD);
    ui->gl.blend_func_separate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    ui->gl.use_program(ui->program);
    ui->gl.uniform_1i(ui->texture_location, 0);
    ui->gl.uniform_matrix_4fv(ui->projection_location, 1, GL_FALSE, &projection[0][0]);
    ui->gl.active_texture(GL_TEXTURE0);
    ui->gl.bind_vertex_array(ui->vao);
    const struct nk_draw_command *command;
    const nk_draw_index *offset = NULL;
    float sx = pixel_width / width, sy = pixel_height / height;
    ui->last_draw_commands = 0;
    ui->last_draw_elements = 0;
    nk_draw_foreach(command, &ui->context, &ui->commands) {
        if (!command->elem_count) continue;
        ui->last_draw_commands++;
        ui->last_draw_elements += command->elem_count;
        glBindTexture(GL_TEXTURE_2D, (GLuint)command->texture.id);
        glScissor((GLint)(command->clip_rect.x * sx),
            (GLint)((height - command->clip_rect.y - command->clip_rect.h) * sy),
            (GLsizei)(command->clip_rect.w * sx), (GLsizei)(command->clip_rect.h * sy));
        glDrawElements(GL_TRIANGLES, (GLsizei)command->elem_count,
            sizeof(nk_draw_index) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, offset);
        offset += command->elem_count;
    }
    nk_clear(&ui->context);
    nk_buffer_clear(&ui->commands);
    /* Complex pages can grow this buffer substantially. Once a simple frame
       is back on screen, release that transient capacity without affecting
       the commands that were just presented. */
    if (ui->last_draw_commands < 256) trim_commands(ui);
    glDisable(GL_SCISSOR_TEST);
    ui->last_gl_error = glGetError();
}

void bongo_cat_ui_trim_idle(BongoCatUIBackend *ui) {
    if (!ui) return;
    bongo_cat_ui_paint_destroy(ui);
    trim_commands(ui);
}

bool bongo_cat_ui_frame_valid(const BongoCatUIBackend *ui) {
    return ui && ui->last_convert_result == NK_CONVERT_SUCCESS &&
        ui->last_vertex_bytes && ui->last_element_bytes &&
        ui->last_draw_commands && ui->last_draw_elements &&
        ui->nonzero_alpha_vertices && ui->max_alpha &&
        ui->font_probe_loaded &&
        ui->last_gl_error == GL_NO_ERROR;
}

BongoCatUIBackend *bongo_cat_ui_backend_for_context(
    const struct nk_context *context) {
    return context_backend && context == &context_backend->context
        ? context_backend : NULL;
}
