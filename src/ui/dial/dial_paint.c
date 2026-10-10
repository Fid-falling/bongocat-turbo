/*
 * 轮盘的渲染后端: 一条极简的 2D 管线 (位置/UV/颜色 三个属性), GL 与 Vulkan 两条腿。
 * 场景层 (dial_scene) 只产出顶点和批次, 本文件负责把它们提交出去 —— 所以 GL 与 Vulkan
 * 的差异全部收敛在这里, 上面的几何/动画代码完全后端无关。
 *
 * 顶点格式是 DialVertex (x,y,u,v,rgba), 由 dial_triangle/dial_quad 组装; 提交时按
 * 纹理分组成 DialBatch (连续区间), 一次 buffer_data 上传 + 每批一个 draw call。
 * 没有索引缓冲: 扇形本身就是三角形列表, 用索引反而更麻烦。
 *
 * Vulkan 分支只能用小写 gl/VAO/program 之外的接口: 菜单窗口在 Vulkan 模式下**没有**
 * GL 上下文, 任何 GL 调用 (包括加载函数) 都会失败 —— 那句
 * "Radial menu GL loader: Missing OpenGL function" 就是这么来的。
 */
#include "dial_internal.h"
#include "bongo_cat/vk_backend.h"
#include <stddef.h>
#include <stdlib.h>

/*
 * 顶点着色器: 只做投影变换 (正交投影由 CPU 算好传入), 不做任何模型变换 ——
 * 变换已经在几何阶段烘进顶点的 xy 里了 (见 dial_transform)。
 */
static const char *vertex_shader =
    "#version 330 core\n"
    "layout(location=0) in vec2 Position; layout(location=1) in vec2 UV;"
    "layout(location=2) in vec4 Color; uniform mat4 Projection;"
    "out vec2 uv; out vec4 color;"
    "void main(){uv=UV;color=Color;gl_Position=Projection*vec4(Position,0,1);}";
static const char *fragment_shader =
    "#version 330 core\n"
    "in vec2 uv; in vec4 color; uniform sampler2D Texture; out vec4 pixel;"
    "void main(){pixel=color*texture(Texture,uv);}";

/*
 * 一次性初始化: 后端资源 + 字体图集 + 根项扇形几何。
 * 两处刻意的安排:
 *   - 根项扇形在这里就建好 (它们的角度只取决于 d->count), 每帧不再重算; 建完立刻检查
 *     triangle_count, 三角化失败就直接报错退出 —— 少了这个检查, 后面会画出一个看不见
 *     的菜单, 排查起来毫无线索;
 *   - 结尾的 glGetError 只在 GL 下做: Vulkan 没有对应的全局错误状态。
 */
bool dial_paint_init(Dial *d) {
    DialPaint *p = &d->paint;
    BongoCatError error = {0};
    /*
     * Vulkan 菜单窗口没有 GL 上下文: 绝不能去加载 GL 函数 (那正是
     * "Radial menu GL loader: Missing OpenGL function" 的来源), 也不建
     * program/VAO/VBO。白点纹理改走后端接口, 登记进纹理表 (p->white 存 id,
     * 与句柄约定一致)。
     */
    if (d->vk) {
        const unsigned char white_pixel[4] = {255, 255, 255, 255};
        BongoCatError dial_error = {0};
        p->white = (GLuint)dial_vk_texture_register(d,
            bongo_cat_vk_texture_from_pixels(d->vk, white_pixel, 1, 1,
                "dial-white", &dial_error));
    } else {
    if (!bongo_cat_gl_load(&p->gl, &error)) return SDL_SetError("Radial menu GL loader: %s",error.message);
    bongo_cat_gl_clear_errors();
    p->program = bongo_cat_gl_program(&p->gl,vertex_shader,fragment_shader,&error);
    if (!p->program) { SDL_SetError("%s",error.message); return false; }
    p->projection = p->gl.uniform_location(p->program,"Projection");
    p->sampler = p->gl.uniform_location(p->program,"Texture");
    p->gl.gen_vertex_arrays(1,&p->vao);
    p->gl.gen_buffers(1,&p->vbo);
    p->gl.bind_vertex_array(p->vao);
    p->gl.bind_buffer(GL_ARRAY_BUFFER,p->vbo);
    p->gl.enable_attribute(0); p->gl.enable_attribute(1); p->gl.enable_attribute(2);
    p->gl.attribute_pointer(0,2,GL_FLOAT,GL_FALSE,sizeof(DialVertex),(void *)offsetof(DialVertex,x));
    p->gl.attribute_pointer(1,2,GL_FLOAT,GL_FALSE,sizeof(DialVertex),(void *)offsetof(DialVertex,u));
    p->gl.attribute_pointer(2,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(DialVertex),(void *)offsetof(DialVertex,r));
    glGenTextures(1,&p->white); glBindTexture(GL_TEXTURE_2D,p->white);
    const uint8_t white[] = {255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    }
    if (!dial_fonts_init(d)) return SDL_SetError("Radial menu font atlas could not be created");
    for (int i = 0; i < d->count; ++i) {
        float angle = -DIAL_PI/2 + (float)i*2*DIAL_PI/(float)d->count;
        float half = DIAL_PI/(float)d->count;
        dial_sector(&p->roots[i],82,184,angle-half,angle+half);
        if (!p->roots[i].triangle_count) return SDL_SetError("Radial menu sector %d could not be triangulated",i);
    }
    if (d->vk) return true; /* Vulkan 侧没有 GL 错误状态可查 */
    GLenum status = glGetError();
    return status == GL_NO_ERROR || SDL_SetError("Radial menu GL initialization error: 0x%x",(unsigned)status);
}

/*
 * 一帧: 清空顶点/批次 -> 让 dial_scene 填 -> 提交。
 * p->alpha 有一个 0.15 的下限 (见英文注释): 完全透明的第一帧在部分平台的原生透明窗口
 * 上收不到指针命中, 菜单会"点不到"。所以开头这 180ms 里菜单至少是 15% 不透明的。
 * p->failed 表示几何超预算 (顶点或批次写满), 这是必须报出来的错误而不是静默丢内容。
 * GL 下投影矩阵把逻辑坐标 (0..width, 0..height) 映到 NDC, y 轴翻转 (屏幕坐标朝下);
 * 混合用分离式 blend_func, 因为 alpha 通道也要按同样规则累加 (否则透明窗口边缘会发黑)。
 */
bool dial_paint_frame(Dial *d) {
    DialPaint *p = &d->paint;
    p->count = 0; p->batch_count = 0;
    /* Never show a completely transparent first frame: it cannot receive
       pointer hits on some native transparent-window implementations. */
    p->alpha = fmaxf(.15f,fminf(1,(float)(SDL_GetTicks()-d->opened_at)/180));
    dial_scene(d);
    if (p->failed) return SDL_SetError("Radial menu geometry budget exceeded");
    if (d->vk) {
        /*
         * Vulkan 菜单窗口: 几何已由 dial_scene() 后端无关地构建好, 这里只提交绘制。
         * 批里存的是纹理 id, 用 dial_vk_texture_lookup 换成绘制需要的句柄。
         * 菜单是透明弹窗, 清屏色必须是全透明。
         */
        bongo_cat_vk_set_clear_color(d->vk, 0, 0, 0, 0);
        if (!bongo_cat_vk_begin_frame(d->vk, d->pixel_width, d->pixel_height))
            return SDL_SetError("Radial menu Vulkan frame failed");
        for (int i = 0; i < p->batch_count; ++i) {
            DialBatch batch = p->batches[i];
            bongo_cat_vk_draw_dial(d->vk,
                dial_vk_texture_lookup(d, batch.texture),
                &p->vertices[batch.first], (unsigned)batch.count,
                d->width, d->height);
        }
        if (!bongo_cat_vk_end_frame(d->vk))
            return SDL_SetError("Radial menu Vulkan present failed");
        return true;
    }

    glViewport(0,0,d->pixel_width,d->pixel_height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); p->gl.blend_equation(GL_FUNC_ADD);
    p->gl.blend_func_separate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    float width = (float)d->width, height = (float)d->height;
    const float projection[16] = {2/width,0,0,0, 0,-2/height,0,0, 0,0,-1,0, -1,1,0,1};
    p->gl.use_program(p->program);
    p->gl.uniform_matrix_4fv(p->projection,1,GL_FALSE,projection);
    p->gl.uniform_1i(p->sampler,0);
    p->gl.active_texture(GL_TEXTURE0);
    p->gl.bind_vertex_array(p->vao);
    p->gl.bind_buffer(GL_ARRAY_BUFFER,p->vbo);
    p->gl.buffer_data(GL_ARRAY_BUFFER,(GLsizeiptr)(p->count*sizeof(*p->vertices)),p->vertices,GL_STREAM_DRAW);
    for (int i = 0; i < p->batch_count; ++i) {
        DialBatch b = p->batches[i];
        glBindTexture(GL_TEXTURE_2D,b.texture);
        glDrawArrays(GL_TRIANGLES,b.first,b.count);
    }
    if (glGetError() != GL_NO_ERROR) return SDL_SetError("Radial menu OpenGL draw failed");
    return SDL_GL_SwapWindow(d->window);
}

/*
 * Vulkan 菜单纹理表: 与设置窗口的界面纹理表同构 (id = 下标 + 1)。
 * 现在只登记/查找/注销句柄, 真正在绘制里使用要等 dial_paint 的 Vulkan 分支落地。
 */
int dial_vk_texture_register(Dial *d, struct BongoCatVkTexture *texture) {
/*
 * 用法约定: register 拿到 id, 批与封面里只存 id, 绘制时用 lookup 换成后端句柄,
 * 销毁时用 release 归还槽位。
 * id 绝不能跨后端使用, 也不能在 release 之后继续用; 表只有 8 格, 满了 register
 * 返回 0 (= 无纹理), 调用方必须能接受 0。
 */
    if (!d || !texture) return 0;
    for (int i = 0; i < 8; ++i) {
        if (d->paint.vk_textures[i]) continue;
        d->paint.vk_textures[i] = texture;
        return i + 1;
    }
    return 0;
}

struct BongoCatVkTexture *dial_vk_texture_lookup(const Dial *d, unsigned int id) {
    if (!d || id == 0 || id > 8) return NULL;
    return d->paint.vk_textures[id - 1];
}

void dial_vk_texture_release(Dial *d, unsigned int id) {
    if (!d || id == 0 || id > 8) return;
    struct BongoCatVkTexture *texture = d->paint.vk_textures[id - 1];
    d->paint.vk_textures[id - 1] = NULL;
    if (texture && d->vk) bongo_cat_vk_texture_destroy(d->vk, texture);
}

void dial_paint_free(Dial *d) {
    DialPaint *p = &d->paint;
/*
 * 释放全部渲染资源。两条路差别很大:
 *   - Vulkan: 纹理对象交给后端销毁 (这里的 id 不是 GL 纹理名, 调 glDeleteTextures 是
 *     无效的), 且 VAO/VBO/Program 本来就是 0;
 *   - GL: 逐个 glDeleteTextures, 再删 VBO/VAO/Program (顺序无所谓, 但都必须在上下文
 *     还当前时做)。
 * 两条路都要清字体图集与 ranges, 并 free 顶点数组 —— 那些是 CPU 内存, 与后端无关。
 */
    if (d->vk) {
        /*
         * Vulkan 菜单: 纹理句柄表里的对象由后端销毁 (glDeleteTextures 在这里是
         * 无效的 —— 那些 id 根本不是 GL 纹理名); VAO/VBO/Program 在 Vulkan 下
         * 本来就是 0, 不需要释放。id 为 0 时 dial_vk_texture_release 直接返回。
         */
        for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i)
            dial_vk_texture_release(d, d->covers[i].texture);
        dial_vk_texture_release(d, (unsigned)p->font_texture);
        if (p->atlas.permanent.alloc) nk_font_atlas_clear(&p->atlas);
        for (int i = 0; i < 4; ++i) free(p->ranges[i]);
        dial_vk_texture_release(d, (unsigned)p->white);
        free(p->vertices);
        return;
    }

    for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i)
        if (d->covers[i].texture) glDeleteTextures(1,&d->covers[i].texture);
    if (p->font_texture) glDeleteTextures(1,&p->font_texture);
    if (p->atlas.permanent.alloc) nk_font_atlas_clear(&p->atlas);
    for (int i = 0; i < 4; ++i) free(p->ranges[i]);
    if (p->white) glDeleteTextures(1,&p->white);
    if (p->vbo) p->gl.delete_buffers(1,&p->vbo);
    if (p->vao) p->gl.delete_vertex_arrays(1,&p->vao);
    if (p->program) p->gl.delete_program(p->program);
    free(p->vertices);
}
