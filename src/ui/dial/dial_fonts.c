/*
 * 菜单的字体与字形图集。
 *
 * 与设置窗口的区别: 菜单是短命窗口 (开一次建一次), 所以图集不做增量维护 —— 直接把
 * "这次菜单可能显示的全部文字"扫一遍, 收集用到的码点, 生成范围表再烘焙一次。
 * 这个"全部文字"的清单就在 ranges() 里: 根项标签、模型名/动作名/表情名/声音名、
 * 以及"添加模型"。新增任何会显示的文字来源都必须加进去, 漏了就会显示成方块。
 *
 * 图集上传同样分两条路: Vulkan 走单通道纹理接口 + 纹理表登记, GL 走 R8 + 通道
 * swizzle (把单通道塞进的 red 当成 alpha, RGB 补 1)。两条路最后都要 nk_font_atlas_end
 * 把纹理句柄告诉 Nuklear。
 */
#include "dial_internal.h"
#include "bongo_cat/vk_backend.h"
#include "ui_font.h"
#include "ui_font_atlas_internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * 码点位图: 0x110000 个码点需要 136KB 的位图, 临时分配一次比排序去重简单得多,
 * 而且这张表只在这一处用 (烘焙完就 free)。
 */
static bool contains(const uint8_t *used, nk_rune rune) {
    return (used[rune >> 3] & (1u << (rune & 7))) != 0;
}

/*
 * 解码一段 UTF-8 并把用到的码点记进位图。坏字节直接终止 (调用方给的都是内部字符串,
 * 出现非法序列说明数据本身有问题, 继续解码只会越解越乱)。
 */
static void collect(uint8_t *used, const char *s) {
    if (!s) return;
    int length = (int)strlen(s);
    while (length > 0) {
        nk_rune rune;
        int n = nk_utf_decode(s,&rune,length);
        if (n <= 0) break;
        if (rune >= 32 && rune < 0x110000)
            used[rune >> 3] |= (uint8_t)(1u << (rune & 7));
        s += n; length -= n;
    }
}

/*
 * 生成 Nuklear 需要的字形范围表: 把位图里的连续码点合并成 [start, end] 区间, 以 0 结尾。
 * 一开始就无条件加入 32..126 (ASCII 可见字符): 百分比数字、模型名里的拉丁字母都要用,
 * 而且它们一定在, 不必逐个判断。
 * 返回的表由调用方持有并负责 free (最终由 dial_paint_free 释放)。
 */
static nk_rune *ranges(Dial *d) {
    uint8_t *used = calloc(0x110000 / 8,1);
    if (!used) return NULL;
    for (int i = 32; i < 127; ++i)
        used[i >> 3] |= (uint8_t)(1u << (i & 7));
    for (int i = 0; i < d->count; ++i) collect(used,d->items[i].label);
    collect(used,d->labels->add_model);
    for (size_t i = 0; i < d->labels->model_count; ++i) collect(used,d->labels->model_names[i]);
    for (size_t i = 0; i < d->labels->motion_count; ++i) collect(used,d->labels->motion_names[i]);
    for (size_t i = 0; i < d->labels->expression_count; ++i) collect(used,d->labels->expression_names[i]);
    for (size_t i = 0; i < d->labels->audio_count; ++i) collect(used,d->labels->audio_names[i]);
    size_t count = 0;
    for (nk_rune i = 32; i < 0x110000; ++i)
        if (contains(used,i) && (i == 32 || !contains(used,i-1))) ++count;
    nk_rune *result = calloc(count*2+1,sizeof(*result));
    if (result) {
        size_t out = 0;
        for (nk_rune i = 32; i < 0x110000; ++i) {
            if (!contains(used,i)) continue;
            result[out++] = i;
            while (i+1 < 0x110000 && contains(used,i+1)) ++i;
            result[out++] = i;
        }
    }
    free(used);
    return result;
}

/*
 * 建图集。字体是三段式 (常规 + 回退 + 韩文回退), 与设置窗口同一套 ui_font 工具;
 * 字号按显示密度放大但封顶 2 倍 (height = 24 * clamp(scale*raster_scale, 1, 2)):
 * 菜单开在高 DPI 屏上时字要够清楚, 但再大下去图集尺寸和烘焙时间都不划算。
 * 上传前用设备的最大纹理尺寸校验一次 (GL_MAX_TEXTURE_SIZE / Vulkan 的设备限值):
 * 中文字图集很容易超过老设备的 2048, 不检查的话上传会失败而且看不出原因。
 * 失败时 ranges[1..3] 置空: split_ranges 在失败时已经释放了部分分配, 留着悬空指针
 * 会让 dial_paint_free 二次释放。
 */
bool dial_fonts_init(Dial *d) {
    DialPaint *p = &d->paint;
    p->ranges[0] = ranges(d);
    if (!p->ranges[0] || !bongo_cat_ui_font_split_ranges(p->ranges[0],
        &p->ranges[1],&p->ranges[2],&p->ranges[3])) {
        /* split_ranges frees partial allocations on failure. */
        p->ranges[1] = p->ranges[2] = p->ranges[3] = NULL;
        return false;
    }
    char paths[3][BONGO_CAT_PATH_CAP];
    const char *names[] = {
        bongo_cat_ui_system_font(paths[0],sizeof(paths[0]),false),
        bongo_cat_ui_system_font(paths[1],sizeof(paths[1]),true),
        bongo_cat_ui_system_korean_font(paths[2],sizeof(paths[2]))};
    UIFontSource sources[3] = {0};
    for (int i = 0; i < 3; ++i)
        if (names[i]) bongo_cat_ui_font_source_load(&sources[i],names[i]);
    nk_font_atlas_init_default(&p->atlas);
    nk_font_atlas_begin(&p->atlas);
    float height = 24 * fmaxf(1,fminf(2,d->scale*d->raster_scale));
    p->font = bongo_cat_ui_font_add_family(&p->atlas,&sources[0],&sources[1],&sources[2],
        height,p->ranges[0],p->ranges[1],p->ranges[2],p->ranges[3]);
    int width = 0, rows = 0, maximum = 0;
    const void *pixels = p->font ? nk_font_atlas_bake(&p->atlas,&width,&rows,NK_FONT_ATLAS_ALPHA8) : NULL;
    /*
     * Vulkan 菜单的字体图集不能用 GL 上传: 从设备限值取尺寸上限, 走 vk_backend
     * 的单通道纹理接口, 再把句柄登记进纹理表 (与设置窗口同一套做法)。
     */
    if (d->vk) maximum = bongo_cat_vk_max_texture_dimension(d->vk);
    else glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
    bool ok = pixels && width > 0 && rows > 0 && width <= maximum && rows <= maximum;
    if (ok && d->vk) {
        BongoCatError error = {0};
        BongoCatVkTexture *texture = bongo_cat_vk_texture_from_alpha(d->vk,pixels,
            width,rows,"dial-font-atlas",&error);
        int id = texture ? dial_vk_texture_register(d,texture) : 0;
        if (id) {
            p->font_texture = (GLuint)id;
            nk_font_atlas_end(&p->atlas,nk_handle_id(id),NULL);
        } else {
            if (texture) bongo_cat_vk_texture_destroy(d->vk,texture);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Dial font atlas upload failed: %s",error.message);
            ok = false;
        }
    }
    if (ok && !d->vk) {
        glGenTextures(1,&p->font_texture);
        glBindTexture(GL_TEXTURE_2D,p->font_texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        /* Match the preferences atlas: white RGB with single-channel alpha. */
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R,GL_ONE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_G,GL_ONE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_B,GL_ONE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_A,GL_RED);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R8,width,rows,0,GL_RED,GL_UNSIGNED_BYTE,pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        ok = p->font_texture && glGetError() == GL_NO_ERROR;
        nk_font_atlas_end(&p->atlas,nk_handle_id((int)p->font_texture),NULL);
    }
    for (int i = 0; i < 3; ++i) {
        bongo_cat_ui_font_detach_source(&p->atlas,&sources[i]);
        bongo_cat_ui_font_source_release(&sources[i]);
    }
    nk_font_atlas_cleanup(&p->atlas);
    return ok;
}

/*
 * 画一行文字: 逐字形取图集里的矩形再贴出来 (没有用 Nuklear 的绘制命令, 直接产出
 * dial_quad 顶点, 这样文字和菜单其余部分走同一条管线)。
 * 两个宽度约束: 若整行超过 width 就整体等比压缩 (scale = width/total), 而不是截断 ——
 * 菜单里的名字都很有信息量, 宁可小一点也要显示全。
 * glyphs[] 数组是按 BONGO_CAT_MENU_LABEL_CAP 定长的, 超长文本会被截断在这里。
 */
static void line(Dial *d, const char *s, int length, float x, float y,
    float width, float size, uint32_t color) {
    struct nk_font *font = d->paint.font;
    const struct nk_font_glyph *glyphs[BONGO_CAT_MENU_LABEL_CAP];
    int count = 0;
    float total = 0;
    while (length > 0 && count < BONGO_CAT_MENU_LABEL_CAP) {
        nk_rune rune;
        int n = nk_utf_decode(s,&rune,length);
        if (n <= 0) break;
        const struct nk_font_glyph *g = nk_font_find_glyph(font,rune);
        if (g) { glyphs[count++] = g; total += g->xadvance; }
        s += n; length -= n;
    }
    float scale = size/font->info.height;
    if (total*scale > width && total > 0) scale = width/total;
    float left = x-total*scale/2, top = y-font->info.height*scale/2;
    for (int i = 0; i < count; ++i) {
        const struct nk_font_glyph *g = glyphs[i];
        if (g->w > 0 && g->h > 0)
            dial_quad(d,d->paint.font_texture,left+g->x0*scale,top+g->y0*scale,
                g->w*scale,g->h*scale,g->u0,g->v0,g->u1,g->v1,color);
        left += g->xadvance*scale;
    }
}

/*
 * 文本入口, 两种特殊排版:
 *   1. "live2d_xxx" 这种内部 id 拆成两行显示 (上一行 live2d, 下一行剩下部分): 直接显示
 *      会又长又难读, 而这类名字在动作列表里很常见;
 *   2. 普通长名先试一行; 一行放不下时找"让两行宽度尽量相等"的 UTF-8 边界再折成两行
 *      (见英文注释)。折行而不缩字号, 是因为缩到能放下时字会小到看不清。
 * 两条路最后都落到 line(), 所以行内的压缩逻辑只需维护一份。
 */
void dial_text(Dial *d, const char *s, float x, float y, float width,
    float size, uint32_t color) {
    if (!s || !d->paint.font) return;
    if (!strncmp(s,"live2d_",7)) {
        line(d,"live2d",6,x,y-size*.55f,width,size,color);
        line(d,s+7,(int)strlen(s+7),x,y+size*.55f,width,size,color);
    } else {
        /* Wrap at a UTF-8 boundary before shrinking a long name. Two lines
           fit the child ring while keeping normal labels at full size. */
        int length = (int)strlen(s), offset = 0, split = 0;
        float total = 0, best = width;
        struct nk_font *font = d->paint.font;
        float scale = size/font->info.height;
        while (offset < length) {
            nk_rune rune;
            int n = nk_utf_decode(s+offset,&rune,length-offset);
            if (n <= 0) break;
            const struct nk_font_glyph *g = nk_font_find_glyph(font,rune);
            if (g) total += g->xadvance*scale;
            offset += n;
        }
        if (total > width) {
            float advance = 0;
            offset = 0; best = total;
            while (offset < length) {
                nk_rune rune;
                int n = nk_utf_decode(s+offset,&rune,length-offset);
                if (n <= 0) break;
                const struct nk_font_glyph *g = nk_font_find_glyph(font,rune);
                if (g) advance += g->xadvance*scale;
                offset += n;
                float difference = fabsf(total-2*advance);
                if (offset < length && difference < best) {
                    best = difference; split = offset;
                }
            }
        }
        if (split) {
            line(d,s,split,x,y-size*.55f,width,size,color);
            line(d,s+split,length-split,x,y+size*.55f,width,size,color);
        } else line(d,s,length,x,y,width,size,color);
    }
}
