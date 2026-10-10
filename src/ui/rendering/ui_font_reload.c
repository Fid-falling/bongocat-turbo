/*
 * 运行时重建字体图集 (用户在设置里换了字体 / 改了语言)。做法是先在栈上搭一个
 * "临时 backend", 把新图集烘到它身上, 成功后再整体搬到真 backend —— 这样烘焙失败
 * 时旧图集毫发无损, 界面不会变成一团豆腐。
 *
 * 几个必须遵守的点:
 *  - frame_building 期间禁止调用: 此时 Nuklear 已经把本帧的图集指针记进命令缓冲,
 *    中途换掉等于让命令引用释放后的纹理;
 *  - 临时 backend 必须共享同一个 vk (见下面的注释), 否则会走 GL 路径去建纹理;
 *  - Vulkan 下只搬纹理、保留原 id: 旧图集先注销, 新纹理搬到同一个槽位, 已经烘进
 *    Nuklear 的 id 就不用改。
 */
#include "ui_font_atlas.h"

#include <SDL3/SDL_opengl.h>
#include <string.h>

bool bongo_cat_ui_font_atlas_reload(BongoCatUIBackend *ui,
    const char *body_path, const char *body_fallback_path,
    const char *body_korean_fallback_path, const char *heading_path,
    const char *heading_fallback_path,
    const char *heading_korean_fallback_path,
    const nk_rune *glyph_ranges, float raster_scale) {
    if (!ui) return false;
    if (ui->frame_building) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Refusing to replace a font atlas while a UI frame is being built");
        return false;
    }
    BongoCatUIBackend next;
    memset(&next, 0, sizeof(next));
    /* Vulkan 模式下临时后端要用同一个上下文, 图集才会走 vk 纹理路径 */
    next.vk = ui->vk;
    if (!nk_init_default(&next.context, NULL)) return false;
    bool created = bongo_cat_ui_font_atlas_create(&next, body_path,
        body_fallback_path, body_korean_fallback_path, heading_path,
        heading_fallback_path, heading_korean_fallback_path, glyph_ranges,
        raster_scale);
    nk_free(&next.context);
    if (!created) {
        if (bongo_cat_ui_uses_vulkan(&next)) {
            for (int i = 0; i < BONGO_CAT_UI_VK_TEXTURES; ++i) {
                if (!next.vk_textures[i]) continue;
                bongo_cat_vk_texture_destroy(next.vk, next.vk_textures[i]);
                next.vk_textures[i] = NULL;
            }
        } else if (next.font_texture) glDeleteTextures(1, &next.font_texture);
        if (next.atlas.permanent.alloc)
            bongo_cat_ui_font_atlas_destroy(&next);
        return false;
    }
    if (bongo_cat_ui_uses_vulkan(ui)) {
        /* 新图集落在临时后端的第一个空槽 (通常就是 1)。先把旧图集腾掉, 再把新
           纹理搬到同一个 id 上, Nuklear 里已经烘好的 id 就不用改了。 */
        int id = (int)next.font_texture;
        bongo_cat_ui_texture_unregister(ui, id);
        bongo_cat_ui_texture_move(ui, &next, id);
    } else if (ui->font_texture) glDeleteTextures(1, &ui->font_texture);
    /* 整体接管临时后端的成果。指针 (字体句柄 / range 数组) 都是所有权转移, 搬完
       之后不能再对 next 调 atlas_destroy —— 那会把它 free 掉, ui 就悬垂了。
       raster_scale 也要跟着更新: 下一次重载和缓存命中判定都以它为准。 */
    bongo_cat_ui_font_atlas_destroy(ui);
    ui->atlas = next.atlas;
    ui->null_texture = next.null_texture;
    ui->font_texture = next.font_texture;
    ui->font_atlas_width = next.font_atlas_width;
    ui->font_atlas_height = next.font_atlas_height;
    ui->caption_font = next.caption_font;
    ui->body_font = next.body_font;
    ui->label_font = next.label_font;
    ui->heading_font = next.heading_font;
    ui->hero_font = next.hero_font;
    ui->latin_glyph_ranges = next.latin_glyph_ranges;
    ui->cjk_glyph_ranges = next.cjk_glyph_ranges;
    ui->korean_glyph_ranges = next.korean_glyph_ranges;
    ui->custom_font_loaded = next.custom_font_loaded;
    ui->font_probe_loaded = next.font_probe_loaded;
    ui->font_path_found = next.font_path_found;
    ui->font_file_loaded = next.font_file_loaded;
    ui->raster_scale = raster_scale;
    nk_style_set_font(&ui->context, ui->body_font);
    return true;
}
