/*
 * 轮盘的几何层: 把"扇形"变成可以直接提交的三角形。
 *
 * 分两步, 这个分工很重要:
 *   1. 先描出扇形的**轮廓** (DialPath.points): 内外两条圆弧 + 四个圆角, 圆角用二次
 *      贝塞尔近似, 圆弧按固定弦长 (3 单位) 分段 —— 所以弧长越长段数越多, 视觉上
 *      曲率一致, 不会出现大弧显得有棱角;
 *   2. 再用 ear clipping (triangulate) 把轮廓剖成三角形。
 * 为什么要自己写三角化: 扇形不是简单多边形 (有 160 个顶点、带圆角), 而这条渲染管线
 * 只吃三角形, 没有 stencil/tessellation 可用。
 *
 * 缓存策略在 dial_child_paths 里: 只有"子项数量/当前根项/根项数量/角间距"任一变化
 * 才重建几何 (翻页不算变化, 见那里的英文注释)。几何放 DialPaint 里, 跨帧活着。
 */
#include "dial_internal.h"
#include <string.h>

/* 极坐标转直角坐标; 角度单位是弧度, 0 度指向 +x (屏幕右), 负角度朝上 */
static DialPoint polar(float r, float a) {
    return (DialPoint){r * cosf(a), r * sinf(a)};
}
/*
 * 追加一个轮廓点。两个保护:
 *   - 与上一点距离小于 0.001 的点直接丢弃: 重复顶点会产生零面积三角形, 让 ear
 *     clipping 卡在找不到"耳朵"的状态;
 *   - 超过 DIAL_PATH_POINTS 直接丢: 数组是定长的, 宁可轮廓略粗糙也不能越界。
 */
static void point(DialPath *p, DialPoint v) {
    if (p->count && hypotf(v.x - p->points[p->count-1].x,
        v.y - p->points[p->count-1].y) < .001f) return;
    if (p->count < DIAL_PATH_POINTS) p->points[p->count++] = v;
}
/*
 * 二次贝塞尔圆角: 从 a 经控制点 c 到 b, 固定 6 段。
 * a/c/b 的顺序是刻意的 (a, c, b) 而不是 (a, b, c) —— 调用点按"圆弧切点 -> 角点 ->
 * 另一侧切点"传参, 与控制点必须夹在中间。
 */
static void quad(DialPath *p, DialPoint a, DialPoint c, DialPoint b) {
    point(p, a);
    for (int i = 1; i <= 6; ++i) {
        float t = (float)i / 6, u = 1 - t;
        point(p, (DialPoint){u*u*a.x + 2*u*t*c.x + t*t*b.x,
            u*u*a.y + 2*u*t*c.y + t*t*b.y});
    }
}
/* 圆弧按"每段约 3 个单位弧长"离散: 半径大就自动多分段, 保证曲率看起来一致 */
static void arc(DialPath *p, float r, float a, float b) {
    int steps = (int)ceilf(fabsf(b-a) * r / 3);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; ++i)
        point(p, polar(r, a + (b-a) * (float)i / (float)steps));
}
static float cross(DialPoint a, DialPoint b, DialPoint c) {
    return (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x);
}
/*
 * ear clipping 三角化。两个细节:
 *   - sign 取自多边形的有向面积, 所以顺/逆时针两种绕序都能处理, 不必要求调用方保证
 *     绕序 (扇形可能因为 start>end 而反向);
 *   - 找不到"耳朵"时直接 break 而不是重试: 说明剩下的轮廓退化/自交了, 这时交出已经
 *     切好的部分三角形 (画面缺一角) 远好于死循环。
 * remaining 用 unsigned short 索引: 顶点数上限就是 DIAL_PATH_POINTS (160), 够用且省栈。
 */
static void triangulate(DialPath *p) {
    unsigned short remaining[DIAL_PATH_POINTS];
    float area = 0;
    for (int i = 0; i < p->count; ++i) {
        remaining[i] = (unsigned short)i;
        DialPoint a = p->points[i], b = p->points[(i+1)%p->count];
        area += a.x*b.y - b.x*a.y;
    }
    float sign = area > 0 ? 1.0f : -1.0f;
    int count = p->count;
    while (count >= 3) {
        bool found = false;
        for (int i = 0; i < count; ++i) {
            int a = remaining[(i+count-1)%count], b = remaining[i], c = remaining[(i+1)%count];
            if (cross(p->points[a], p->points[b], p->points[c])*sign <= .00001f) continue;
            bool inside = false;
            for (int j = 0; j < count; ++j) {
                int v = remaining[j];
                if (v == a || v == b || v == c) continue;
                DialPoint q = p->points[v];
                if (cross(p->points[a], p->points[b], q)*sign >= 0 &&
                    cross(p->points[b], p->points[c], q)*sign >= 0 &&
                    cross(p->points[c], p->points[a], q)*sign >= 0) {
                    inside = true; break;
                }
            }
            if (inside) continue;
            p->triangles[p->triangle_count++] = (unsigned short)a;
            p->triangles[p->triangle_count++] = (unsigned short)b;
            p->triangles[p->triangle_count++] = (unsigned short)c;
            memmove(remaining+i, remaining+i+1, (size_t)(count-i-1)*sizeof(*remaining));
            --count; found = true; break;
        }
        if (!found) break;
    }
}
/*
 * 生成一个"圆角扇形"轮廓: 内弧半径 inner、外弧半径 outer、角度区间 [start, end]。
 * corner = 12 是圆角半径, 除以半径再换成角度 (ci/co) 就是圆角在弧上占的角度 ——
 * 半径越小占的角度越大, 所以内弧的圆角看起来和外弧一样圆。
 * a/b 各向内收 0.024 弧度: 相邻扇形之间留一条缝, 描边才看得清。
 * rim_start 记录"外弧从第几个点开始", dial_scene 用它单独把外缘描得更亮一圈。
 * 最后一个点要丢掉: 它和第一个点重合, point() 的去重也会拦掉, 但这里显式减一是为了
 * 让 count 表示"不重复的顶点数"。
 */
void dial_sector(DialPath *p, float inner, float outer, float start, float end) {
    memset(p, 0, sizeof(*p));
    const float corner = 12, a = start + .024f, b = end - .024f;
    const float ci = corner / inner, co = corner / outer;
    point(p, polar(outer-corner, b));
    quad(p, polar(inner+corner,b), polar(inner,b), polar(inner,b-ci));
    arc(p, inner,b-ci,a+ci);
    quad(p, polar(inner,a+ci),polar(inner,a),polar(inner+corner,a));
    p->rim_start = p->count;
    quad(p,polar(outer-corner,a),polar(outer,a),polar(outer,a+co));
    arc(p,outer,a+co,b-co);
    quad(p,polar(outer,b-co),polar(outer,b),polar(outer-corner,b));
    if (p->count > 1) --p->count; /* Last point closes back onto the first. */
    p->min_x = p->min_y = 1000; p->max_x = p->max_y = -1000;
    for (int i = 0; i < p->count; ++i) {
        p->min_x = fminf(p->min_x, p->points[i].x); p->max_x = fmaxf(p->max_x,p->points[i].x);
        p->min_y = fminf(p->min_y, p->points[i].y); p->max_y = fmaxf(p->max_y,p->points[i].y);
    }
    triangulate(p);
}
/*
 * 重建内圈子项的扇形几何。
 * 缓存键是四个值 (子项数量 / 当前根项 / 根项数量 / 角间距), 任一变化才重建 ——
 * 几何是每帧都要用的, 每次都重算 16 个扇形的三角化明显浪费。
 * 注意"翻页"不在缓存键里 (见英文注释): 一页的内容不影响扇形的位置和形状, 子项总数
 * 变了才会变。所以翻页时复用上一页的几何是对的。
 * count == 0 时直接返回、不清空: 内圈暂时隐藏 (比如从子菜单退出来) 不该丢掉布局,
 * 否则再进去会闪一下重新展开。
 */
void dial_child_paths(Dial *d) {
    int count = dial_child_count(d);
    float step = dial_child_step(d);
    DialPaint *p = &d->paint;
    /* Page contents do not affect geometry. Keep the last layout even when
       the child ring is temporarily hidden; the draw loop uses child_count. */
    if (!count) return;
    if (p->child_path_count == count && p->child_path_active == d->active &&
        p->child_path_roots == d->count && p->child_path_step == step) return;
    memset(d->paint.children, 0, sizeof(d->paint.children));
    for (int i = 0; i < count; ++i) {
        float angle = dial_child_angle(d,i), half = dial_child_step(d)/2;
        dial_sector(&d->paint.children[i],198,262,angle-half,angle+half);
    }
    p->child_path_count = count;
    p->child_path_active = d->active;
    p->child_path_roots = d->count;
    p->child_path_step = step;
}

/*
 * 两个角度之间的最短有向差 (处理 ±π 环绕): 直接相减会在跨越 -π/π 时得到 2π 的假差距,
 * 表现为指针在经过正左方时选中的项突然跳变。
 */
static float angle_distance(float a, float b) {
    return atan2f(sinf(a - b), cosf(a - b));
}

/*
 * 命中测试 —— 全部用极坐标做, 不做逐多边形求交:
 *   1. 先试内圈 (半径 190..277), 只有已经展开子菜单时才算;
 *   2. 再试外圈 (73..198), child_focus 时只有当前根项可命中;
 *   3. 都不中但半径仍在内圈范围内时, 保持当前根项 (见英文注释) —— 指针从扇形缝隙或
 *      中心圆盘上划过时不能让菜单"关掉", 否则手一抖就没了。
 * 判定用的半径区间比画出来的几何略宽 (扇形是 198..262、外圈 73..198), 是刻意留的
 * 容错: 边缘几像素捏不准不该算没点到。
 * 角度判定用"与扇形中心角的差 < 半宽", 所以扇形之间没有空隙 (第 1 步先返回)。
 */
void dial_hit(Dial *d, float x, float y, int *root, int *child) {
    float radius = hypotf(x, y), angle = atan2f(y, x);
    *root = -1; *child = -1;
    if (d->active >= 0 && radius >= 190 && radius <= 277) {
        for (int i = 0; i < dial_child_count(d); ++i) {
            if (fabsf(angle_distance(angle, dial_child_angle(d, i))) <
                dial_child_step(d) / 2) {
                *root = d->active; *child = i; return;
            }
        }
    }
    if (radius >= 73 && radius <= 198) {
        for (int i = 0; i < d->count; ++i) {
            if (d->child_focus && i != d->active) continue;
            float center = -DIAL_PI / 2 + i * 2 * DIAL_PI / d->count;
            if (fabsf(angle_distance(angle, center)) <= DIAL_PI / d->count) {
                *root = i; return;
            }
        }
    }
    /* Keep the iris open while crossing its radial gap or the center hub. */
    if (radius <= 280) *root = d->active;
}

/*
 * 设置"接下来要画的东西"的变换。这不是一个矩阵栈, 只是把 zoom/offset 记在 DialPaint
 * 上, 由 dial_triangle 在生成顶点时应用 —— 所以每次切换绘制对象都要重新调一次, 且
 * 画的顺序就是变换生效的顺序。
 */
void dial_transform(Dial *d, float zoom, float x, float y) {
    d->paint.zoom = zoom;
    d->paint.offset_x = x;
    d->paint.offset_y = y;
}
