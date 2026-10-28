#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <string.h>

/*
 * 未启用 Vulkan 时的桩实现。
 *
 * 调用方 (窗口创建、叠加层、模型加载、命中测试) 因此不需要到处写 #ifdef
 * BONGO_CAT_HAS_VULKAN: 这条后端在不可用时只是老老实实地返回失败/空指针。
 */

/*
 * 维护这个文件的两条硬约束:
 *  1. 必须覆盖 bongo_cat/vk_backend.h 里**每一个**外部符号。少一个就是"关闭 Vulkan
 *     的构建直接链接失败", 而这套桩代码平时不会被 CI 之外的人注意到 —— 新增对外
 *     接口时先在这里补桩, 再去写真正的实现。
 *  2. 返回值必须是"安全失败": 句柄给 NULL、判断给 false、尺寸给 0、销毁/设置类是空
 *     操作。调用方普遍写成 if (vk) {...} / if (!bongo_cat_vk_xxx(...)) 回退, 所以绝不能
 *     在桩里断言或写坏调用方的指针 —— 桩存在的意义就是让这条路可诊断地降级。
 *
 * 报错策略: 会在**运行时真的尝试用这条后端**的入口 (probe/create/纹理上传) 才写
 * BongoCatError, 文案里带上启用开关的名字, 方便用户照着改构建参数; 而
 * create_shared / texture_from_external 不填 error, 只靠返回值表示不可用。
 */

#ifndef BONGO_CAT_HAS_VULKAN

/* 恒为 false: 设置界面据此把 Vulkan 选项显示成不可用, 而不是让用户选了之后静默回退 */
bool bongo_cat_vk_compiled(void) { return false; }

/* 没有活跃上下文: 所有"拿当前上下文"的调用点都会因此走 OpenGL 分支 */
BongoCatVk *bongo_cat_vk_current(void) { return NULL; }

bool bongo_cat_vk_probe(BongoCatError *error) {
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
    return false;
}

BongoCatVk *bongo_cat_vk_create(SDL_Window *window, BongoCatError *error) {
    (void)window;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
    return NULL;
}

void bongo_cat_vk_destroy(BongoCatVk *vk) { (void)vk; }

BongoCatVk *bongo_cat_vk_create_shared(SDL_Window *window, const BongoCatVk *owner,
    BongoCatError *error) {
    (void)window; (void)owner; (void)error;
    return NULL;
}

bool bongo_cat_vk_info(const BongoCatVk *vk, BongoCatVkInfo *info) {
    (void)vk; (void)info;
    return false;
}

bool bongo_cat_vk_swapchain_info(const BongoCatVk *vk,
    BongoCatVkSwapchainInfo *info) {
    (void)vk; (void)info;
    return false;
}

bool bongo_cat_vk_resize(BongoCatVk *vk, int width, int height) {
    (void)vk; (void)width; (void)height;
    return false;
}

bool bongo_cat_vk_begin_frame(BongoCatVk *vk, int width, int height) {
    (void)vk; (void)width; (void)height;
    return false;
}

bool bongo_cat_vk_end_frame(BongoCatVk *vk) { (void)vk; return false; }
bool bongo_cat_vk_frame_active(const BongoCatVk *vk) { (void)vk; return false; }

void bongo_cat_vk_set_clear_color(BongoCatVk *vk, float red, float green,
    float blue, float alpha) {
    (void)vk; (void)red; (void)green; (void)blue; (void)alpha;
}

BongoCatVkTexture *bongo_cat_vk_texture_from_pixels(BongoCatVk *vk,
    const unsigned char *rgba, int width, int height, const char *label,
    BongoCatError *error) {
    (void)vk; (void)rgba; (void)width; (void)height; (void)label;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
    return NULL;
}

BongoCatVkTexture *bongo_cat_vk_texture_load(BongoCatVk *vk, const char *path,
    int *width, int *height, BongoCatError *error) {
    (void)vk; (void)path; (void)width; (void)height;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
    return NULL;
}

void bongo_cat_vk_texture_destroy(BongoCatVk *vk, BongoCatVkTexture *texture) {
    (void)vk; (void)texture;
}

int bongo_cat_vk_texture_width(const BongoCatVkTexture *texture) {
    (void)texture;
    return 0;
}

int bongo_cat_vk_texture_height(const BongoCatVkTexture *texture) {
    (void)texture;
    return 0;
}

bool bongo_cat_vk_draw_overlay_quad(BongoCatVk *vk, BongoCatVkTexture *texture,
    bool mirror, int reference_width, int reference_height, bool erase_left,
    bool erase_right, bool premultiply, bool keycap_only) {
    (void)vk; (void)texture; (void)mirror; (void)reference_width;
    (void)reference_height; (void)erase_left; (void)erase_right;
    (void)premultiply; (void)keycap_only;
    return false;
}

bool bongo_cat_vk_draw_triangles(BongoCatVk *vk, BongoCatVkTexture *texture,
    const float *vertices, unsigned int vertex_count, bool textured) {
    (void)vk; (void)texture; (void)vertices; (void)vertex_count; (void)textured;
    return false;
}

bool bongo_cat_vk_draw_dial(BongoCatVk *vk, BongoCatVkTexture *texture,
    const void *vertices, unsigned int vertex_count, int width, int height) {
    (void)vk; (void)texture; (void)vertices; (void)vertex_count; (void)width;
    (void)height;
    return false;
}

bool bongo_cat_vk_mask_corners(BongoCatVk *vk, int width, int height,
    float radius_percent) {
    (void)vk; (void)width; (void)height; (void)radius_percent;
    return false;
}

void bongo_cat_vk_set_viewport(BongoCatVk *vk, int x, int y, int width,
    int height) {
    (void)vk; (void)x; (void)y; (void)width; (void)height;
}

void bongo_cat_vk_set_vsync(BongoCatVk *vk, bool vsync) {
    (void)vk; (void)vsync;
}

bool bongo_cat_vk_queue_hit_sample(BongoCatVk *vk, int x, int y) {
    (void)vk; (void)x; (void)y;
    return false;
}

bool bongo_cat_vk_queue_frame_sample(BongoCatVk *vk, int x, int y, int w, int h) {
    (void)vk; (void)x; (void)y; (void)w; (void)h;
    return false;
}

bool bongo_cat_vk_take_frame_sample(BongoCatVk *vk, int *w, int *h,
    const unsigned char **pixels) {
    (void)vk; (void)w; (void)h; (void)pixels;
    return false;
}

void bongo_cat_vk_release_cached_frames(BongoCatVk *vk) {
    (void)vk;
}

void bongo_cat_vk_discard_pending_frame_sample(BongoCatVk *vk) {
    (void)vk;
}

/* 命中测试与整帧回读在这里恒为失败: 上层据此回退到几何判断/跳过诊断输出, 而不是
   以为"回读结果还没准备好"而一直等下去 */
bool bongo_cat_vk_hit_alpha(const BongoCatVk *vk, int x, int y,
    unsigned char *alpha) {
    (void)vk; (void)x; (void)y; (void)alpha;
    return false;
}

BongoCatVkTexture *bongo_cat_vk_texture_from_alpha(BongoCatVk *vk,
    const unsigned char *alpha, int width, int height, const char *label,
    BongoCatError *error) {
    (void)vk; (void)alpha; (void)width; (void)height; (void)label;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
    return NULL;
}

void bongo_cat_vk_set_scissor(BongoCatVk *vk, int x, int y, int width, int height) {
    (void)vk; (void)x; (void)y; (void)width; (void)height;
}

bool bongo_cat_vk_ui_upload_vertices(BongoCatVk *vk, const void *vertices,
    unsigned int vertex_count) {
    (void)vk; (void)vertices; (void)vertex_count;
    return false;
}

bool bongo_cat_vk_ui_draw(BongoCatVk *vk, BongoCatVkTexture *texture,
    const void *indices, unsigned int index_count, float scale_x, float scale_y) {
    (void)vk; (void)texture; (void)indices; (void)index_count;
    (void)scale_x; (void)scale_y;
    return false;
}

/* 返回 0 在真实路径里观察不到: 两个调用点 (字体图集、轮盘字体) 都以 vk 句柄非空为
   前提, 而本构建下句柄恒为 NULL。保留 0 只是为了让"没上下文"有个明确答案。 */
int bongo_cat_vk_max_texture_dimension(const BongoCatVk *vk) {
    (void)vk;
    return 0;
}

BongoCatVkTexture *bongo_cat_vk_texture_from_external(BongoCatVk *vk,
    void *image, void *view, int width, int height, const char *label,
    BongoCatError *error) {
    (void)vk; (void)image; (void)view; (void)width; (void)height;
    (void)label; (void)error;
    return NULL;
}

bool bongo_cat_vk_native_info(const BongoCatVk *vk, BongoCatVkNativeInfo *info) {
    (void)vk;
    (void)info;
    return false;
}

/* 自检的 Vulkan 首帧探测: 没有编 Vulkan 时直接给出"这个构建没有 Vulkan"的结论,
   而不是让面板显示"未知"。 */
bool bongo_cat_vk_selfcheck(BongoCatVkSelfCheck *report) {
    if (report) {
        memset(report, 0, sizeof(*report));
        report->compiled = false;
        report->format = -1;
        report->present_result = -1;
        snprintf(report->error, sizeof(report->error),
            "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
    }
    return false;
}

void bongo_cat_vk_set_render_scale(float scale) { (void)scale; }

#endif /* !BONGO_CAT_HAS_VULKAN */
