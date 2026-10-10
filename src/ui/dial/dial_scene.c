/*
 * 轮盘的"场景": 把一帧里要画的所有东西按顺序塞进绘制流。
 * 顺序是: 根项 (可能只有当前项) -> 中心 -> 子项。后画的盖在前面, 所以中心永远压在
 * 根项之上, 子项又压在中心之上 —— 三层重叠的观感就是这么来的。
 *
 * 这里所有的坐标都是**逻辑坐标** (以菜单中心为原点, 设计直径 608), 每个元素画之前
 * 用 dial_transform 设置一次自己的缩放/偏移。这就是这套渲染的核心模式:
 * "变换 -> 画 -> 再变换 -> 再画", 没有场景树。
 *
 * 颜色是 0xAARRGGBB 的打包值, 与 dial_triangle 的入参一致; 本文件里的 alpha()/mix()
 * 都是在这个打包格式上做的手工运算, 不要和 nk_color 混用。
 */
#include "dial_internal.h"
#include <stdio.h>

/*
 * 级联展开: 每一项比前一项晚 DIAL_REVEAL_DELAY_MS 开始, 各自用 DIAL_REVEAL_DURATION_MS
 * 走完 cubic ease-out。返回 0..1 的进度, 同时被用作缩放与不透明度 —— 两者共用同一条
 * 曲线才能看起来是"从中心长出来"而不是"淡入的同时缩放"。
 */
static float reveal(uint64_t now, uint64_t started, int index) {
    float elapsed = (float)(now - started) - index * DIAL_REVEAL_DELAY_MS;
    float t = fmaxf(0, fminf(1, elapsed / DIAL_REVEAL_DURATION_MS));
    return 1 - powf(1 - t, 3);
}

static uint32_t alpha(uint32_t color, float opacity) {
    return (color&0xffffff) | ((uint32_t)((float)(color>>24)*opacity)<<24);
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t color = 0;
    for (int shift = 0; shift <= 24; shift += 8) {
        float av = (float)((a>>shift)&255), bv = (float)((b>>shift)&255);
        color |= (uint32_t)(av+(bv-av)*t)<<shift;
    }
    return color;
}
/*
 * 沿"左上 -> 右下"的对角方向做三色渐变 (t 由顶点在包围盒里的相对位置算出):
 * 这样一整块扇形看起来是斜着打的光, 而不是平均一个颜色。
 */
static uint32_t gradient(const DialPath *p, DialPoint v, const uint32_t *colors, float opacity) {
    float t = .5f*((v.x-p->min_x)/(p->max_x-p->min_x)+(v.y-p->min_y)/(p->max_y-p->min_y));
    t = fmaxf(0,fminf(1,t));
    return alpha(t < .5f ? mix(colors[0],colors[1],t*2) : mix(colors[1],colors[2],(t-.5f)*2),opacity);
}
/*
 * 画一块扇形本体: 外发光 (往外描 6 圈逐渐变粗且越来越淡的边) + 渐变填充 + 两道描边。
 * tint: 0 普通 (跟随主题), 1 蓝=悬停, 2 粉=已选中, 3 红=危险项 (退出/关闭这类)。
 * 那圈发光是用"重复描边"堆出来的, 不是模糊: 6 次描边在软件光栅上也够快, 而且不依赖
 * 后处理。发光颜色取渐变中间色并把 alpha 压到很低 (0x06/0x03)。
 * 顶点色只算一次然后按索引复用 (见英文注释): 扇形是共享顶点的三角形扇, 每个顶点算
 * 一遍渐变的开销在 160 个点 x 每帧是能看出来的。
 */
static void surface(Dial *d, const DialPath *p, int tint, float opacity) {
    if (!p->count) return;
    uint32_t colors[3];
    if (tint == 1) { colors[0]=0xff6ec2ff; colors[1]=0xff54aeff; colors[2]=0xff1e82f0; }
    else if (tint == 2) { colors[0]=0xffff97be; colors[1]=0xfff77daa; colors[2]=0xffe34882; }
    else if (tint == 3) { colors[0]=0xffff6960; colors[1]=0xffff453a; colors[2]=0xffd10014; }
    else if (d->dark) { colors[0]=0xff010409; colors[1]=0xff010409; colors[2]=0xff010409; }
    else { colors[0]=0xffffffff; colors[1]=0xfff6f9ff; colors[2]=0xffeef3fc; }
    DialPoint closed[DIAL_PATH_POINTS+1];
    for (int i = 0; i < p->count; ++i) closed[i]=p->points[i];
    closed[p->count]=p->points[0];
    uint32_t glow = tint ? (colors[1]&0xffffff)|0x06000000 : 0x03000000;
    for (int i = 6; i > 0; --i)
        dial_stroke(d,closed,p->count+1,(float)i*5,alpha(glow,opacity),false);
    /* Shared polygon vertices only need their gradient calculated once. */
    uint32_t vertex_colors[DIAL_PATH_POINTS];
    for (int i = 0; i < p->count; ++i)
        vertex_colors[i] = gradient(p,p->points[i],colors,opacity);
    for (int i = 0; i < p->triangle_count; i += 3) {
        int a=p->triangles[i], b=p->triangles[i+1], c=p->triangles[i+2];
        dial_triangle(d,p->points[a],p->points[b],p->points[c],
            vertex_colors[a],vertex_colors[b],vertex_colors[c]);
    }
    dial_stroke(d,closed,p->count+1,1.1f,alpha(tint || !d->dark ? 0xfaffffff : 0x38ffffff,opacity),true);
    dial_stroke(d,closed+p->rim_start,p->count+1-p->rim_start,1.4f,
        alpha(d->dark ? 0xd3ffffff : 0xebffffff,opacity),true);
}
/* 复用 dial_text, 但把"标题/正文"翻译成字号并就地传入颜色 (调用点更短) */
static void text(Dial *d, const char *s, float x, float y, float width, bool title, uint32_t color) {
    dial_text(d,s,x,y,width,title ? 19.0f : 14.0f,color);
}
/*
 * 图标选择里那个小特例: icon 2/3 (播放/暂停、随机这类) 在"已勾选"状态下要换成
 * 12/13 两个变体图标, 用图标本身表达开关状态, 而不是在旁边加个对勾。
 */
static int item_icon(DialItem item) {
    return item.checked && item.icon == 2 ? 12 :
        (item.checked && item.icon == 3 ? 13 : item.icon);
}

/*
 * 一个根项 (环形菜单外圈的一个扇形 + 图标 + 选中角标)。
 * zoom/pull 都由 reveal 进度和 lift (悬停抬起) 组合出来:
 *   pull 为负时是"还没展开, 从中心往外顶"; 展开完成后变成悬停时的 7.5 外推;
 *   pressed 时缩小 4% 并把外推压到 40% —— 这是按下反馈。
 * index 0 的 ease 有 15% 下限 (见英文注释): 原生透明窗口下最上面那一个扇形的
 * 起始位置可能正好落在窗口可点击区域之外, 完全没有下限时展开的头几毫秒点不到它。
 * "index>=6 && <=8 且无子项"时整体降到 40% 不透明度: 这三个是占位/不可用项。
 */
static void root(Dial *d, int index, uint64_t now) {
    DialItem item = d->items[index];
    float angle = -DIAL_PI / 2 + index * 2 * DIAL_PI / d->count;
    float x = 132 * cosf(angle), y = 132 * sinf(angle);
    float ease = reveal(now, d->opened_at, index);
    /* Keep the first sector hittable on transparent native windows. */
    if (index == 0) ease = fmaxf(.15f, ease);
    float lift = d->lift[index], zoom = .72f + .28f * ease + .055f * lift;
    float pull = -26 * (1 - ease) + 7.5f * lift;
    if (d->pressed == index) { zoom *= .96f; pull *= .4f; }
    dial_transform(d, zoom, x * (1 - zoom) + cosf(angle) * pull,
        y * (1 - zoom) + sinf(angle) * pull);
    bool active = index == d->active;
    float opacity = ease;
    int tint = active ? (index >= 10 ? 3 : 1) : 0;
    surface(d, &d->paint.roots[index], tint, opacity);
    uint32_t color = active ? 0xffffffff : (item.checked ? 0xfff77daa : item.color);
    if (index >= 6 && index <= 8 && !item.children) opacity *= .4f;
    dial_icon(d, item_icon(item), x, y, 28 * (1 + .15f * lift), alpha(color, opacity));
    if (item.checked && item.command != BONGO_CAT_MENU_ALWAYS_ON_TOP) {
        for (int i = 4; i > 0; --i)
            dial_dot(d, x + 17, y - 17, 3.2f + i * 1.5f, alpha(0x0af77daa, ease));
        dial_dot(d, x + 17, y - 17, 3.2f, alpha(0xfff77daa, ease));
    }
}

/*
 * 中心的圆形背景 (径向渐变圆盘)。
 * 网格只在第一次调用时生成并缓存在 DialPaint 里 (center_ready): 16 环 x 64 段的顶点
 * 与每环的 alpha 是常量, 但上面的颜色和透明度要跟随主题/开关动画实时变, 所以缓存的是
 * "顶点与 alpha 表", 不是最终颜色 (见英文注释)。
 * alpha 用 smoothstep 从中心到边缘降到全透明, 这样圆盘没有硬边。
 * 相邻两环共享边: 每段画两个三角形拼成四边形, 最内环只画一个 (退化成三角形)。
 */
static void center_background(Dial *d) {
    enum { segments = DIAL_CENTER_SEGMENTS, rings = DIAL_CENTER_RINGS };
    const float radius = 80.0f;
    uint32_t color = d->dark ? 0x00010409 : 0x00f6f9ff;
    DialPaint *p = &d->paint;
    /* Cache the original mesh per menu; transforms and opening alpha stay live. */
    if (!p->center_ready) {
        DialPoint directions[segments];
        for (int i = 0; i < segments; ++i) {
            float angle = (float)i * 2 * DIAL_PI / segments;
            directions[i] = (DialPoint){cosf(angle), sinf(angle)};
        }
        for (int ring = 0; ring <= rings; ++ring) {
            float t = (float)ring / rings;
            p->center_alpha[ring] = (uint8_t)(alpha(0xe6000000,
                1 - t * t * (3 - 2 * t)) >> 24);
            for (int i = 0; i < segments; ++i)
                p->center_points[ring][i] = (DialPoint){
                    radius * t * directions[i].x, radius * t * directions[i].y};
        }
        p->center_ready = true;
    }
    /* Adjacent rings share edges; smoothstep fades to a fully clear rim. */
    for (int ring = 0; ring < rings; ++ring) {
        uint32_t ci = color | ((uint32_t)p->center_alpha[ring] << 24);
        uint32_t co = color | ((uint32_t)p->center_alpha[ring+1] << 24);
        for (int i = 0; i < segments; ++i) {
            int next = (i + 1) % segments;
            DialPoint a = p->center_points[ring][i];
            DialPoint b = p->center_points[ring+1][i];
            DialPoint c = p->center_points[ring+1][next];
            DialPoint e = p->center_points[ring][next];
            dial_triangle(d, a, b, c, ci, co, co);
            if (ring > 0) dial_triangle(d, a, c, e, ci, co, ci);
        }
    }
}

/*
 * 中心内容: 当前根的图标 + 名字, 悬停子项时整块让位给子项的名字与图标。
 * 有分页的子项 (>DIAL_PAGE) 在下面补一行 "当前页/总页数"。
 * child_hovered 时图标不画 —— 位置要留给文字, 硬塞会重叠。
 */
static void center(Dial *d) {
    if (d->active < 0) return;
    dial_transform(d, 1, 0, 0);
    center_background(d);
    DialItem item = d->items[d->active];
    char buffer[32];
    bool child_hovered = d->child >= 0;
    uint32_t color = child_hovered ? 0xfff77daa : 0xff52a9f8;
    const char *label = child_hovered ?
        dial_child_item(d, d->child, buffer, sizeof(buffer)).label : item.label;
    if (!child_hovered)
        dial_icon(d, item_icon(item), 0, -5, 38, item.checked ? 0xfff77daa : item.color);
    text(d, label, 0, child_hovered ? 0.0f : 30.0f, 140, true, color);
    if (item.children > DIAL_PAGE) {
        snprintf(buffer, sizeof(buffer), "%d / %d", d->page + 1,
            ((int)item.children + DIAL_PAGE - 1) / DIAL_PAGE);
        text(d, buffer, 0, 58, 80, false, color);
    }
}

/*
 * 子项 (内圈)。展开方向是顺时针的 (见英文注释): 子项下标随屏幕角度递增, 而 reveal 的
 * 延迟随下标递增, 两者一致才不会看起来"从中间往两边长"。
 * 有封面的子项 (模型列表) 优先画封面, 画不出来才退回文字 —— 这样封面加载失败时不会
 * 留下一个空扇形。
 * 文字宽度按"子项之间的角间距"切换 (dial_child_step > .4 时用宽版 92, 否则 62):
 * 项多时列窄, 用宽版文字会互相压。
 */
static void children(Dial *d, uint64_t now) {
    for (int i = 0; i < dial_child_count(d); ++i) {
        /* Child indices increase with screen angle, so this reveals clockwise. */
        float ease = reveal(now, d->changed_at, i);
        float angle = dial_child_angle(d, i), x = 230 * cosf(angle), y = 230 * sinf(angle);
        bool hover = i == d->child;
        float zoom = .72f + .28f * ease + (hover ? .06f : 0);
        float pull = -26 * (1 - ease) + (hover ? 6 : 0);
        dial_transform(d, zoom, x * (1 - zoom) + cosf(angle) * pull,
            y * (1 - zoom) + sinf(angle) * pull);
        char buffer[32];
        DialItem item = dial_child_item(d, i, buffer, sizeof(buffer));
        surface(d, &d->paint.children[i],
            item.checked ? 2 : (hover ? 1 : 0), ease);
        uint32_t color = hover || item.checked || d->dark ? 0xffffffff : 0xff181c28;
        if (!dial_cover_draw(d, i, x, y, ease))
            text(d, item.label, x, y, dial_child_step(d) > .4f ? 92.0f : 62.0f,
                false, alpha(color, ease));
    }
}

/*
 * 一帧的绘制入口。child_focus (已经进到某个根项的子菜单) 时, 只画当前那个根项, 其它
 * 根项整个跳过 —— 这样中心区域才空得出来给子项。
 */
void dial_scene(Dial *d) {
    uint64_t now = SDL_GetTicks();
    for (int i = 0; i < d->count; ++i)
        if (!d->child_focus || i == d->active) root(d,i,now);
    center(d);
    children(d,now);
}
