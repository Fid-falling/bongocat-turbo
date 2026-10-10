#include "mver_pointer_overlay_internal.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <string.h>

/*
 * 顶点布局是三处硬耦合的中间那一处, 改这里必须同步改三处:
 *  - mver_pointer_overlay_internal.h / mver_pointer_overlay.c 里的 attribute_pointer
 *    步长 (8 个 float) 与 uniform_location 拿到的 location 0/1/2;
 *  - OpenGL 侧内嵌着色器的 layout(location=...) 声明;
 *  - Vulkan 侧 pointer 管线的顶点输入状态 (src/render/vk/vk_pipeline.c)。
 * 结构体全是 float 且没有多余成员, 所以可以直接按 8 个 float 平铺给后端。
 */
typedef struct PointerVertex {
    float x, y, u, v;
    float r, g, b, a;
} PointerVertex;

/* 参考空间像素 -> NDC。y 轴要翻转: 几何用像素坐标 (原点左上, 向下增大), NDC 是
   向上为正, 漏掉这个 1.0f - ... 会让整只手臂上下颠倒。这里不做"参考尺寸缩放",
   整个参考空间就是铺满窗口的内容区, 缩放由上层视口负责。 */
static PointerVertex vertex(const BongoCatMverPointerOverlay *value,
    float x, float y, float u, float v, float r, float g, float b, float a) {
    return (PointerVertex){2.0f * x / value->reference_width - 1.0f,
        1.0f - 2.0f * y / value->reference_height, u, v, r, g, b, a};
}

/*
 * Vulkan 没有三角形扇, 三角形带也要靠管线拓扑; 这里统一把两种图元展开成三角形
 * 列表再交给 vk_backend。顶点结构与着色器 location 顺序一致 (8 个 float)。
 */
/* 384 是栈上展开缓冲的顶点预算 (384*8*4 = 12 KB): 本文件最大的图元是描边条带
   (52 顶点) 与手臂填充 (28 顶点), 展开成三角形列表后也就 150 个顶点, 余量充足。
   真超了宁可整块不画也不截断 —— 截断会画出缺角的三角形, 比不画更难看。 */
enum { BONGO_CAT_POINTER_LIST_CAP = 384 };

static bool draw_vulkan(const PointerVertex *vertices, size_t count,
    unsigned int primitive, const BongoCatTexture *texture) {
    if (count > BONGO_CAT_POINTER_LIST_CAP) return false;
    float list[BONGO_CAT_POINTER_LIST_CAP * 8];
    size_t used = 0;
    /* 两种图元的展开公式不同: 带是相邻三点, 扇是以首点为中心。循环边界保证不会
       读到越界顶点; 顶点数不足 3 个时 used 保持 0, 由下面的判断直接放弃这一项。 */
    if (primitive == GL_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < count; ++i) {
            const PointerVertex *triangle[3] = {&vertices[i], &vertices[i + 1],
                &vertices[i + 2]};
            for (size_t v = 0; v < 3; ++v)
                memcpy(&list[(used++) * 8], triangle[v], sizeof(PointerVertex));
        }
    } else {
        for (size_t i = 1; i + 1 < count; ++i) {
            const PointerVertex *triangle[3] = {&vertices[0], &vertices[i],
                &vertices[i + 1]};
            for (size_t v = 0; v < 3; ++v)
                memcpy(&list[(used++) * 8], triangle[v], sizeof(PointerVertex));
        }
    }
    if (!used) return false;
    return bongo_cat_vk_draw_triangles(bongo_cat_vk_current(),
        texture ? texture->vk : NULL, list, (unsigned int)used, texture != NULL);
}

/*
 * 两条后端的共同入口, 语义必须逐条对齐 (预乘 alpha、丢弃全透明), 否则同一个
 * overlay 在 OpenGL 与 Vulkan 下颜色和边缘会不一致。
 * GL 侧每次都重设状态而不假设上一次设过什么: 这个函数被 overlay 的按键绘制夹在
 * 中间调用, 同一帧里别的子系统随时可能改过混合/深度/模板状态。
 */
static void draw(BongoCatMverPointerOverlay *value, unsigned int primitive,
    const PointerVertex *vertices, size_t count,
    const BongoCatTexture *texture) {
    if (!value || !vertices || !count) return;
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        draw_vulkan(vertices, count, primitive, texture);
        return;
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    value->gl.blend_equation(GL_FUNC_ADD);
    value->gl.blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    value->gl.use_program(value->program);
    value->gl.uniform_1i(value->textured_location, texture != NULL);
    value->gl.uniform_1i(value->image_location, 0);
    value->gl.active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture ? texture->gl : 0);
    value->gl.bind_vertex_array(value->vao);
    value->gl.bind_buffer(GL_ARRAY_BUFFER, value->vbo);
    value->gl.buffer_data(GL_ARRAY_BUFFER, sizeof(*vertices) * count,
        vertices, GL_DYNAMIC_DRAW);
    glDrawArrays(primitive, 0, (int)count);
    value->gl.bind_vertex_array(0);
}

/* 按 "贴图像素 * scale" 铺一个四边形 (scale 来自元数据)。纹理没就绪就整块跳过,
   所以可选贴图缺失时不会留下一个空框或纯色块。 */
static void draw_texture(BongoCatMverPointerOverlay *value,
    const BongoCatPointerTexture *texture, float x, float y) {
    if (!bongo_cat_texture_ready(&texture->texture)) return;
    float right = x + texture->width * value->scale;
    float bottom = y + texture->height * value->scale;
    PointerVertex vertices[] = {
        vertex(value, x, y, 0, 0, 1, 1, 1, 1),
        vertex(value, x, bottom, 0, 1, 1, 1, 1, 1),
        vertex(value, right, y, 1, 0, 1, 1, 1, 1),
        vertex(value, right, bottom, 1, 1, 1, 1, 1, 1)
    };
    draw(value, GL_TRIANGLE_STRIP, vertices, 4, &texture->texture);
}

/* 非鼠标模式的贴图优先级: 右键按下 > 左键按下 > 默认设备图。鼠标模式下设备图
   始终要画, 再把按下的键贴图叠上去 —— 所以鼠标贴图必须是"叠加"语义, 不能替换。 */
static void draw_device(BongoCatMverPointerOverlay *value,
    const BongoCatMverPointerGeometry *geometry) {
    if (value->mouse) {
        draw_texture(value, &value->device, geometry->device_x, geometry->device_y);
        if (value->left_down)
            draw_texture(value, &value->left, geometry->device_x, geometry->device_y);
        if (value->right_down)
            draw_texture(value, &value->right, geometry->device_x, geometry->device_y);
        if (value->side_down)
            draw_texture(value, &value->side, geometry->device_x, geometry->device_y);
        return;
    }
    const BongoCatPointerTexture *texture =
        value->right_down && bongo_cat_texture_ready(&value->right.texture)
        ? &value->right
        : value->left_down && bongo_cat_texture_ready(&value->left.texture)
        ? &value->left : &value->device;
    draw_texture(value, texture, geometry->device_x, geometry->device_y);
}

/*
 * uv 按折线外接矩形归一化, 等价于把手臂贴图拉伸铺满整条折线的包围盒: 美术图的
 * 实际像素尺寸不影响几何, 但它必须按参考空间的比例来画图案。
 * 包围盒退化成一条线或一个点 (所有采样点重合) 时直接放弃: 否则 width/height 为 0
 * 会算出 NaN 的 uv, 图元静默消失且没有任何日志可查。
 */
static void draw_arm_fill(BongoCatMverPointerOverlay *value,
    const BongoCatMverPointerGeometry *geometry) {
    float left = geometry->arm[0].x, right = left;
    float top = geometry->arm[0].y, bottom = top;
    for (size_t index = 1; index < BONGO_CAT_MVER_ARM_POINT_COUNT; ++index) {
        left = fminf(left, geometry->arm[index].x);
        right = fmaxf(right, geometry->arm[index].x);
        top = fminf(top, geometry->arm[index].y);
        bottom = fmaxf(bottom, geometry->arm[index].y);
    }
    float width = right - left, height = bottom - top;
    if (width <= 0.0f || height <= 0.0f) return;
    PointerVertex vertices[BONGO_CAT_MVER_ARM_POINT_COUNT + 2];
    float centre_x = left + width * 0.5f, centre_y = top + height * 0.5f;
    vertices[0] = vertex(value, centre_x, centre_y, 0.5f, 0.5f, 1, 1, 1, 1);
    for (size_t index = 0; index <= BONGO_CAT_MVER_ARM_POINT_COUNT; ++index) {
        const BongoCatMverPoint *point =
            &geometry->arm[index % BONGO_CAT_MVER_ARM_POINT_COUNT];
        vertices[index + 1] = vertex(value, point->x, point->y,
            (point->x - left) / width, (point->y - top) / height, 1, 1, 1, 1);
    }
    draw(value, GL_TRIANGLE_FAN, vertices,
        BONGO_CAT_MVER_ARM_POINT_COUNT + 2, &value->arm.texture);
}

/* 30 段折线近似圆 (关节与线帽都是圆), 起始角 -90 度, 用三角形扇提交, 无纹理 */
static void draw_circle(BongoCatMverPointerOverlay *value, float x, float y,
    float radius, float r, float g, float b, float alpha) {
    enum { POINTS = 30 };
    PointerVertex vertices[POINTS + 2];
    vertices[0] = vertex(value, x, y, 0, 0, r, g, b, alpha);
    for (int index = 0; index <= POINTS; ++index) {
        float angle = (float)index * 6.28318530718f / POINTS - 1.57079632679f;
        vertices[index + 1] = vertex(value, x + cosf(angle) * radius,
            y + sinf(angle) * radius, 0, 0, r, g, b, alpha);
    }
    draw(value, GL_TRIANGLE_FAN, vertices, POINTS + 2, NULL);
}

/*
 * 手臂的描边与投影。每一段采样点生成一小条四边形 (按段方向求法线撑开 width/2),
 * 宽度每段递减 0.08, 于是整条手臂往手腕方向逐渐收窄。
 * shadow=true 时颜色换成纯黑、宽度更粗, 作为垫在下面的投影; 由 draw_arm 按
 * "先阴影后描边"的顺序调用, 顺序反了描边会被阴影盖住而显得发灰。
 */
static void draw_line_layer(BongoCatMverPointerOverlay *value,
    const BongoCatMverPointerGeometry *geometry, float width, float alpha,
    bool shadow) {
    PointerVertex edge[BONGO_CAT_MVER_ARM_POINT_COUNT * 2];
    draw_circle(value, geometry->arm[0].x, geometry->arm[0].y, width * 0.5f,
        value->line_red, value->line_green, value->line_blue, alpha);
    for (size_t index = 0; index + 1 < BONGO_CAT_MVER_ARM_POINT_COUNT; ++index) {
        float dx = geometry->arm[index].x - geometry->arm[index + 1].x;
        float dy = geometry->arm[index].y - geometry->arm[index + 1].y;
        float length = hypotf(dx, dy);
        float r = shadow ? 0.0f : value->line_red;
        float g = shadow ? 0.0f : value->line_green;
        float b = shadow ? 0.0f : value->line_blue;
        if (length <= 0.0f) length = 1.0f;
        edge[index * 2] = vertex(value,
            geometry->arm[index].x + dy / length * width * 0.5f,
            geometry->arm[index].y - dx / length * width * 0.5f,
            0, 0, r, g, b, alpha);
        edge[index * 2 + 1] = vertex(value,
            geometry->arm[index].x - dy / length * width * 0.5f,
            geometry->arm[index].y + dx / length * width * 0.5f,
            0, 0, r, g, b, alpha);
        width -= 0.08f;
    }
    size_t last = BONGO_CAT_MVER_ARM_POINT_COUNT - 1;
    float dx = geometry->arm[last].x - geometry->arm[last - 1].x;
    float dy = geometry->arm[last].y - geometry->arm[last - 1].y;
    float length = hypotf(dx, dy);
    if (length <= 0.0f) length = 1.0f;
    edge[last * 2] = vertex(value,
        geometry->arm[last].x - dy / length * width * 0.5f,
        geometry->arm[last].y + dx / length * width * 0.5f,
        0, 0, value->line_red, value->line_green, value->line_blue, alpha);
    edge[last * 2 + 1] = vertex(value,
        geometry->arm[last].x + dy / length * width * 0.5f,
        geometry->arm[last].y - dx / length * width * 0.5f,
        0, 0, value->line_red, value->line_green, value->line_blue, alpha);
    draw(value, GL_TRIANGLE_STRIP, edge,
        BONGO_CAT_MVER_ARM_POINT_COUNT * 2, NULL);
    draw_circle(value, geometry->arm[last].x, geometry->arm[last].y,
        width * 0.5f, value->line_red, value->line_green,
        value->line_blue, alpha);
}

/* 左手模式把 x 比例镜像成 1-x: 皮肤美术通常只画了右手, 靠镜像复用。注意只镜像
   几何, 贴图本身不翻转, 所以贴图里的图案左右要大致对称才不穿帮。 */
static bool geometry(BongoCatMverPointerOverlay *value,
    BongoCatMverPointerGeometry *output) {
    float x = value->left_handed ? 1.0f - value->x_ratio : value->x_ratio;
    return bongo_cat_mver_pointer_geometry(x, value->y_ratio,
        &value->geometry, output);
}

static void draw_arm(BongoCatMverPointerOverlay *value,
    const BongoCatMverPointerGeometry *geometry) {
    draw_arm_fill(value, geometry);
    draw_line_layer(value, geometry, 7.0f, 77.0f / 255.0f, true);
    draw_line_layer(value, geometry, 6.0f, 1.0f, false);
}

/* 入口守卫: 未启用或几何退化就整层跳过, 绝不画半截。鼠标模式的设备图在这里画,
   非鼠标模式留给 after_keys, 两者互斥 (由 value->mouse 决定, 见 draw_device)。 */
void bongo_cat_mver_pointer_overlay_draw_before_keys(
    BongoCatMverPointerOverlay *value) {
    BongoCatMverPointerGeometry current;
    if (!value || !value->enabled || !geometry(value, &current)) return;
    if (value->mouse) draw_device(value, &current);
    draw_arm(value, &current);
}

/* 非鼠标模式的设备图: 必须画在按键之后, 手/手柄才会压在键盘面板之上。
   鼠标模式下这里什么都不做 —— 设备图已经在 before_keys 画过了, 重复画会叠色。 */
void bongo_cat_mver_pointer_overlay_draw_after_keys(
    BongoCatMverPointerOverlay *value) {
    BongoCatMverPointerGeometry current;
    if (!value || !value->enabled || value->mouse ||
        !geometry(value, &current)) return;
    draw_device(value, &current);
}
