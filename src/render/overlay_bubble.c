#include "overlay_internal.h"
#include "bongo_cat/image.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 气泡合成/拉宽的调试日志: 走全局 SDL 日志, DEBUG 级别 (Debug 构建下会进 BongoCat.log;
   Release 构建按日志级别被过滤)。判定与数值都由这个函数统一输出。 */
void bongo_cat_bubble_debug(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    SDL_LogDebug(BONGO_CAT_LOG_UPDATE, "%s", line);
}

/*
 * 自带气泡素材的模型 (转换模型: 每键一张"气泡 + 手"的整图, 气泡固定画在同一位置、只有
 * 里面的键名随键变化) 在"同侧多键"时有两种画法, 由设置选 (settings.model.bubble_merge):
 *
 *  - 合并 (bubble_compose_merged): 只画**一个**气泡, 里面把所有键名用 "+" 连起来 ("A + S")。
 *  - 单独 (bubble_compose_separate): 每个键各占一个气泡, 并排排开, 气泡里的键名还是模型
 *    自己画好的那一个 —— 与只按这一个键时看到的一模一样。
 *
 * 两种都必须走 CPU 合成, 而不是给每个键各画一次整图: 整图里带着手, 画 N 次就是 N 只手。
 * 所以底图统一取**首个键**那张 (手 + 它自己的气泡), 其余键只往这张底图上加东西。
 *
 * 合并模式为什么必须拼字: 一张贴图塞不下 "A + S" 这段文本。做法是把气泡里的键名抹成
 * 填充色得到空气泡, 再从每张键图里把"键名文字"那部分抠成小贴片, 按 "键名 + 键名" 的次序
 * 排进空气泡。字符本身来自模型自己的美术, 所以字号与颜色与单键时天然一致。
 */

/* 键名文字的抠取 (掩码算法 + 贴片 + 程序画的 "+") 全在 overlay_bubble_glyph.c: 那里判定
   "哪几个像素是键名", 这里只管把贴片排进气泡。 */

/* 把贴片按 scale 缩放 (最近邻) 贴到 (dx, dy), 只覆盖贴片不透明处。目标像素裁在气泡矩形内:
   气泡外的部分是透明画布, 文字溢出去会比"稍微挤一点"难看得多。 */
static void glyph_blit(BongoCatImage *base, const BubbleGlyph *glyph,
    int dx, int dy, float scale, const float *rect) {
    const int dw = (int)((float)glyph->width * scale + 0.5f);
    const int dh = (int)((float)glyph->height * scale + 0.5f);
    if (dw < 1 || dh < 1) return;
    int bounds[4];
    bongo_cat_bubble_rect_pixels(base, rect, bounds);
    ++bounds[2];
    const int cx0 = bounds[0], cy0 = bounds[1];
    const int cx1 = bounds[2] < base->width ? bounds[2] : base->width;
    const int cy1 = bounds[3] < base->height ? bounds[3] : base->height;
    for (int y = 0; y < dh; ++y) {
        const int ty = dy + y;
        if (ty < cy0 || ty >= cy1) continue;
        int sy = (int)((float)y / scale);
        if (sy >= glyph->height) sy = glyph->height - 1;
        for (int x = 0; x < dw; ++x) {
            const int tx = dx + x;
            if (tx < cx0 || tx >= cx1) continue;
            int sx = (int)((float)x / scale);
            if (sx >= glyph->width) sx = glyph->width - 1;
            const unsigned char *source =
                glyph->pixels + ((size_t)sy * glyph->width + sx) * 4;
            if (source[3] <= 8) continue;
            unsigned char *target =
                base->pixels + ((size_t)ty * base->width + tx) * 4;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
        }
    }
}

/* 开头那枚键名 (来自底图自己) 的贴片与"抹掉旧键名"都由 bongo_cat_bubble_first_glyph 一并
   完成 (掩码只建一次, 抠图与抹白共用同一份判定), 这里只负责排版。 */

/* 这一排 ("键名 + 键名 + ...") 的总宽度与共用度量: 字高决定间距 (12%) 与加号边长 (68%)。
   估算气泡要拉多宽、正式排版、整体缩放这三处必须用同一套数字 —— 否则估出来的宽度和排完的
   实际宽度对不上, 气泡要么留一大块空白要么缩得多余。 */
static int merged_span(const BubbleGlyph *glyphs, int count, int *glyph_h, int *gap) {
    int h = 0;
    for (int i = 0; i < count; ++i)
        if (glyphs[i].height > h) h = glyphs[i].height;
    const int g = h * 12 / 100;
    int total = 0;
    for (int i = 0; i < count; ++i) {
        total += glyphs[i].width;
        if (i + 1 < count) total += h * 68 / 100;
    }
    total += g * (count * 2 - 2);
    *glyph_h = h;
    *gap = g;
    return total;
}

/* 把气泡横向拉宽 dx 像素: 画布尺寸不变, 气泡的右边界外移 (多出来的部分压在原图右侧的画面上)。
   **三段式**: 左段 (尾巴 + 左圆角) 与右段 (右圆角) 在拉宽时**原样保留** (整段平移), 只有中间
   [start, mid) 这一段横向**平滑重采样**到 [start, target_right)。这样气泡的两端圆弧不会变形,
   只有中段的描边/填充被拉长 —— 就是"左端不动、中间填充、右端不动"的效果。
   重采样在接缝处是恒等映射 (输出第 0 列取的就是输入第 0 列), 描边、填充在接缝处天然连续。

   实际拉宽量可能小于请求值: 要么撞到其他美术 (手/花瓣就在气泡右边), 要么撞到画布右边界 ——
   所以逐列量一下"气泡这几行里、气泡右侧还有多少列是全透明的", 在那里封顶, 并把真正生效的
   dx 回给调用方 (它要按这个宽度排版文字)。 */
static int bubble_stretch(BongoCatImage *image, const float *rect, int dx) {
    int bounds[4];
    bongo_cat_bubble_rect_pixels(image, rect, bounds);
    const int x0 = bounds[0], x1 = bounds[2], y0 = bounds[1], y1 = bounds[3];
    const int width = x1 - x0, height = y1 - y0;
    if (dx <= 0 || width < 8 || height < 8) return 0;
    if (x1 + dx > image->width) dx = image->width - x1;
    /* 探测"气泡右侧的空地"要从 x1+1 开始: x1 是 rect 右边界所在列, 气泡自己的右描边
       往往正好延伸到这一列, 从它开始会把气泡本体误认为"已占用", room 恒为 0 —— 气泡就
       永远拉不开宽。从下一列开始, 量到的才是真正还能拉宽进去的透明区域。 */
    int room = 0;
    for (int x = x1 + 1; x < image->width; ++x) {
        bool occupied = false;
        for (int y = y0; y < y1 && !occupied; ++y)
            occupied = image->pixels[((size_t)y * image->width + x) * 4 + 3] > 1;
        if (occupied) break;
        room++;
    }
    if (dx > room) dx = room;
    bongo_cat_bubble_debug("  stretch in: x %d..%d y %d..%d imgW=%d room=%d dx=%d",
        x0, x1, y0, y1, image->width, room, dx);
    if (dx <= 0) return 0;
    const int seg = width * 30 / 100;   /* 左端圆角段 (含尾巴): 原样保留 */
    const int seg_r = width * 30 / 100; /* 右端圆角段: 原样保留 */
    const int start = x0 + seg;
    const int mid_end = x1 - seg_r;           /* 中段右界(源) */
    if (seg < 1 || seg_r < 1 || mid_end - start < 1) return 0;
    const int source_width = mid_end - start;   /* 参与放大的中段源宽度 */
    const int target_right = start + source_width + dx;  /* 中段目标右界 */
    unsigned char *band = malloc((size_t)source_width * (size_t)height * 4);
    unsigned char *right_end = malloc((size_t)seg_r * (size_t)height * 4);
    if (!band || !right_end) {
        free(band);
        free(right_end);
        return 0;
    }
    const size_t stride = (size_t)image->width * 4;
    /* 先把中段源和右圆角都拷到内存: 重采样中段时写的位置可能恰好落在右圆角源上面, 必须先
       备份右端, 最后再原样写回, 否则顺序颠倒会把圆角覆盖掉。 */
    for (int y = 0; y < height; ++y) {
        const unsigned char *src_row =
            image->pixels + (size_t)(y0 + y) * image->width * 4;
        memcpy(band + (size_t)y * (size_t)source_width * 4,
            src_row + (size_t)start * 4, (size_t)source_width * 4);
        memcpy(right_end + (size_t)y * (size_t)seg_r * 4,
            src_row + (size_t)mid_end * 4, (size_t)seg_r * 4);
    }
    /* 中段平滑重采样拉宽到 [start, target_right) */
    for (int y = y0; y < y1; ++y) {
        unsigned char *row = image->pixels + (size_t)y * stride;
        const unsigned char *line = band + (size_t)(y - y0) *
            (size_t)source_width * 4;
        const int target_width = target_right - start;
        for (int i = 0; i < target_width; ++i) {
            int sx = (int)((long long)i * source_width / target_width);
            if (sx >= source_width) sx = source_width - 1;
            const unsigned char *source = line + (size_t)sx * 4;
            unsigned char *target = row + (size_t)(start + i) * 4;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
            target[3] = source[3];
        }
    }
    /* 右圆角原样平移到 [target_right, target_right + seg_r) */
    for (int y = y0; y < y1; ++y) {
        unsigned char *row = image->pixels + (size_t)y * stride;
        memcpy(row + (size_t)target_right * 4,
            right_end + (size_t)(y - y0) * (size_t)seg_r * 4,
            (size_t)seg_r * 4);
    }
    free(band);
    free(right_end);
    return dx;
}

/* 合并模式的排版: 各键名贴片横向排开, 键名按**基线**对齐 (这些键名都没有下伸部, 包围盒
   底边就是基线), 每个 "+" 对齐到这一排文字的中线高度。整排宽度是逐贴片累加出来的, 连 "+"
   与前后的间隙一起算; 调用方已按这个宽度把气泡拉宽过, 所以正常情况下 scale 就是 1 —— 只有
   拉到上限还不够时 (键名太多太长) 才整体缩小兜底。 */
static void merged_layout(BongoCatImage *base, const float *rect, BubbleGlyph *glyphs,
    int count, BubbleGlyph *plus, int plus_count) {
    int glyph_h = 0, gap = 0;
    const int total = merged_span(glyphs, count, &glyph_h, &gap);
    const int baseline = glyphs[0].top + glyphs[0].height;
    const int rx0 = (int)(rect[0] * base->width);
    int rx1 = (int)(rect[2] * base->width);
    if (rx1 > base->width) rx1 = base->width;
    const int avail = (rx1 - rx0) * 96 / 100;
    float scale = 1.0f;
    if (total > avail && total > 0) scale = (float)avail / (float)total;
    float pen = (float)rx0 + ((float)(rx1 - rx0) - (float)total * scale) / 2.0f;
    for (int i = 0; i < count; ++i) {
        const int dh = (int)((float)glyphs[i].height * scale + 0.5f);
        glyph_blit(base, &glyphs[i], (int)pen, baseline - dh, scale, rect);
        pen += (float)(int)((float)glyphs[i].width * scale + 0.5f) +
            (float)gap * scale;
        if (i < plus_count) {
            const int pdh = (int)((float)plus[i].height * scale + 0.5f);
            glyph_blit(base, &plus[i], (int)pen,
                baseline - glyph_h * 36 / 100 - pdh / 2, scale, rect);
            pen += (float)(int)((float)plus[i].width * scale + 0.5f) +
                (float)gap * scale;
        }
    }
}

/* 合并模式: 一个气泡 + "A + S"。键名多了气泡就跟着变宽 —— 先把整排文字的宽度估出来,
   按这个宽度把气泡中段拉长 (见 bubble_stretch), 再在拉宽后的矩形里排版; 拉宽量撞到上限
   (右边是手/花瓣或画布边界) 时, merged_layout 会整体缩小兜底, 绝不让文字溢出气泡。
   失败时为"画不出这个气泡" (素材缺失/内存不足), 调用方退回只画首个键 —— 不要画半个气泡。 */
static bool bubble_compose_merged(BongoCatOverlay *value, int side, int wanted,
    BongoCatImage *out, const float *rect) {
    const OverlaySide *hand = &value->sides[side];
    /* 贴片表: 最多 wanted 个键名 + (wanted-1) 个 "+", 后半段给 "+" 用 */
    BubbleGlyph *glyphs = calloc((size_t)wanted * 2, sizeof(*glyphs));
    if (!glyphs) return false;
    int count = bongo_cat_bubble_first_glyph(out, rect, glyphs);
    char path[BONGO_CAT_PATH_CAP];
    BongoCatError ignored = {0};
    for (int i = 1; i < wanted && count < wanted; ++i) {
        if (!bongo_cat_overlay_key_path(value, side, hand->keys[i].name, path)) continue;
        BongoCatImage key = {0};
        if (bongo_cat_image_load(path, &key, &ignored) != BONGO_CAT_OK) continue;
        if (bongo_cat_bubble_glyph_from_key(&key, rect, &glyphs[count])) count++;
        bongo_cat_image_free(&key);
    }
    BubbleGlyph *plus = glyphs + count;
    int plus_count = 0;
    bool ok = count >= 2;
    bongo_cat_bubble_debug("  merge glyphs: count=%d wanted=%d", count, wanted);
    if (ok) {
        int glyph_h = 0;
        for (int i = 0; i < count; ++i)
            if (glyphs[i].height > glyph_h) glyph_h = glyphs[i].height;
        for (int i = 0; i + 1 < count && plus_count < count; ++i) {
            if (!bongo_cat_bubble_glyph_plus(&plus[plus_count], glyph_h * 68 / 100,
                    glyph_h * 10 / 100, glyphs[0].rgb)) break;
            plus_count++;
        }
        ok = plus_count == count - 1;
    }
    if (ok) {
        int glyph_h = 0, gap = 0;
        const int span = merged_span(glyphs, count, &glyph_h, &gap);
        float layout_rect[4] = {rect[0], rect[1], rect[2], rect[3]};
        const int rx0 = (int)(rect[0] * out->width);
        int rx1 = (int)(rect[2] * out->width);
        if (rx1 > out->width) rx1 = out->width;
        /* 文字占拉宽后气泡宽度的 75%, 两侧各留一成多的边距 —— 与单键时"字母浮在气泡
           中间"的观感一致 */
        const int need = span * 100 / 75;
        int dx = need - (rx1 - rx0);
        const int cap = (rx1 - rx0) * 2;   /* 最多拉到 3 倍宽, 再宽就不像气泡了 */
        if (dx > cap) dx = cap;
        bongo_cat_bubble_debug("  merge: count=%d plus=%d span=%d need=%d bubbleW=%d dx_req=%d cap=%d",
            count, plus_count, span, need, rx1 - rx0, dx, cap);
        dx = bubble_stretch(out, rect, dx);
        if (dx > 0) layout_rect[2] = rect[2] + (float)dx / (float)out->width;
        bongo_cat_bubble_debug("  merge: dx_applied=%d layout x=%.4f..%.4f", dx,
            (double)layout_rect[0], (double)layout_rect[2]);
        merged_layout(out, layout_rect, glyphs, count, plus, plus_count);
    }
    for (int i = 0; i < count; ++i) bongo_cat_bubble_glyph_free(&glyphs[i]);
    for (int i = 0; i < plus_count; ++i) bongo_cat_bubble_glyph_free(&plus[i]);
    free(glyphs);
    return ok;
}

/* 单独模式里相邻两个气泡的横向间距 (归一化)。模型的气泡本来就画在左上角, 所以要保证
   排开后最右一个仍在画布内 —— 间距 = "气泡宽 + 一点缝", 但不越过 (右侧余量 / (n-1)),
   否则最后一个气泡会被裁掉一条边。 */
static float bubble_step(const BongoCatOverlay *value, int side, int count) {
    const float x0 = value->bubble_rect[side][0];
    const float width = value->bubble_rect[side][2] - x0;
    float step = width + 0.012f;
    if (count > 1) {
        const float even = (1.0f - x0 - width) / (float)(count - 1);
        if (even < step) step = even;
        if (step < 0.0f) step = 0.0f;
    }
    return step;
}

/* 把源图气泡矩形那一块 (含黑描边、白底、尾巴与键名) 整体横移到 dx 处贴到底图上 */
static void bubble_copy(BongoCatImage *base, const BongoCatImage *source,
    const float *rect, int dx) {
    int bounds[4];
    bongo_cat_bubble_rect_pixels(source, rect, bounds);
    for (int y = bounds[1]; y < bounds[3] && y < base->height; ++y) {
        const unsigned char *src =
            source->pixels + (size_t)y * source->width * 4;
        unsigned char *dst = base->pixels + (size_t)y * base->width * 4;
        for (int x = bounds[0]; x < bounds[2]; ++x) {
            const int tx = x + dx;
            if (tx < 0 || tx >= base->width) continue;
            const unsigned char *sp = src + (size_t)x * 4;
            if (sp[3] <= 8) continue;
            unsigned char *dp = dst + (size_t)tx * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
}

/* 单独模式: 每个键各占一个气泡并排。底图仍是首个键那张 (手 + 它自己的气泡), 其余键只把
   气泡那一块平移贴上去 —— 手不会重复, 每个气泡里的键名仍是模型原来画好的那一个。 */
static bool bubble_compose_separate(BongoCatOverlay *value, int side, int wanted,
    BongoCatImage *out, const float *rect) {
    const OverlaySide *hand = &value->sides[side];
    const int shift = (int)(bubble_step(value, side, wanted) * (float)out->width + 0.5f);
    char path[BONGO_CAT_PATH_CAP];
    BongoCatError ignored = {0};
    bool placed = false;
    for (int i = 1; i < wanted; ++i) {
        if (!bongo_cat_overlay_key_path(value, side, hand->keys[i].name, path)) continue;
        BongoCatImage key = {0};
        if (bongo_cat_image_load(path, &key, &ignored) != BONGO_CAT_OK) continue;
        if (key.width == out->width && key.height == out->height) {
            bubble_copy(out, &key, rect, shift * i);
            placed = true;
        }
        bongo_cat_image_free(&key);
    }
    return placed;
}

/* 拼出这一侧的多键整图 (手 + 气泡), 按设置选合并 (一个气泡) 还是单独 (每键一个) */
static bool bubble_compose(BongoCatOverlay *value, int side, BongoCatImage *out) {
    const OverlaySide *hand = &value->sides[side];
    const int limit = bongo_cat_overlay_display_limit(value);
    int wanted = (int)hand->count;
    if (wanted > limit) wanted = limit;
    if (wanted < 2 || limit < 2) return false;
    const float *rect = value->bubble_rect[side];
    bongo_cat_bubble_debug("compose side=%d count=%zu limit=%d wanted=%d merge=%d rect=[%.4f %.4f %.4f %.4f] first=%s",
        side, hand->count, limit, wanted, value->bubble_merge, (double)rect[0],
        (double)rect[1], (double)rect[2], (double)rect[3], hand->keys[0].name);
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_overlay_key_path(value, side, hand->keys[0].name, path)) return false;
    BongoCatError ignored = {0};
    if (bongo_cat_image_load(path, out, &ignored) != BONGO_CAT_OK) {
        bongo_cat_bubble_debug("compose: 首键整图载入失败 %s", path);
        return false;
    }
    const bool ok = value->bubble_merge
        ? bubble_compose_merged(value, side, wanted, out, rect)
        : bubble_compose_separate(value, side, wanted, out, rect);
    if (!ok) {
        bongo_cat_image_free(out);
        return false;
    }
    return true;
}

/*
 * 该侧是否有一张可用的多键合成图 (需要时重建)。重建由 dirty 驱动而不是每帧: 拼一次要
 * 解码最多 key_limit 张 1400×1400 整图, 每帧做会明显掉帧; 代价是失败后要等下一次按键变化
 * 或设置变化才重试 (与 non-Cubism 的 refresh_composite 同一个取舍)。
 * 前置条件不成立时**不**动 dirty 也不动贴图: 那些是"这一侧本来就不该有气泡"的情形 (单键、
 * 关了多键、非 Cubism、模型没有气泡素材), 与"拼失败"不是一回事。
 */
bool bongo_cat_overlay_refresh_bubble(BongoCatOverlay *value, int side) {
    if (!value || (side != 0 && side != 1)) return false;
    /* 临时诊断: 这条路径每帧都会被调用, 只在"原因变了"时记一行, 免得把日志刷爆 */
    static int logged[2] = {-1, -1};
    const int limit = bongo_cat_overlay_display_limit(value);
    const size_t count = value->sides[side].count;
    int reason = 0;
    if (limit < 2) reason = 1;
    else if (count < 2) reason = 2;
    else if (!bongo_cat_overlay_side_bubble(value, side)) reason = 3;
    else if (!value->bubble_dirty[side]) reason = 4;
    if (reason != 0) {
        if (logged[side] != reason)
            bongo_cat_bubble_debug("refresh side=%d limit=%d count=%zu merge=%d -> 早退 %d (%s)",
                side, limit, count, value->bubble_merge, reason,
                reason == 1 ? "多键上限<2" : reason == 2 ? "该侧按键数<2" :
                reason == 3 ? "没探测到气泡素材" : "不脏, 复用旧图");
        logged[side] = reason;
        if (reason != 4) return false;
        return bongo_cat_texture_ready(&value->bubble_composite[side]);
    }
    logged[side] = 0;
    value->bubble_dirty[side] = false;
    BongoCatImage image = {0};
    if (bubble_compose(value, side, &image)) {
        BongoCatError ignored = {0};
        /* premultiply 传 false 与 non-Cubism 的 refresh_composite 一致: GL 的片元着色器
           自己乘 alpha, 上传时就乘会变成 a²; Vulkan 侧则在 overlay.frag 里按推送的
           premultiply 标志乘一次 (绘制点传 true)。 */
        const bool uploaded = bongo_cat_texture_from_image(
            &value->bubble_composite[side], &image, false, &ignored);
        bongo_cat_bubble_debug("refresh side=%d upload=%d msg=%s", side,
            uploaded, ignored.message);
        bongo_cat_image_free(&image);
    } else {
        bongo_cat_bubble_debug("refresh side=%d 合成失败 -> 退回单键整图", side);
        /* 拼不出来就把旧图丢掉: 留着的是上一种按键组合的文字, 比不画更难解释 */
        bongo_cat_texture_release(&value->bubble_composite[side]);
    }
    return bongo_cat_texture_ready(&value->bubble_composite[side]);
}
