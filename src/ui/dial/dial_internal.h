/*
 * 轮盘 (径向菜单) 的内部定义。
 *
 * 全局设计要点, 改这里之前先读:
 *   1. 轮盘**不是**接入主循环的一套 UI, 而是一个自带模态循环的独立窗口
 *      (bongo_cat_platform_context_menu 里的 while)。所以它自己 pump 事件、自己
 *      控帧率 (16ms 预算)、退出时自己收拾全部资源;
 *   2. 它共享宠物窗口的 Vulkan 设备 (只建自己的交换链)。第二条设备在装了 RTSS /
 *      OBS 注入钩子的机器上第一帧呈现就崩 —— 与设置窗口同一个坑, 见 dial_window.c
 *      create() 里的注释;
 *   3. 纹理句柄是后端无关的: GL 下是 GL 纹理名, Vulkan 下是下面 dial_vk_texture_*
 *      那张表的 id (id = 下标 + 1, 0 表示无)。任何要交给后端的纹理都必须先经
 *      dial_vk_texture_lookup 换一次, 不能把 id 直接当句柄用;
 *   4. 所有几何都先算成 DialPath (扇形折线), 再经 dial_triangle 送进一条统一的顶点
 *      流, 由 DialPaint 按批次一次提交 —— 菜单里所有东西 (扇形、文字、图标、封面)
 *      共用同一条管线, 没有第二个 draw call 路径。
 */
#ifndef BONGO_CAT_DIAL_INTERNAL_H
#define BONGO_CAT_DIAL_INTERNAL_H
#include "bongo_cat/platform.h"
#include "bongo_cat/gl_api.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <math.h>
/*
 * 几何/动画常量。DIAL_PAGE 是子项分页上限, DIAL_ROOTS 是根项上限;
 * 两个 REVEAL_* 配合使用: 每一项比前一项晚 22ms 开始、各自用 360ms 展开 (级联效果);
 * _POINTS/SEGMENTS/RINGS 是各类折线/圆环的离散段数, 它们同时决定固定数组的大小,
 * 调大就会同时增加内存与顶点数。
 */
#define DIAL_PI 3.14159265358979323846f
#define DIAL_PAGE 16
#define DIAL_ROOTS 12
#define DIAL_REVEAL_DELAY_MS 22
#define DIAL_REVEAL_DURATION_MS 360
/* 连续多少帧画不出来就认定这个菜单窗口端不出去了 (Vulkan 侧据此回退 OpenGL)。
   取 3 而不是 1: 交换链过期/重建失败常常只影响一两帧, 一失败就换后端会把偶发抖动
   放大成整窗重建。 */
#define DIAL_PAINT_FAILURE_LIMIT 3
#define DIAL_PATH_POINTS 160
#define DIAL_CENTER_SEGMENTS 64
#define DIAL_CENTER_RINGS 16
typedef struct DialPoint { float x, y; } DialPoint;
typedef struct DialPath {
    DialPoint points[DIAL_PATH_POINTS];
    unsigned short triangles[DIAL_PATH_POINTS * 3];
    int count, triangle_count, rim_start;
    float min_x, min_y, max_x, max_y;
} DialPath;
typedef struct DialItem {
    const char *label;
    BongoCatMenuAction command;
    uint32_t color;
    int icon;
    bool checked;
    size_t children;
} DialItem;
/*
 * 顶点/批次都是预分配的 (没有动态增长): 菜单的规模有上界, 预分配换来了绘制期间
 * 零分配, 也就不会有分配失败路径。
 */
typedef struct DialVertex { float x, y, u, v; uint8_t r, g, b, a; } DialVertex;
/* 纹理句柄与后端无关: GL 下是 GL 纹理名, Vulkan 下是界面纹理表的 id */
typedef struct DialBatch { unsigned int texture; int first, count; } DialBatch;
typedef struct DialCover { unsigned int texture; float width, height; bool attempted; } DialCover;
typedef struct DialPaint {
    BongoCatGL gl;
    GLuint program, vao, vbo, white, font_texture;
    /*
     * Vulkan 菜单窗口的纹理句柄表: 批/封面里存的是 id, 画的时候要换成
     * bongo_cat_vk_draw_triangles 需要的纹理指针。id = 下标 + 1。
     * 与设置窗口里 BongoCatUIBackend.vk_textures[] 同一套做法。
     */
    /*
     * Vulkan 菜单窗口的纹理句柄表: 批/封面里存的是 id, 画的时候要换成
     * bongo_cat_vk_draw_triangles 需要的纹理指针。id = 下标 + 1。
     * 与设置窗口里 BongoCatUIBackend.vk_textures[] 同一套做法。
     */
    struct BongoCatVkTexture *vk_textures[8];
    GLint projection, sampler;
    DialVertex *vertices;
    size_t count, capacity;
    DialBatch batches[512];
    int batch_count;
    struct nk_font_atlas atlas;
    struct nk_font *font;
    nk_rune *ranges[4];
    DialPath roots[DIAL_ROOTS], children[DIAL_PAGE];
    int child_path_count, child_path_active, child_path_roots;
    float child_path_step;
    float zoom, offset_x, offset_y, alpha;
    DialPoint center_points[DIAL_CENTER_RINGS + 1][DIAL_CENTER_SEGMENTS];
    uint8_t center_alpha[DIAL_CENTER_RINGS + 1];
    bool center_ready;
    bool failed;
} DialPaint;
/*
 * 轮盘的全部状态。生命周期只有一个: bongo_cat_platform_context_menu 里 calloc 出来,
 * 循环结束后 free —— 没有"重建窗口"这条路, 每次开菜单都是全新一份。
 *
 * previous_window/previous_context 记录"打开菜单之前谁的 GL 上下文是当前的": 预览
 * 回调要把画面画到宠物窗口上, 所以调它之前必须先把上下文切回去, 之后还要切回来。
 * Vulkan 下这两个字段没有意义 (不切上下文), 相关代码都要用 if (!d->vk) 保护。
 *
 * dirty 是这套渲染的开关: 任何会改变画面的东西 (动画步进、输入、封面加载完成) 都要
 * 把它置真; 模态循环只在 dirty 时真正画一帧。漏标的表现就是菜单卡住不动。
 */
typedef struct Dial {
    SDL_Window *window, *owner;
    SDL_GLContext context, previous_context;
    /*
     * 菜单窗口自己的 Vulkan 上下文 (renderer=vulkan 时非空, 否则为 NULL 走 GL)。
     * 用前置声明避免这个头文件依赖 vk_backend.h。
     * 施工图: docs/vulkan-dial-port-recipe.md
     */
    /*
     * 菜单窗口自己的 Vulkan 上下文 (renderer=vulkan 时非空, 否则为 NULL 走 GL)。
     * 用前置声明避免这个头文件依赖 vk_backend.h。
     * 施工图: docs/vulkan-dial-port-recipe.md
     */
    struct BongoCatVk *vk;
    /*
     * 菜单窗口自己的分层呈现器实例 (仅 Windows + GL 渲染 + 开了
     * BONGO_CAT_GL_TRANSPARENCY 时非空, 否则 NULL)。类型对上层不透明, 见
     * src/platform/windows/windows_layered.h —— 这里的 void * 让本文件不必包含
     * 平台头文件, 非 Windows 平台这个字段永远是 NULL。
     */
    void *layered;
    /*
     * window.glTransparency 的快照 (来自 platform->gl_transparency, 见 platform.h)。
     * 菜单在打开的那一刻据此决定要不要挂代理呈现器 —— 菜单是模态的, 打开期间用户
     * 改不了设置, 所以一份快照就够, 不必每帧回查。
     */
    bool gl_transparency;
    /*
     * 强制这个菜单窗口用 OpenGL。由"上一次用 Vulkan 开菜单失败"置位 (建窗/首帧/
     * 连续提交失败), 见 dial_window.c 的 context_menu: 窗口的图形 API 标志在创建时
     * 就定死了, 所以回退只能整窗重建。
     */
    bool force_opengl;
    /*
     * 这一次建窗**尝试过** Vulkan。与 force_opengl/vk 分开记: 上下文创建失败时
     * d->vk 还是 NULL, 只看 d->vk 会把"试过但失败"误判成"本来就没走 Vulkan",
     * 于是 OpenGL 回退永远不会触发 (dial_loop.c 的 menu_run 就靠这个字段)。
     */
    bool vk_attempted;
    SDL_Window *previous_window;
    SDL_WindowID window_id;
    const BongoCatMenuLabels *labels;
    DialItem items[DIAL_ROOTS];
    int count, active, child, page, pressed;
    bool done, dark, dirty, child_focus, shown, popup;
    /*
     * "已经忽略过一条假的 MOUSE_LEAVE" 的记账位 (只在代理分层模式下会置位)。
     * 代理模式下这条事件跟着每一条真实的鼠标移动来, 光标不停就一直在来 —— 不能每次
     * 都写日志, 否则诊断日志自己就变成新的风暴。见 dial_input.c 的
     * pointer_inside_window。
     */
    bool leave_filtered;
    /*
     * 帧率诊断: frames 是模态循环的迭代次数, presents 是真正提交出去的帧数。
     * 代理分层呈现下两者会明显分离 —— 静止的菜单一次都不回读(见 dial_paint_frame),
     * 这正是"别把 GL 拖成幻灯片"的关键, 出问题时靠这两个数分辨。
     */
    int frames, presents;
    /*
     * 连续绘制失败次数 (含 Vulkan 的 acquire/present 失败)。达到上限就退出模态循环,
     * 而不是每 16ms 重试一次坏掉的交换链 —— 后者会让用户看着一个卡死的菜单却不知道
     * 出了什么事。
     */
    int paint_failures;
    uint64_t input_after_ns;
    BongoCatMenuAction result, preview;
    int width, height, pixel_width, pixel_height;
    float scale, opening, lift[DIAL_ROOTS], raster_scale;
    uint64_t opened_at, changed_at, animation_at;
    SDL_Event events[128];
    /*
     * 本帧从 SDL 队列里"截"下来的自有事件 (见 dial_window.c 的 collect_event):
     * 先攒起来再处理, 是为了在事件回调里不重入模态循环的状态机。
     */
    int event_count;
    DialPaint paint;
    DialCover covers[BONGO_CAT_MODEL_CAP];
} Dial;
/*
 * 内部接口清单, 按调用顺序大致排列: 建表(items) -> 几何(hit/child_paths/sector) ->
 * 渲染(paint_ 系列: scene/quad/stroke/icon/text) -> 封面 -> 事件 -> 预览回调。
 * 其中 dial_transform 修改的是"绘制用的"zoom/offset (不改布局), dial_hit 则完全在
 * 逻辑坐标里判定 —— 两者必须用同一套布局公式, 改一边就要改另一边。
 */
void dial_items(Dial *d);
/* 菜单窗口的建立 (dial_window.c) 与模态循环 (dial_loop.c) 之间的接口 */
bool dial_window_create(Dial *d);
bool SDLCALL dial_collect_event(void *userdata, SDL_Event *event);
int dial_child_count(const Dial *d);
float dial_child_angle(const Dial *d, int index);
float dial_child_step(const Dial *d);
DialItem dial_child_item(const Dial *d, int index, char *text, size_t capacity);
void dial_select(Dial *d, int root, int child);
void dial_hit(Dial *d, float x, float y, int *root, int *child);
void dial_child_paths(Dial *d);
void dial_sector(DialPath *path, float inner, float outer, float start, float end);
void dial_transform(Dial *d, float zoom, float x, float y);
bool dial_paint_init(Dial *d);
void dial_paint_free(Dial *d);
bool dial_paint_frame(Dial *d);
void dial_scene(Dial *d);
void dial_triangle(Dial *d, DialPoint a, DialPoint b, DialPoint c,
    uint32_t ca, uint32_t cb, uint32_t cc);
/* Vulkan 纹理句柄表 (id = 下标 + 1, 0 表示无) */
int dial_vk_texture_register(Dial *d, struct BongoCatVkTexture *texture);
struct BongoCatVkTexture *dial_vk_texture_lookup(const Dial *d, unsigned int id);
void dial_vk_texture_release(Dial *d, unsigned int id);

void dial_quad(Dial *d, unsigned int texture, float x, float y, float w, float h,
    float u0, float v0, float u1, float v1, uint32_t color);
void dial_stroke(Dial *d, const DialPoint *points, int count, float width,
    uint32_t color, bool smooth);
void dial_dot(Dial *d, float x, float y, float radius, uint32_t color);
void dial_icon(Dial *d, int icon, float x, float y, float size, uint32_t color);
bool dial_fonts_init(Dial *d);
void dial_text(Dial *d, const char *text, float x, float y, float width,
    float size, uint32_t color);
void dial_covers_tick(Dial *d);
bool dial_cover_draw(Dial *d, int child, float x, float y, float opacity);
void dial_event(Dial *d, const SDL_Event *event);
void dial_preview(Dial *d, BongoCatMenuAction action);
#endif
