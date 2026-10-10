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
    /* LRU 时间戳: 来自下面 value->clock 的逻辑时钟, 值越大越新, 淘汰时挑最小者。
       0 表示这个槽位从没被用过 (也同时是"空槽"的判定依据之一)。 */
    uint64_t used;
} TextureSlot;

struct BongoCatOverlay {
    /*
     * GL 资源在 Vulkan 后端下全是 0 (create 直接早退, 见 overlay.c): 所有 GL 调用
     * 都必须以对象名非 0 为前提, 否则会调用空函数指针。
     * mirror/image/... _location 是 uniform 位置, 与 program 同生命周期 —— program
     * 只在 create 时编译一次且从不重建, 所以这些位置查询一次即可。
     */
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
    /* 非 Cubism 构建的合成预览图三件套:
       composed_cover = 整只猫都在 cover.png 里 (而不是只有底板 background.png);
       clean_paws    = 合成时要把 cover 上的左右手脚印擦掉 (画手时才不会露出两层);
       composite_dirty = 按键状态变过, 需要重新跑一次 CPU 合成 (见 overlay_draw.c)。 */
    bool composed_cover;
    bool clean_paws;
    bool composite_dirty;
    /* 模型自带指针优先: 为真时不装载也不绘制 Mver 指针层, 两者同时出现会画两套指针 */
    bool model_pointer_preferred;
    /* 叠加层参考尺寸, 0 = 不缩放 (直接铺满视口); 见 overlay.c 顶点着色器里的判断 */
    int reference_width;
    int reference_height;
    /* 按键/特效贴图的 LRU 缓存, 容量 4: 左右手 + 特效 + 一个备用。槽位被 left/right/
       effect 引用时不会被淘汰 (指针别名, 见下), 四个全被钉住时新贴图取不到, 表现为
       该层静默不画 —— 这是刻意的降级, 不是错误。 */
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
    /* 当前模型目录: 按键/特效贴图的相对路径都以它为基准, 由 overlay_load 写入 */
    char directory[BONGO_CAT_PATH_CAP];
    /* LRU 用的逻辑时钟, 每次查询自增。刻意不用真实时间或帧号: 暂停/掉帧/时钟回绕
       都不该影响淘汰顺序。 */
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
