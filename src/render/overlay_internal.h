#ifndef BONGO_CAT_OVERLAY_INTERNAL_H
#define BONGO_CAT_OVERLAY_INTERNAL_H

#include "bongo_cat/overlay.h"
#include "bongo_cat/config.h"
#include "bongo_cat/gl_api.h"
#include "bongo_cat/image.h"
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

/* 每侧最多同时显示的按键数量, 与设置里的 multi_key_limit 上限是同一个数 */
#define BONGO_CAT_OVERLAY_KEYS_CAP BONGO_CAT_MULTI_KEY_LIMIT_MAX
/*
 * 每侧最多"记住"的按住键数量。超出的只记名字、不装载贴图: 松开前面的键之后, 后面的
 * 键会补位到显示窗口里再装载 (与 BongoCat-X 的"超出显示数量的按键保留按住状态"一致)。
 * 16 远超单手的物理按键数, 只是防止手键枚举把数组写穿。
 */
#define BONGO_CAT_OVERLAY_HELD_CAP 16
/*
 * 按键/特效贴图的 LRU 容量。上限 = 两侧各 KEYS_CAP 个键 + 一张特效, 再留几个空槽: 槽位
 * 被引用时不可淘汰 (下文的"钉住"规则), 容量必须**严格大于**最大同时在用的张数, 否则
 * 新键取不到贴图会静默不画 —— 那是刻意的降级, 但不该在常规用法下触发。
 */
#define BONGO_CAT_OVERLAY_CACHE_CAP (BONGO_CAT_OVERLAY_KEYS_CAP * 2 + 4)

/*
 * 一个"按住中"的键。texture 是指向 cache 槽位内部的句柄 (不拷贝, 理由见下面 cache 的
 * 注释), NULL 表示这个键当前没有贴图: 要么超出显示数量, 要么名字对不上任何按键文件。
 */
typedef struct OverlayKey {
    char name[BONGO_CAT_ID_CAP];
    BongoCatTexture *texture;
} OverlayKey;

/* 一只手: 按住中的键, 顺序即按下顺序。keys[0] 永远是"最早按下的那个", 也是唯一画出
   手部姿势的那个键 (见 overlay_draw.c 的分层说明)。 */
typedef struct OverlaySide {
    OverlayKey keys[BONGO_CAT_OVERLAY_HELD_CAP];
    size_t count;
} OverlaySide;

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
    /* 1 = 只保留"按键高亮"像素 (多键显示里非首个按键走这条通道), 见 overlay.c 的片元着色器 */
    GLint keycap_location;

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
    /* 按键/特效贴图的 LRU 缓存。槽位被下面的 sides/effect 引用时不会被淘汰 (指针别名,
       见下), 全被钉住时新贴图取不到, 表现为该层静默不画 —— 这是刻意的降级, 不是错误。 */
    TextureSlot cache[BONGO_CAT_OVERLAY_CACHE_CAP];
    /*
     * 两只手按住中的按键。[0] = 左手, [1] = 右手。贴图句柄指向 cache 里的槽位 (不拷贝
     * 句柄): 拷贝的话, 槽位被复用并释放纹理之后, 拷贝体就成了野指针。
     * 之前这里是 left/right 两个单槽, 每按一次键就覆盖 —— 同侧多键只能看到最后一个,
     * 这就是"气泡只能显示一个按键"的根因。改成有序列表之后按键按顺序保留。
     */
    OverlaySide sides[2];
    /* 多键显示开关与每侧显示上限, 来自设置 (bongo_cat_overlay_set_key_display 写入)。
       关掉时按 1 处理: 只画每侧最早按下的那个键, 即旧行为。 */
    bool multi_key;
    int key_limit;
    /* 1 = 同侧多键合并成一个气泡 ("A + S"), 0 = 每个键各占一个气泡并排。来自设置
       (bongo_cat_overlay_set_bubble_merge 写入), 只对有气泡素材的模型有效。 */
    bool bubble_merge;
    /*
     * 每侧"模型自带按键气泡素材"的 uv 矩形 (x0,y0,x1,y1 归一化; x1<=x0 = 没有气泡素材)。
     * BongoCat-X 的模型按键图是"气泡 + 手"的整图, 气泡在各键图上画在同一位置、只有里面的
     * 字母随键变化 —— 同侧多键时它既是"文字贴片的来源", 也是下面单气泡合成的落点。
     * 探测一次就缓存 (state: 0=未探测, 1=有, 2=确认没有), 换模型时由 clear_textures 清空。
     * 见 overlay_assets.c 的 bongo_cat_overlay_side_bubble。
     */
    float bubble_rect[2][4];
    unsigned char bubble_state[2];
    /*
     * 每侧"单气泡"合成图 (只有自带气泡素材的模型用得上): 同侧多键时在 CPU 侧把各键的键名
     * 拼成 "A + S" 重画进同一个气泡, 整张图 (手 + 气泡) 当这一侧**唯一**的图层画出去 ——
     * 用户看到的是一个气泡, 而不是并排的 N 个。dirty 表示这一侧的键集变过、下一帧要重拼:
     * 拼一次要解码最多 key_limit 张整图, 不能每帧做。见 overlay_assets.c 的 refresh_bubble。
     */
    BongoCatTexture bubble_composite[2];
    bool bubble_dirty[2];

    /* 特效贴图 (走同一个 cache) */
    BongoCatTexture *effect;
    char background_path[BONGO_CAT_PATH_CAP];
    /* 每侧首个(显示中的)按键贴图路径: 只有非 Cubism 构建用它跑 CPU 合成 (那张合成图只
       能吃左右各一张), 所以多键在非 Cubism 构建里退化为单键显示。 */
    char left_path[BONGO_CAT_PATH_CAP];
    char right_path[BONGO_CAT_PATH_CAP];
    char effect_path[BONGO_CAT_PATH_CAP];
    /* 当前模型目录: 按键/特效贴图的相对路径都以它为基准, 由 overlay_load 写入 */
    char directory[BONGO_CAT_PATH_CAP];
    /* LRU 用的逻辑时钟, 每次查询自增。刻意不用真实时间或帧号: 暂停/掉帧/时钟回绕
       都不该影响淘汰顺序。 */
    uint64_t clock;
};

/*
 * 一枚"键名贴片": 从模型自带按键图的气泡里抠出来的那个键名 (或程序画的 "+")。文字来自
 * 模型自己的美术, 所以合并模式 ("A + S") 排出来的字与单键时同字同色同字号。
 * pixels 是 left,top 处的 width×height 四通道小图, 只有文字处不透明。
 */
typedef struct BubbleGlyph {
    unsigned char *pixels;
    int width, height;
    int left, top;
    unsigned char rgb[3];   /* 文字主体色 (画 "+" 时用它) */
} BubbleGlyph;

/* 掩码/贴片的实现都在 overlay_bubble_glyph.c, 排版与绘制在 overlay_bubble.c */
void bongo_cat_bubble_rect_pixels(const BongoCatImage *image, const float *rect,
    int bounds[4]);
/* 气泡合成/拉宽的调试日志: 走全局 SDL 日志 (DEBUG 级别, 见 overlay_bubble.c) */
void bongo_cat_bubble_debug(const char *format, ...);
void bongo_cat_bubble_glyph_free(BubbleGlyph *glyph);
/* 在这张键图的气泡矩形内抠出键名贴片; 图里没有文字时返回 false */
bool bongo_cat_bubble_glyph_from_key(const BongoCatImage *image, const float *rect,
    BubbleGlyph *glyph);
/* 底图那枚键名的贴片 + 把它的键名从气泡里抹掉 (掩码只建一次, 两处共用) */
int bongo_cat_bubble_first_glyph(BongoCatImage *base, const float *rect,
    BubbleGlyph *glyphs);
/* 程序画的 "+": 颜色取自模型自己的键名文字色 */
bool bongo_cat_bubble_glyph_plus(BubbleGlyph *glyph, int size, int thickness,
    const unsigned char rgb[3]);

/* Shared texture ownership for model loading and input/effect activation. */
void bongo_cat_overlay_clear_textures(BongoCatOverlay *value);
/* 合成背景 (基图 + 左右按键图), 供叠加层绘制前调用 */
void bongo_cat_overlay_refresh_composite(BongoCatOverlay *value);
/*
 * 重新装载某一侧"要显示的"那些键的贴图 (超出的只留名字, 松开前面的键之后它们会补位
 * 进来再装载), 并同步 left_path/right_path。按下/抬起/换显示上限之后都要调一次。
 * side: 0 = 左手, 1 = 右手。
 */
void bongo_cat_overlay_refresh_keys(BongoCatOverlay *value, int side);
/* 该侧这一帧最多画几个键 (关闭多键显示或非 Cubism 构建时恒为 1)。 */
int bongo_cat_overlay_display_limit(const BongoCatOverlay *value);
/*
 * 该侧按键图里是否自带按键气泡素材, 命中时把 uv 矩形写进 value->bubble_rect[side]。
 * 结果缓存, 换模型后失效; 只有在"还没探测过"时才会去读一张按键图。
 */
bool bongo_cat_overlay_side_bubble(BongoCatOverlay *value, int side);
/* 某一侧某个按键的贴图路径 (与 overlay_input.c 里按键归属判定用的是同一套查找规则) */
bool bongo_cat_overlay_key_path(const BongoCatOverlay *value, int side,
    const char *name, char path[BONGO_CAT_PATH_CAP]);
#ifdef BONGO_CAT_HAS_CUBISM
/*
 * 该侧"单气泡"合成图是否可用 (需要时按 dirty 重建)。只有自带气泡素材且同侧多键时才可能
 * 为真; 其余情形返回 false, 调用方退回"首个键整图 + 其余键帽"的老画法。
 */
bool bongo_cat_overlay_refresh_bubble(BongoCatOverlay *value, int side);
BongoCatTexture *bongo_cat_overlay_cached_texture(BongoCatOverlay *value,
    const char *path);
#endif

#endif
