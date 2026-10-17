#include "overlay_internal.h"
#include "bongo_cat/image.h"

#include <stdlib.h>
#include <string.h>

/*
 * "键名贴片"的抠取: 从模型自带的按键图里, 把气泡中的那个键名单独抠成一张小图。
 * 合并模式要在一张气泡里排 "A + S", 必须把各键的键名挪进同一个气泡; 而字符本身来自
 * 模型自己的美术, 所以抠出来的字与单键时天然同字同色同字号。上层 (overlay_bubble.c)
 * 只负责排版, 这里负责"哪几个像素是键名"与"把它拷成贴片"。
 *
 * 键名文字的颜色因模型而异 (实测: 浅蓝 #8FC1F4、酒红 #763353、珊瑚红 #DB5D6A、藏青
 * #2E3560、石板蓝 #4F5D79), 描边也未必是黑色 (酒棕、深紫), 任何固定颜色判据都注定漏掉
 * 一部分模型。真正可靠的共性是几何: 文字浮在气泡填充色中央, 而描边、心形缺口与尾巴都与
 * 气泡外框连通 (矩形就是按外框包围盒算的)。所以这里不按色相找文字:
 *   1. 内缩 14% 的中心区里出现最多的颜色 = 填充色 (4bit/通道量化直方图, 再取该桶均值);
 *   2. 与填充色曼哈顿距离 >40 的不透明像素是"非填充候选" (>96 算严格候选, 即文字主体);
 *   3. 从矩形边界沿候选像素 flood fill, 连通的都是描边/装饰, 剩下的孤岛才是文字。
 * 孤岛按尺寸过滤掉噪点, 包围盒取所有保留孤岛的并集 ("i" 的点与竖是两座孤岛, 都算文字)。
 * 掩码每位像素: bit0/1 = 候选级别 (1 宽松 / 2 严格), bit2 = 已访问, bit3 = 保留 (文字)。 */
typedef struct BubbleTextMask {
    unsigned char *mask;   /* bw*bh 字节 */
    unsigned char fill[3]; /* 学出的气泡填充色 */
    int left, top;         /* 覆盖区左上角在整图中的像素坐标 */
    int bw, bh;            /* 覆盖区尺寸 */
    int x0, y0, x1, y1;    /* 文字紧包围盒 (整图像素坐标, 含端点) */
} BubbleTextMask;

static void text_mask_free(BubbleTextMask *value) {
    free(value->mask);
    memset(value, 0, sizeof(*value));
}

/* 贴片左上角在整张键图里的像素坐标; 矩形按归一化坐标换算, 越界处夹回图内。气泡矩形来自
   模型探测 (见 overlay_assets.c), 而排版与绘制都要按同一套像素边界裁切, 所以两边共用。 */
void bongo_cat_bubble_rect_pixels(const BongoCatImage *image, const float *rect,
    int bounds[4]) {
    bounds[0] = (int)(rect[0] * image->width);
    bounds[1] = (int)(rect[1] * image->height);
    bounds[2] = (int)(rect[2] * image->width);
    bounds[3] = (int)(rect[3] * image->height);
    if (bounds[0] < 0) bounds[0] = 0;
    if (bounds[1] < 0) bounds[1] = 0;
    if (bounds[2] > image->width) bounds[2] = image->width;
    if (bounds[3] > image->height) bounds[3] = image->height;
}

/* 边界/孤岛共用这一个宏做 flood 扩展: 候选且未访问就标记并入队 (每个像素至多入队一次,
   队列按 bw*bh 配容量, 不会溢出)。 */
#define FLOOD_VISIT(index) \
    do { \
        if ((mask[index] & 3) && !(mask[index] & 4)) { \
            mask[index] |= 4; \
            queue[tail++] = (index); \
        } \
    } while (0)

/* 学出气泡填充色并把"键名文字"从描边/装饰里分离出来; 矩形内没有任何文字时返回 false */
static bool text_mask_build(const BongoCatImage *image, const float *rect,
    BubbleTextMask *out) {
    int bounds[4];
    bongo_cat_bubble_rect_pixels(image, rect, bounds);
    const int bw = bounds[2] - bounds[0], bh = bounds[3] - bounds[1];
    if (bw < 8 || bh < 8) return false;
    unsigned bins[4096] = {0};
    const int mx = bw * 14 / 100, my = bh * 14 / 100;
    for (int y = bounds[1] + my; y < bounds[3] - my; ++y) {
        const unsigned char *row = image->pixels + (size_t)y * image->width * 4;
        for (int x = bounds[0] + mx; x < bounds[2] - mx; ++x) {
            const unsigned char *pixel = row + (size_t)x * 4;
            if (pixel[3] <= 8) continue;
            bins[(pixel[0] >> 4) << 8 | (pixel[1] >> 4) << 4 | pixel[2] >> 4]++;
        }
    }
    int best = 0;
    for (int i = 1; i < 4096; ++i)
        if (bins[i] > bins[best]) best = i;
    unsigned sr = 0, sg = 0, sb = 0, sn = 0;
    for (int y = bounds[1] + my; y < bounds[3] - my; ++y) {
        const unsigned char *row = image->pixels + (size_t)y * image->width * 4;
        for (int x = bounds[0] + mx; x < bounds[2] - mx; ++x) {
            const unsigned char *pixel = row + (size_t)x * 4;
            if (pixel[3] <= 8) continue;
            if (((pixel[0] >> 4) << 8 | (pixel[1] >> 4) << 4 |
                pixel[2] >> 4) != best) continue;
            sr += pixel[0]; sg += pixel[1]; sb += pixel[2]; sn++;
        }
    }
    if (!sn) return false;
    out->fill[0] = (unsigned char)(sr / sn);
    out->fill[1] = (unsigned char)(sg / sn);
    out->fill[2] = (unsigned char)(sb / sn);
    unsigned char *mask = calloc((size_t)bw * bh, 1);
    int *queue = malloc((size_t)bw * bh * sizeof(*queue));
    if (!mask || !queue) {
        free(mask);
        free(queue);
        return false;
    }
    int head = 0, tail = 0;
    for (int y = 0; y < bh; ++y) {
        const unsigned char *row =
            image->pixels + (size_t)(bounds[1] + y) * image->width * 4;
        for (int x = 0; x < bw; ++x) {
            const unsigned char *pixel = row + (size_t)(bounds[0] + x) * 4;
            if (pixel[3] <= 8) continue;
            const int d = abs(pixel[0] - out->fill[0]) +
                abs(pixel[1] - out->fill[1]) +
                abs(pixel[2] - out->fill[2]);
            if (d <= 40) continue;
            mask[(size_t)y * bw + x] = d > 96 ? 2 : 1;
            if (x == 0 || y == 0 || x == bw - 1 || y == bh - 1)
                FLOOD_VISIT(y * bw + x);
        }
    }
    while (head < tail) {   /* 先扩完整片"与边界连通"的描边/缺口/尾巴 */
        const int cur = queue[head++];
        const int cx = cur % bw, cy = cur / bw;
        if (cx > 0)      FLOOD_VISIT(cur - 1);
        if (cx < bw - 1) FLOOD_VISIT(cur + 1);
        if (cy > 0)      FLOOD_VISIT(cur - bw);
        if (cy < bh - 1) FLOOD_VISIT(cur + bw);
    }
    int min_island = bw * bh / 400;
    if (min_island > 64) min_island = 64;
    if (min_island < 6) min_island = 6;
    /* 两道过滤缺一不可。边界 flood 挡不住"心形气泡"这类四角透明/白底的素材: 描边四周
       是白色填充, 不与矩形边连通, 于是整条描边自成一个孤岛 (实测爱莉 KeyA 里这个孤岛
       12419 像素)。它有一条稳定特征: 这个孤岛的包围盒几乎填满整个气泡外接矩形 (实测
       占 96% × 95%), 而键名文字浮在气泡内部, 两个方向都远小于矩形 (实测 35% × 47%)。
       所以覆盖率超 85% 的孤岛一律按外框丢弃 —— 长键名最多只会超出其中一个方向。 */
    int x0 = bw, y0 = bh, x1 = -1, y1 = -1;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            if (!(mask[(size_t)y * bw + x] & 3) || (mask[(size_t)y * bw + x] & 4))
                continue;
            const int base = tail;   /* 队列尾部暂存本孤岛像素, 事后按尺寸决定去留 */
            FLOOD_VISIT(y * bw + x);
            while (head < tail) {
                const int cur = queue[head++];
                const int cx = cur % bw, cy = cur / bw;
                if (cx > 0)      FLOOD_VISIT(cur - 1);
                if (cx < bw - 1) FLOOD_VISIT(cur + 1);
                if (cy > 0)      FLOOD_VISIT(cur - bw);
                if (cy < bh - 1) FLOOD_VISIT(cur + bw);
            }
            if (tail - base < min_island) continue;
            int ix0 = bw, iy0 = bh, ix1 = -1, iy1 = -1;
            for (int i = base; i < tail; ++i) {
                const int cx = queue[i] % bw, cy = queue[i] / bw;
                if (cx < ix0) ix0 = cx;
                if (cx > ix1) ix1 = cx;
                if (cy < iy0) iy0 = cy;
                if (cy > iy1) iy1 = cy;
            }
            if ((ix1 - ix0 + 1) * 100 >= bw * 85 &&
                (iy1 - iy0 + 1) * 100 >= bh * 85) continue;
            for (int i = base; i < tail; ++i) {
                mask[queue[i]] |= 8;
                const int cx = queue[i] % bw, cy = queue[i] / bw;
                if (cx < x0) x0 = cx;
                if (cx > x1) x1 = cx;
                if (cy < y0) y0 = cy;
                if (cy > y1) y1 = cy;
            }
        }
    free(queue);
    if (x1 < x0) {
        free(mask);
        return false;
    }
    out->mask = mask;
    out->left = bounds[0];
    out->top = bounds[1];
    out->bw = bw;
    out->bh = bh;
    out->x0 = bounds[0] + x0;
    out->y0 = bounds[1] + y0;
    out->x1 = bounds[0] + x1;
    out->y1 = bounds[1] + y1;
    return true;
}
#undef FLOOD_VISIT

void bongo_cat_bubble_glyph_free(BubbleGlyph *glyph) {
    free(glyph->pixels);
    memset(glyph, 0, sizeof(*glyph));
}

/* 按掩码把"键名文字"拷成贴片并记下主体色。文字的抗锯齿是在**填充色**上混出来的 (像素本身
   就写着混好的 RGB), 照抄 RGB 就能在被抹成同一填充色的空气泡上还原同一份观感。 */
static bool glyph_from_mask(const BongoCatImage *image,
    const BubbleTextMask *tm, BubbleGlyph *glyph) {
    const int width = tm->x1 - tm->x0 + 1, height = tm->y1 - tm->y0 + 1;
    unsigned char *pixels = calloc((size_t)width * (size_t)height * 4, 1);
    if (!pixels) return false;
    unsigned sr = 0, sg = 0, sb = 0, sn = 0, lr = 0, lg = 0, lb = 0, ln = 0;
    for (int y = 0; y < height; ++y) {
        const unsigned char *src =
            image->pixels + (size_t)(tm->y0 + y) * image->width * 4;
        const unsigned char *mrow =
            tm->mask + (size_t)(tm->y0 - tm->top + y) * tm->bw;
        unsigned char *dst = pixels + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            const unsigned char mark = mrow[tm->x0 - tm->left + x];
            if (!(mark & 8)) continue;
            const unsigned char *pixel = src + (size_t)(tm->x0 + x) * 4;
            dst[x * 4] = pixel[0];
            dst[x * 4 + 1] = pixel[1];
            dst[x * 4 + 2] = pixel[2];
            dst[x * 4 + 3] = 255;
            lr += pixel[0]; lg += pixel[1]; lb += pixel[2]; ln++;
            if (mark & 2) { sr += pixel[0]; sg += pixel[1]; sb += pixel[2]; sn++; }
        }
    }
    if (!sn) { sr = lr; sg = lg; sb = lb; sn = ln; }   /* 全是宽松边缘时退而用整体均值 */
    if (sn) {
        glyph->rgb[0] = (unsigned char)(sr / sn);
        glyph->rgb[1] = (unsigned char)(sg / sn);
        glyph->rgb[2] = (unsigned char)(sb / sn);
    }
    glyph->pixels = pixels;
    glyph->width = width;
    glyph->height = height;
    glyph->left = tm->x0;
    glyph->top = tm->y0;
    return true;
}

/* 在这张键图的气泡矩形内抠出键名贴片; 图里没有文字时返回 false */
bool bongo_cat_bubble_glyph_from_key(const BongoCatImage *image, const float *rect,
    BubbleGlyph *glyph) {
    BubbleTextMask tm;
    if (!text_mask_build(image, rect, &tm)) return false;
    const bool ok = glyph_from_mask(image, &tm, glyph);
    text_mask_free(&tm);
    return ok;
}

/* 把底图气泡里的旧键名抹成填充色, 得到只剩描边与底色的空气泡 */
static void blank_from_mask(BongoCatImage *image, const BubbleTextMask *tm) {
    for (int y = tm->y0; y <= tm->y1; ++y) {
        unsigned char *row = image->pixels + (size_t)y * image->width * 4;
        const unsigned char *mrow = tm->mask + (size_t)(y - tm->top) * tm->bw;
        for (int x = tm->x0; x <= tm->x1; ++x) {
            if (!(mrow[x - tm->left] & 8)) continue;
            unsigned char *pixel = row + (size_t)x * 4;
            pixel[0] = tm->fill[0];
            pixel[1] = tm->fill[1];
            pixel[2] = tm->fill[2];
        }
    }
}

/* 底图那枚键名 (来自首个键自己的图) 的贴片 + 把它的键名从气泡里抹掉。掩码只建一次,
   抠图与抹白共用同一份判定 —— 建两次的话两边的"哪几个像素是文字"就可能对不上。 */
int bongo_cat_bubble_first_glyph(BongoCatImage *base, const float *rect,
    BubbleGlyph *glyphs) {
    BubbleTextMask tm;
    if (!text_mask_build(base, rect, &tm)) return 0;
    int count = 0;
    if (glyph_from_mask(base, &tm, &glyphs[0])) count++;
    blank_from_mask(base, &tm);
    text_mask_free(&tm);
    return count;
}

/* 程序画的 "+": 两道条, 颜色取自模型自己的键名文字色。它不依赖 KpPlus 素材 (不是每个模型
   都有), 尺寸按同一排文字的包围盒算出来, 排进去后不会显得突兀。 */
bool bongo_cat_bubble_glyph_plus(BubbleGlyph *glyph, int size, int thickness,
    const unsigned char rgb[3]) {
    if (size < 3) size = 3;
    if (thickness < 1) thickness = 1;
    if (thickness > size) thickness = size;
    unsigned char *pixels = calloc((size_t)size * (size_t)size * 4, 1);
    if (!pixels) return false;
    const int lo = (size - thickness) / 2, hi = lo + thickness;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const bool horizontal = y >= lo && y < hi;
            const bool vertical = x >= lo && x < hi;
            if (!horizontal && !vertical) continue;
            unsigned char *pixel = pixels + ((size_t)y * size + x) * 4;
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
            pixel[3] = 255;
        }
    glyph->pixels = pixels;
    glyph->width = glyph->height = size;
    glyph->left = glyph->top = 0;
    return true;
}
