/*
 * 顶点装配层: 所有图元的唯一出口都是 vertex()。
 *
 * 这里做三件事, 每一件都影响到"菜单能不能画出来":
 *   1. 顶点缓冲按需翻倍增长, 但有两个硬上限 (524288 个顶点 / 512 个批次)。超限不是
 *      截断而是置 failed —— 宁可报错也不画出半个菜单;
 *   2. 按纹理自动分批: 连续同一纹理的顶点归进同一批, 纹理一变就开新批。所以批次数取决于
 *      "绘制顺序里纹理切换了多少次", 而不是元素个数;
 *   3. 把逻辑坐标变换到窗口坐标: 加中心偏移 -> 应用当前 transform (zoom/offset) ->
 *      乘 d->scale * d->opening。缩放里带上 opening 是刻意的 —— 整个菜单的展开动画
 *      就是靠这里的统一缩放实现的, 不需要每个元素自己算。
 * 顶点色是 0xAARRGGBB, alpha 还要再乘 p->alpha (整菜单的淡入)。
 */
#include "dial_internal.h"
#include <stdlib.h>

static void vertex(Dial *d, unsigned int texture, float x, float y, float u, float v,
    uint32_t color) {
    DialPaint *p = &d->paint;
    if (p->failed) return;
    if (p->count == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 32768;
        if (capacity > 524288) { p->failed = true; return; }
        void *memory = realloc(p->vertices, capacity * sizeof(*p->vertices));
        if (!memory) { p->failed = true; return; }
        p->vertices = memory; p->capacity = capacity;
    }
    if (!p->batch_count || p->batches[p->batch_count-1].texture != texture) {
        if (p->batch_count == 512) { p->failed = true; return; }
        p->batches[p->batch_count++] = (DialBatch){texture, (int)p->count, 0};
    }
    float scale = d->scale * d->opening;
    p->vertices[p->count++] = (DialVertex){
        (float)d->width / 2 + (x*p->zoom+p->offset_x)*scale,
        (float)d->height / 2 + (y*p->zoom+p->offset_y)*scale, u, v,
        (uint8_t)(color>>16), (uint8_t)(color>>8), (uint8_t)color,
        (uint8_t)((float)(color>>24)*p->alpha)};
    p->batches[p->batch_count-1].count++;
}

/* dial_triangle 的专用形态: 用白点纹理 + 无 UV, 也就是"纯色三角形" */
void dial_triangle(Dial *d, DialPoint a, DialPoint b, DialPoint c,
    uint32_t ca, uint32_t cb, uint32_t cc) {
    vertex(d,d->paint.white,a.x,a.y,0,0,ca);
    vertex(d,d->paint.white,b.x,b.y,0,0,cb);
    vertex(d,d->paint.white,c.x,c.y,0,0,cc);
}

/* 纹理四边形 (两个三角形)。UV 由调用方给, 所以同一函数既能整图显示也能取子区域 */
void dial_quad(Dial *d, unsigned int texture, float x, float y, float w, float h,
    float u0, float v0, float u1, float v1, uint32_t color) {
    vertex(d,texture,x,y,u0,v0,color);
    vertex(d,texture,x+w,y,u1,v0,color);
    vertex(d,texture,x+w,y+h,u1,v1,color);
    vertex(d,texture,x,y,u0,v0,color);
    vertex(d,texture,x+w,y+h,u1,v1,color);
    vertex(d,texture,x,y+h,u0,v1,color);
}

/*
 * 一条线段的"条带化"填充: 沿法线方向从 inner 铺到 outer, 拼成两个三角形。
 * 描边的抗锯齿就是靠它实现的 —— 主体用 [−w/2, +w/2], 两侧再各加一条从边缘到
 * 全透明的过渡带 (smooth 分支), 相当于手写的 1px 羽化。过渡带的宽度 edge 会按
 * 当前缩放反算 (见 dial_stroke), 保证屏幕上始终是大约 0.7 像素。
 */
static void strip(Dial *d, DialPoint a, DialPoint b, float nx, float ny,
    float inner, float outer, uint32_t ci, uint32_t co) {
    DialPoint a0 = {a.x+nx*inner,a.y+ny*inner}, a1 = {a.x+nx*outer,a.y+ny*outer};
    DialPoint b0 = {b.x+nx*inner,b.y+ny*inner}, b1 = {b.x+nx*outer,b.y+ny*outer};
    dial_triangle(d,a0,b0,b1,ci,ci,co);
    dial_triangle(d,a0,b1,a1,ci,co,co);
}

/* 实心圆 (20 段扇形)。用它而不是贴图: 圆点尺寸随动画变, 贴图会被拉伸模糊 */
void dial_dot(Dial *d, float x, float y, float r, uint32_t color) {
    const int count = 20;
    for (int i = 0; i < count; ++i) {
        float a = (float)i*2*DIAL_PI/(float)count, b = (float)(i+1)*2*DIAL_PI/(float)count;
        DialPoint p = {x+r*cosf(a),y+r*sinf(a)}, q = {x+r*cosf(b),y+r*sinf(b)};
        dial_triangle(d,(DialPoint){x,y},p,q,color,color,color);
    }
}

/*
 * 沿折线画带宽度的线 (菜单里所有描边都走它)。
 * smooth 为真时额外做三件事: 两侧加羽化过渡带、两端各补一个圆点当圆头。
 * 端点补圆是必要的: 条带只在垂直于线段的方向铺开, 折线拐角处会留缺口, 补圆之后
 * 拐角才是连续的。
 * edge 反算自 d->scale*d->opening, 所以菜单缩放/展开过程中羽化宽度在屏幕上看起来是
 * 恒定的 —— 直接用固定值的话, 缩放动画里描边会一会儿粗一会儿细。
 */
void dial_stroke(Dial *d, const DialPoint *points, int count, float width,
    uint32_t color, bool smooth) {
    float edge = .7f / fmaxf(.1f, d->scale*d->opening);
    for (int i = 1; i < count; ++i) {
        DialPoint a = points[i-1], b = points[i];
        float dx = b.x-a.x, dy = b.y-a.y, length = hypotf(dx,dy);
        if (length < .001f) continue;
        float nx = -dy/length, ny = dx/length;
        strip(d,a,b,nx,ny,-width/2,width/2,color,color);
        if (smooth) {
            strip(d,a,b,nx,ny,width/2,width/2+edge,color,color&0xffffff);
            strip(d,a,b,nx,ny,-width/2,-width/2-edge,color,color&0xffffff);
        }
    }
    if (smooth && count > 1) {
        dial_dot(d,points[0].x,points[0].y,width/2,color);
        dial_dot(d,points[count-1].x,points[count-1].y,width/2,color);
    }
}
