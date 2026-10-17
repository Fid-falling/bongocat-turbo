#include "overlay_internal.h"

#include "bongo_cat/image.h"
#include "bongo_cat/log.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL_log.h>

#ifndef BONGO_CAT_HAS_CUBISM
/* 非 Cubism 构建里叠加层画的是合成预览图: 基图 + 按下按键的左右手图。 */
static void refresh_composite(BongoCatOverlay *value) {
    /* 先清脏标记再干活: 合成一张图要读三个文件并逐像素混合, 每帧重算会明显掉帧,
       所以只由按键状态变化触发的 dirty 驱动; 代价是合成失败后不会每帧重试, 要等
       下一次按键变化 (这样也避免了失败日志刷屏)。 */
    if (!value->composite_dirty) return;
    value->composite_dirty = false;
    /* 多键显示下每侧只把**最早按下**的那个键 (keys[0], 由 refresh_keys 同步进
       left_path/right_path) 合成进这张图: 合成图只吃左右各一张, 所以非 Cubism 构建里
       多键退化为单键 —— 与 bongo_cat_overlay_display_limit 恒返回 1 是同一件事的两面。 */
    const bool left_active = value->left_path[0] != '\0';
    const bool right_active = value->right_path[0] != '\0';
    if (!left_active && !right_active) return;
    BongoCatError ignored = {0};
    BongoCatImage image = {0};
    if (bongo_cat_image_composite(value->background_path, value->left_path,
            value->right_path, value->clean_paws && left_active,
            value->clean_paws && right_active, &image, &ignored) != BONGO_CAT_OK) {
        if (ignored.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", ignored.message);
        return;
    }
    bongo_cat_texture_from_image(&value->composite, &image, false, &ignored);
    bongo_cat_image_free(&image);
}
#else
static void refresh_composite(BongoCatOverlay *value) { (void)value; }
#endif

static bool has_composite(const BongoCatOverlay *value) {
    return bongo_cat_texture_ready(&value->composite);
}

/*
 * 两条后端的共同入口, 语义必须逐条对齐: 参考尺寸缩放、镜像、擦除脚印、预乘 alpha。
 * OpenGL 侧的混合模式由 erase 开关决定 —— 擦除脚印要求"把 RGB 乘上覆盖率"的乘法
 * 混合 (见 draw 里的 else 分支), Vulkan 侧对应的是 BONGO_CAT_VK_BLEND_MULTIPLY,
 * 两边改一边就会只有一条后端能擦干净。
 * 纹理没就绪直接返回: 背景/按键贴图都可能缺失, 缺了就当这一层不存在。
 *
 * keycap_only (多键显示的第二通道): 同一侧最早按下的键画整张图 (手 + 它自己的按键提示),
 * 其余键只留"够亮 + 够彩"的按键高亮像素 (见 overlay.c 的片元着色器), 免得叠出多只手。
 * 只有**没有自带气泡素材**的模型走这条通道 —— 自带气泡的那些由下面 draw_keys 交给
 * overlay_assets.c 的单气泡合成处理 (那里拼出来的是整张图, 按普通贴图画即可)。
 *
 * premultiply 只影响 Vulkan: 叠加层贴图在 Vulkan 侧是直通 alpha 上传的, 片元里要
 * 乘一次 alpha (见 overlay.frag); GL 侧上传时就乘好了, 这里传什么都不用。传错会让
 * 半透明区域整体偏亮或者偏暗, 所以调用点按贴图来源显式给出。
 */
static void draw(BongoCatOverlay *value, const BongoCatTexture *texture,
    bool mirror, bool blend, bool premultiply, bool keycap_only) {
    if (!value || !bongo_cat_texture_ready(texture)) return;
    const bool erase_left = !blend && value->composed_cover &&
        !has_composite(value) && bongo_cat_overlay_hand_active(value, false);
    const bool erase_right = !blend && value->composed_cover &&
        !has_composite(value) && bongo_cat_overlay_hand_active(value, true);
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        bongo_cat_vk_draw_overlay_quad(bongo_cat_vk_current(), texture->vk,
            mirror, value->reference_width, value->reference_height,
            erase_left, erase_right, premultiply, keycap_only);
        return;
    }
    if (blend) {
        glEnable(GL_BLEND);
        value->gl.blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else glDisable(GL_BLEND);
    value->gl.use_program(value->program);
    value->gl.uniform_1i(value->mirror_location, mirror);
    value->gl.uniform_1i(value->image_location, 0);
    value->gl.uniform_1i(value->reference_width_location, value->reference_width);
    value->gl.uniform_1i(value->reference_height_location, value->reference_height);
    value->gl.uniform_1i(value->erase_left_location, erase_left);
    value->gl.uniform_1i(value->erase_right_location, erase_right);
    value->gl.uniform_1i(value->keycap_location, keycap_only);
    value->gl.active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture->gl);
    value->gl.bind_vertex_array(value->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    value->gl.bind_vertex_array(0);
}

/* 有手按下且合成图可用时画合成图 (按下的手已经烧进图里), 否则画纯背景。两者只能
   二选一: 同时画会让按下的手在背景与合成图上各出现一次。 */
void bongo_cat_overlay_draw_background(BongoCatOverlay *value, bool mirror) {
    if (!value) return;
    refresh_composite(value);
    bool active = bongo_cat_overlay_hand_active(value, false) ||
        bongo_cat_overlay_hand_active(value, true);
    draw(value, active && has_composite(value) ? &value->composite :
        &value->background, mirror, false, true, false);
}

/*
 * 按键层。每侧分两种画法, 由"模型有没有自带气泡素材"决定:
 *
 *  - 自带气泡素材 (转换模型: 每键一张"气泡 + 手"的整图) 且同侧多键: 只画**一张** CPU 合成图
 *    (手 + 一个装着 "A + S" 的气泡), 于是头顶始终只有一个气泡。合成失败就退回只画首个键,
 *    绝不同时画合成图与逐个按键 —— 那会叠出多只手。
 *  - 其余情形 (内置模型没有气泡素材): 首个键画整张图 —— 手 + 它自己的按键高亮; 其余键走
 *    第二通道只留高饱和键帽。同侧多键于是得到"一只手 + N 块键帽"。
 */
void bongo_cat_overlay_draw_keys(BongoCatOverlay *value, bool mirror) {
    if (!value) return;
#ifdef BONGO_CAT_HAS_CUBISM
    const int limit = bongo_cat_overlay_display_limit(value);
    for (int side = 0; side < 2; ++side) {
        const OverlaySide *hand = &value->sides[side];
        if (hand->count == 0) continue;
        if (hand->count > 1 && bongo_cat_overlay_side_bubble(value, side)) {
            /* 临时诊断 (定位完删除): 只在分支结果变化时记一行 */
            static int logged[2] = {-1, -1};
            const bool refreshed = bongo_cat_overlay_refresh_bubble(value, side);
            if (logged[side] != (int)refreshed) {
                bongo_cat_bubble_debug("draw side=%d 合成图可用=%d", side, refreshed);
                logged[side] = (int)refreshed;
            }
            const BongoCatTexture *bubble = refreshed
                ? &value->bubble_composite[side] : hand->keys[0].texture;
            draw(value, bubble, mirror, true, true, false);
            continue;
        }
        for (size_t i = 0; i < hand->count && (int)i < limit; ++i) {
            /* i == 0: 手 + 它自己的按键高亮 (整张图); 其余: 只留高饱和键帽 */
            draw(value, hand->keys[i].texture, mirror, true, true, i != 0);
        }
    }
#else
    /* 非 Cubism 构建: 左右手在 cover 合成阶段就烧进背景了, 这里刻意什么都不画。
       不要"补"上绘制调用 —— 手会在背景与这一层各出现一次。 */
    (void)mirror;
#endif
}

/* 必须在按键绘制之前调用: 手臂要被键盘面板压住 (见 mver 头文件里的分层契约) */
void bongo_cat_overlay_draw_pointer_before_keys(BongoCatOverlay *value) {
    if (bongo_cat_overlay_mver_pointer_enabled(value))
        bongo_cat_mver_pointer_overlay_draw_before_keys(value->mver_pointer);
}

/* 按键特效层。调用方 (OpenGL 的 app_loop 与 Vulkan 的 vk_scene) 都固定按
   background -> pointer_before_keys -> keys -> effect -> pointer_after_keys 的顺序
   调用, 这个顺序不能调: 特效要盖在按键之上, 而非鼠标模式的手/手柄还要压住特效。
   非 Cubism 构建没有独立特效贴图 (合成图里已含), 这里是空实现。 */
void bongo_cat_overlay_draw_effect(BongoCatOverlay *value, bool mirror) {
    if (!value) return;
#ifdef BONGO_CAT_HAS_CUBISM
    draw(value, value->effect, mirror, true, true, false);
#else
    (void)mirror;
#endif
}

/* 必须在按键绘制之后调用: 非鼠标模式下画的手/手柄要压在键盘面板之上 */
void bongo_cat_overlay_draw_pointer_after_keys(BongoCatOverlay *value) {
    if (bongo_cat_overlay_mver_pointer_enabled(value))
        bongo_cat_mver_pointer_overlay_draw_after_keys(value->mver_pointer);
}
