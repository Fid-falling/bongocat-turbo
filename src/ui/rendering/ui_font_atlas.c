/*
 * 字体图集的建立: 6 个字体文件 (正文字体 + 拉丁/韩文回退, 标题字体 + 同样的两个
 * 回退) 烘成一张纹理, 再给每个字号建一个 nk_font 句柄。
 *
 * 所有权契约 (很容易搞错): TTF 文件是按路径 mmap 的, 图集烘焙期间 nk_font_config
 * 只借指针 (ttf_data_owned_by_atlas=1 会让 Nuklear 在 clear 时去 free)。所以
 * 顺序必须是: 先 detach_source() 把 config 里的 blob 指针抹掉, 再 release() 解除
 * 映射 —— 反过来就是 free 一个 mmap 出来的地址。失败路径同样要成对走完, 否则
 * 图集建失败时会留下泄漏的映射。
 *
 * 同一个路径传多次 (例如正文和标题都是微软雅黑) 只加载一份, load_source 会复用
 * 前面的条目; 这也是下面为什么能用指针相等来判断"要不要共用字号句柄"。
 */
#include "ui_font_atlas_internal.h"

#include <SDL3/SDL_opengl.h>
#include <stdlib.h>
#include <string.h>

/* Reuse mappings only within this bake. Owners remain in sources[] and are
   detached/released once, including on failure. */
static const UIFontSource *load_source(UIFontSource *sources,
    const char *const *paths, int index) {
    if (!paths[index]) return NULL;
    for (int i = 0; i < index; ++i)
        if (sources[i].data && paths[i] && strcmp(paths[i], paths[index]) == 0)
            return &sources[i];
    return bongo_cat_ui_font_source_load(&sources[index], paths[index]) ?
        &sources[index] : NULL;
}

bool bongo_cat_ui_font_atlas_create(BongoCatUIBackend *ui,
    const char *body_path, const char *body_fallback_path,
    const char *body_korean_fallback_path, const char *heading_path,
    const char *heading_fallback_path,
    const char *heading_korean_fallback_path,
    const nk_rune *glyph_ranges, float raster_scale) {
    UIFontSource sources[6] = {0};
    const char *paths[] = {body_path, body_fallback_path,
        body_korean_fallback_path, heading_path, heading_fallback_path,
        heading_korean_fallback_path};
    const UIFontSource *body = load_source(sources, paths, 0);
    const UIFontSource *body_fallback = body_fallback_path &&
        (!body_path || strcmp(body_path, body_fallback_path) != 0) ?
        load_source(sources, paths, 1) : NULL;
    const UIFontSource *body_korean_fallback = body_korean_fallback_path &&
        (!body_path || strcmp(body_path, body_korean_fallback_path) != 0) &&
        (!body_fallback_path || strcmp(body_fallback_path,
            body_korean_fallback_path) != 0) ?
        load_source(sources, paths, 2) : NULL;
    const UIFontSource *heading = load_source(sources, paths, 3);
    const UIFontSource *heading_fallback = heading_fallback_path &&
        (!heading_path || strcmp(heading_path, heading_fallback_path) != 0) ?
        load_source(sources, paths, 4) : NULL;
    const UIFontSource *heading_korean_fallback = heading_korean_fallback_path &&
        (!heading_path || strcmp(heading_path, heading_korean_fallback_path) != 0) &&
        (!heading_fallback_path || strcmp(heading_fallback_path,
            heading_korean_fallback_path) != 0) ?
        load_source(sources, paths, 5) : NULL;
    const UIFontSource *heading_source = heading ? heading : body;
    const UIFontSource *heading_fallback_source = heading_fallback ?
        heading_fallback : body_fallback;
    const UIFontSource *heading_korean_fallback_source =
        heading_korean_fallback ? heading_korean_fallback :
        (body_korean_fallback ? body_korean_fallback : heading_fallback_source);
    nk_rune *primary_ranges = NULL, *cjk_ranges = NULL;
    nk_rune *korean_ranges = NULL;
    if (!bongo_cat_ui_font_split_ranges(glyph_ranges, &primary_ranges,
        &cjk_ranges, &korean_ranges)) {
        for (int i = 0; i < 6; ++i)
            bongo_cat_ui_font_source_release(&sources[i]);
        return false;
    }
    nk_font_atlas_init_default(&ui->atlas);
    nk_font_atlas_begin(&ui->atlas);
    /* 图集尺寸要按 GPU 上限夹一下: 超过 GL_MAX_TEXTURE_SIZE / Vulkan 的
       maxImageDimension2D 时上传必然失败, 而失败发生在烘焙之后 (白等几百毫秒)。 */
    GLint maximum_texture = 0;
    if (bongo_cat_ui_uses_vulkan(ui))
        maximum_texture = bongo_cat_vk_max_texture_dimension(ui->vk);
    else
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture);
    /* Keep the atlas bounded on high-DPI displays. The UI is laid out in
       logical pixels, so a 1.5x raster is enough for crisp text while a 2x
       atlas can quadruple both the texture and bake working memory. */
    float scale_limit = maximum_texture >= 8192 ? 1.5f : 1.0f;
    float font_scale = NK_CLAMP(1.0f, raster_scale, scale_limit);
    if (font_scale + .01f < raster_scale) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "Preferences font raster scale limited to %.2fx by the UI memory "
        "budget (GPU texture limit %d)",
        font_scale, maximum_texture);
    /* 正文字体 + 拉丁回退 + 韩文回退合成一个家族; 韩文缺省时退到拉丁回退字体,
       所以 body_korean_fallback_source 允许为 NULL 也不会缺字形 (只是画不出韩文)。 */
    const UIFontSource *body_source = body;
    const UIFontSource *fallback_source = body_fallback;
    const UIFontSource *body_korean_fallback_source =
        body_korean_fallback ? body_korean_fallback : fallback_source;
    struct nk_font *caption_font = bongo_cat_ui_font_add_family(&ui->atlas,
        body_source, fallback_source, body_korean_fallback_source,
        18.0f * font_scale, glyph_ranges, primary_ranges, cjk_ranges,
        korean_ranges);
    struct nk_font *body_font = bongo_cat_ui_font_add_family(&ui->atlas,
        body_source, fallback_source, body_korean_fallback_source,
        20.0f * font_scale, glyph_ranges, primary_ranges, cjk_ranges,
        korean_ranges);
    /* Identical families at 20 px can share glyphs and the font handle. */
    bool shared_label = body_source == heading_source &&
        fallback_source == heading_fallback_source &&
        body_korean_fallback_source == heading_korean_fallback_source;
    struct nk_font *label_font = shared_label ? body_font :
        bongo_cat_ui_font_add_family(&ui->atlas,
        heading_source, heading_fallback_source,
        heading_korean_fallback_source, 20.0f * font_scale, glyph_ranges,
        primary_ranges, cjk_ranges, korean_ranges);
    struct nk_font *heading_font = bongo_cat_ui_font_add_family(&ui->atlas,
        heading_source, heading_fallback_source,
        heading_korean_fallback_source, 28.0f * font_scale, glyph_ranges,
        primary_ranges, cjk_ranges, korean_ranges);
    struct nk_font *hero_font = bongo_cat_ui_font_add_family(&ui->atlas,
        heading_source, heading_fallback_source,
        heading_korean_fallback_source, 36.0f * font_scale, glyph_ranges,
        primary_ranges, cjk_ranges, korean_ranges);
    bool uploaded = caption_font && body_font && label_font && heading_font &&
        hero_font && bongo_cat_ui_font_upload_atlas(ui);
    if (uploaded) {
        /* handle.height 必须改回"逻辑像素"字号: 字体是按 font_scale 烘焙的, 而
           Nuklear 用 handle.height 参与布局 (行高、居中、tooltip 换行都读它)。
           留着烘焙时的像素值会让高 DPI 下每行都变高、整页被撑开。 */
        ui->caption_font = &caption_font->handle;
        ui->body_font = &body_font->handle;
        ui->label_font = &label_font->handle;
        ui->heading_font = &heading_font->handle;
        ui->hero_font = &hero_font->handle;
        caption_font->handle.height = 18.0f;
        body_font->handle.height = 20.0f;
        label_font->handle.height = 20.0f;
        heading_font->handle.height = 28.0f;
        hero_font->handle.height = 36.0f;
        ui->latin_glyph_ranges = primary_ranges;
        ui->cjk_glyph_ranges = cjk_ranges;
        ui->korean_glyph_ranges = korean_ranges;
        ui->font_probe_loaded = bongo_cat_ui_font_has_ranges(body_font,
            glyph_ranges);
        nk_style_set_font(&ui->context, ui->body_font);
    } else {
        free(korean_ranges); free(cjk_ranges); free(primary_ranges);
    }
    ui->font_path_found = body_path != NULL || body_fallback_path != NULL ||
        body_korean_fallback_path != NULL;
    ui->font_file_loaded = body || body_fallback || body_korean_fallback;
    ui->custom_font_loaded = ui->font_file_loaded && body_font != NULL;
    /*
     * 收尾顺序是契约的一部分: 先 detach 把 config 里的 ttf_blob 抹掉, 再
     * atlas_cleanup 让 Nuklear 释放它自己分配的东西, 最后才解除文件映射。
     * 失败时 (uploaded == false) 这三个数组已经从本函数交不出去, 由这里 free;
     * 成功时它们挂在 ui 上, 由 bongo_cat_ui_font_atlas_destroy() 负责。
     */
    for (int i = 0; i < 6; ++i)
        bongo_cat_ui_font_detach_source(&ui->atlas, &sources[i]);
    nk_font_atlas_cleanup(&ui->atlas);
    for (int i = 0; i < 6; ++i)
        bongo_cat_ui_font_source_release(&sources[i]);
    return uploaded;
}

void bongo_cat_ui_font_atlas_destroy(BongoCatUIBackend *ui) {
    if (!ui) return;
    nk_font_atlas_clear(&ui->atlas);
    free(ui->korean_glyph_ranges); free(ui->cjk_glyph_ranges);
    free(ui->latin_glyph_ranges);
    ui->korean_glyph_ranges = NULL; ui->cjk_glyph_ranges = NULL;
    ui->latin_glyph_ranges = NULL;
    ui->caption_font = NULL;
    ui->body_font = NULL;
    ui->label_font = NULL;
    ui->heading_font = NULL;
    ui->hero_font = NULL;
    ui->font_atlas_width = 0;
    ui->font_atlas_height = 0;
}
