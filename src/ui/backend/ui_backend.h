/*
 * 设置界面的渲染后端。一个 BongoCatUIBackend 对应一个 SDL 窗口 + 一个 Nuklear
 * 上下文, 并且可能是两套完全不同的实现:
 *  - GL 路径 (vk == NULL): 自己建 program/vao/vbo/ebo, 纹理就是 GL 纹理名;
 *  - Vulkan 路径 (vk != NULL): 顶点与绘制命令交给 vk_backend, 纹理登记进
 *    vk_textures 表, Nuklear 拿到的 id 是表下标 + 1。
 * 两条路径共享同一套上层绘制代码, 所以下面每个字段都要问一句"另一条路径怎么办"。
 *
 * 生命周期: init() 失败会自己清理干净 (不会留下半个 backend); 拆的时候按上下文
 * 是否还有效区分 destroy() 与 abandon()。
 */
#ifndef BONGO_CAT_UI_BACKEND_H
#define BONGO_CAT_UI_BACKEND_H

#include "bongo_cat/gl_api.h"
#include "bongo_cat/vk_backend.h"
#include <SDL3/SDL.h>
#include "nuklear_config.h"
#include <stdint.h>

/*
 * 设置界面在 Vulkan 模式下使用的纹理表: Nuklear 的纹理 id = 下标 + 1。
 *
 * 别调小: 表满时 bongo_cat_ui_paint_cache_upload 会拿不到 id, item->texture 永远是 0,
 * ready() 就永远为假 —— 于是每一项 (包括那张全屏的 sidebar glow) 都变成"每帧重新
 * CPU 烘焙 + 重新上传", 设置界面直接掉到 20 FPS。常驻项就有字体图集/logo/图标图集/
 * 模型封面, 加上 paint cache 的渐变/阴影/圆角几十项, 64 曾经刚好不够。
 */
#define BONGO_CAT_UI_VK_TEXTURES 256

typedef enum BongoCatUICursor {
    BONGO_CAT_UI_CURSOR_DEFAULT,
    BONGO_CAT_UI_CURSOR_POINTER,
    BONGO_CAT_UI_CURSOR_TEXT,
    BONGO_CAT_UI_CURSOR_RESIZE_EW
} BongoCatUICursor;

typedef struct BongoCatUIBackend {
    SDL_Window *window;
    /* 非空表示这个界面由 Vulkan 渲染 (设置窗口的独立上下文) */
    BongoCatVk *vk;
    BongoCatVkTexture *vk_textures[BONGO_CAT_UI_VK_TEXTURES];
    /* 两套缩放, 别混用: layout 用于逻辑布局与输入坐标换算, raster 用于字体/图形烘焙 */
    float layout_scale;
    float raster_scale;
    BongoCatGL gl;
    struct nk_context context;
    struct nk_font_atlas atlas;
    struct nk_buffer commands;
    struct nk_draw_null_texture null_texture;
    GLuint program;
    GLint texture_location;
    GLint projection_location;
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    GLuint font_texture;
    GLuint resize_cache_texture;
    int resize_cache_width;
    int resize_cache_height;
    /* 实时缩放快路径的计数: 前者说明它确实生效, 后者说明这台机器上不可用 (自动退化) */
    unsigned resize_cache_presentations;
    unsigned resize_cache_failures;
    int font_atlas_width;
    int font_atlas_height;
    const struct nk_user_font *caption_font;
    const struct nk_user_font *body_font;
    const struct nk_user_font *label_font;
    const struct nk_user_font *heading_font;
    const struct nk_user_font *hero_font;
    nk_rune *latin_glyph_ranges;
    nk_rune *cjk_glyph_ranges;
    nk_rune *korean_glyph_ranges;
    void *vertices;
    void *elements;
    size_t vertex_capacity;
    size_t element_capacity;
    /* 上一帧的转换结果, 只在诊断与 frame_valid() 里读, 不参与渲染决策 */
    size_t last_vertex_bytes;
    size_t last_element_bytes;
    uint64_t paint_frame_marker;
    unsigned last_draw_commands;
    unsigned last_draw_elements;
    int last_convert_result;
    GLenum last_gl_error;
    unsigned nonzero_alpha_vertices;
    unsigned max_alpha;
    uint64_t last_left_click_ns;
    int last_left_click_x;
    int last_left_click_y;
    bool custom_font_loaded;
    bool font_probe_loaded;
    bool font_path_found;
    bool font_file_loaded;
    bool dark_theme;
    /* 实时缩放中: 为省时间跳过发光/阴影这类模糊效果 (见 ui_catime.c 的 shell_draw) */
    bool live_resize_fast;
    /* 正在构建某一帧。字体图集重建会检查它, 避免换掉本帧命令里已经引用的纹理 */
    bool frame_building;
    bool double_click_down;
    SDL_Cursor *default_cursor;
    SDL_Cursor *pointer_cursor;
    SDL_Cursor *text_cursor;
    SDL_Cursor *resize_ew_cursor;
    BongoCatUICursor requested_cursor;
} BongoCatUIBackend;

bool bongo_cat_ui_init(BongoCatUIBackend *ui, SDL_Window *window,
    BongoCatVk *vk,
    const char *body_font_path, const char *body_fallback_path,
    const char *body_korean_fallback_path, const char *heading_font_path,
    const char *heading_fallback_path,
    const char *heading_korean_fallback_path,
    const nk_rune *glyph_ranges, float layout_scale, float raster_scale,
    BongoCatError *error);
/* Vulkan 模式下界面纹理的登记与查找 (Nuklear 用返回的 id 引用它) */
int bongo_cat_ui_texture_register(BongoCatUIBackend *ui,
    BongoCatVkTexture *texture);
void bongo_cat_ui_texture_unregister(BongoCatUIBackend *ui, int id);
BongoCatVkTexture *bongo_cat_ui_texture_lookup(const BongoCatUIBackend *ui,
    int id);
/* 把 src 在 id 上的纹理转交给 dst 的同一个 id (字体图集重建时保持 id 不变) */
void bongo_cat_ui_texture_move(BongoCatUIBackend *dst, BongoCatUIBackend *src,
    int id);
/* 图片文件 -> 界面纹理, 返回 Nuklear 用的 id (0 表示失败) */
int bongo_cat_ui_texture_from_file(BongoCatUIBackend *ui, const char *path,
    int max_width, int max_height, int *width, int *height, BongoCatError *error);
void bongo_cat_ui_texture_release_id(BongoCatUIBackend *ui, int id);
bool bongo_cat_ui_uses_vulkan(const BongoCatUIBackend *ui);
void bongo_cat_ui_destroy(BongoCatUIBackend *ui);
void bongo_cat_ui_abandon(BongoCatUIBackend *ui);
void bongo_cat_ui_input_begin(BongoCatUIBackend *ui);
void bongo_cat_ui_input_end(BongoCatUIBackend *ui);
void bongo_cat_ui_input_reset(BongoCatUIBackend *ui);
bool bongo_cat_ui_event(BongoCatUIBackend *ui, const SDL_Event *event);
void bongo_cat_ui_render(BongoCatUIBackend *ui);
void bongo_cat_ui_trim_idle(BongoCatUIBackend *ui);
bool bongo_cat_ui_resize_cache_capture(BongoCatUIBackend *ui);
bool bongo_cat_ui_resize_cache_present(BongoCatUIBackend *ui);
void bongo_cat_ui_resize_cache_destroy(BongoCatUIBackend *ui);
bool bongo_cat_ui_frame_valid(const BongoCatUIBackend *ui);
BongoCatUIBackend *bongo_cat_ui_backend_for_context(
    const struct nk_context *context);
float bongo_cat_ui_display_layout_scale(SDL_DisplayID display);
void bongo_cat_ui_query_window_scale(SDL_Window *window,
    float *layout_scale, float *raster_scale);
/*
 * 窗口的三套尺寸口径一次取全。只给诊断/自检用, 不进每帧路径:
 *  - window_*: SDL 窗口坐标。Windows 的 DPI 感知窗口这里就是物理像素, macOS 是逻辑点;
 *  - pixel_*:  交换链 / 视口 / 裁剪矩形用的像素尺寸;
 *  - logical_*: 布局与输入坐标用的逻辑尺寸, 由 window_* / layout_scale 算出。
 * 高 DPI 下这三者会分成两组 (Windows 200%: 窗口=像素=564x493, 逻辑=282x247)。
 * 易改错: 把 SDL_GetWindowSize 直接当"逻辑尺寸"打印/使用, 在 Windows 上会得到
 * "像素 == 逻辑" 的假象 —— 自检报告里那条互相矛盾的数字就是这么来的。
 */
typedef struct BongoCatUIWindowMetrics {
    int window_width, window_height;
    int pixel_width, pixel_height;
    int logical_width, logical_height;
    float layout_scale, raster_scale;
} BongoCatUIWindowMetrics;

void bongo_cat_ui_window_metrics(SDL_Window *window,
    BongoCatUIWindowMetrics *metrics);
/* 逻辑尺寸换算的纯函数: 自检、日志与测试共用一条式子。layout_scale 非法按 1 算,
   结果至少为 1 (窗口最小化时 SDL 可能给出 0)。 */
void bongo_cat_ui_logical_from_window(int window_width, int window_height,
    float layout_scale, int *logical_width, int *logical_height);
void bongo_cat_ui_logical_size(const BongoCatUIBackend *ui,
    float *width, float *height);
/*
 * 诊断: 一行打全 "逻辑尺寸 / 像素尺寸 / 窗口尺寸 / 缩放因子"。窗口建立与缩放变化时
 * 各调用一次, 不进每帧循环。缩放值内部取自 bongo_cat_ui_query_window_scale, 与业务
 * 代码同源, 免得日志和实际用的值对不上。
 */
void bongo_cat_ui_log_scale(const char *tag, SDL_Window *window);
const struct nk_user_font *bongo_cat_ui_caption_font(
    const struct nk_context *context);
const struct nk_user_font *bongo_cat_ui_body_font(
    const struct nk_context *context);
const struct nk_user_font *bongo_cat_ui_label_font(
    const struct nk_context *context);
const struct nk_user_font *bongo_cat_ui_heading_font(
    const struct nk_context *context);
const struct nk_user_font *bongo_cat_ui_hero_font(
    const struct nk_context *context);
void bongo_cat_ui_cursor_begin(BongoCatUIBackend *ui);
void bongo_cat_ui_cursor_apply(BongoCatUIBackend *ui);
void bongo_cat_ui_cursor_destroy(BongoCatUIBackend *ui);
void bongo_cat_ui_cursor_reset(struct nk_context *context);
void bongo_cat_ui_cursor_hover_rect(struct nk_context *context,
    struct nk_rect bounds, BongoCatUICursor cursor);
void bongo_cat_ui_cursor_hover_widget(struct nk_context *context,
    BongoCatUICursor cursor);

#endif
