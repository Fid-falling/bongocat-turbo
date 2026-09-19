#ifndef BONGO_CAT_OVERLAY_INTERNAL_H
#define BONGO_CAT_OVERLAY_INTERNAL_H

#include "bongo_cat/overlay.h"
#include "bongo_cat/gl_api.h"
#include "bongo_cat/render.h"
#include "mver_pointer_overlay.h"
#include "render_texture.h"

#include <SDL3/SDL_opengl.h>

typedef struct TextureSlot {
    char path[BONGO_CAT_PATH_CAP];
    /* 后端无关句柄: OpenGL 纹理名或 Vulkan 纹理对象 (Vulkan 后端没有 GL 上下文) */
    BongoCatTexture texture;
    uint64_t used;
} TextureSlot;

struct BongoCatOverlay {
    BongoCatGL gl;
    BongoCatMverPointerOverlay *mver_pointer;
    GLuint program;
    GLint mirror_location;
    GLint image_location;
    GLint reference_width_location;
    GLint reference_height_location;
    GLint erase_left_location;
    GLint erase_right_location;
    GLuint vao;
    GLuint vbo;
    /* 主窗口叠加层贴图: 后端无关句柄, OpenGL/Vulkan 各用各的一份 */
    BongoCatTexture background;
    BongoCatTexture composite;
    bool composed_cover;
    bool clean_paws;
    bool composite_dirty;
    bool model_pointer_preferred;
    int reference_width;
    int reference_height;
    TextureSlot cache[4];
    /*
     * 按键/特效贴图指向 cache 里的槽位 (不拷贝句柄): 拷贝的话, 槽位被复用并
     * 释放纹理之后, 拷贝体就成了野指针。NULL 表示当前没有这张贴图。
     * 之前这里是 GLuint, 在 Vulkan 后端下画不出来 —— 那些 id 不是 GL 纹理名,
     * 而且根本没有 GL 上下文可以创建它们。
     */
    BongoCatTexture *left;
    BongoCatTexture *right;
    BongoCatTexture *effect;
    char left_name[BONGO_CAT_ID_CAP];
    char right_name[BONGO_CAT_ID_CAP];
    char background_path[BONGO_CAT_PATH_CAP];
    char left_path[BONGO_CAT_PATH_CAP];
    char right_path[BONGO_CAT_PATH_CAP];
    char effect_path[BONGO_CAT_PATH_CAP];
    char directory[BONGO_CAT_PATH_CAP];
    uint64_t clock;
};

/* Shared texture ownership for model loading and input/effect activation. */
void bongo_cat_overlay_clear_textures(BongoCatOverlay *value);
/* 合成背景 (基图 + 左右按键图), 供叠加层绘制前调用 */
void bongo_cat_overlay_refresh_composite(BongoCatOverlay *value);
#ifdef BONGO_CAT_HAS_CUBISM
BongoCatTexture *bongo_cat_overlay_cached_texture(BongoCatOverlay *value,
    const char *path);
#endif

#endif
