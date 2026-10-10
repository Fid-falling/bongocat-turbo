#include "overlay_internal.h"

#include <stdlib.h>

/*
 * 叠加层的两个程序以字符串内嵌: 它们是这条渲染路径的内部细节, 内嵌可以避免资源
 * 目录缺失导致桌宠整个画不出来。这两段与 Vulkan 侧的 overlay 着色器/管线是**同一
 * 套语义的两个实现**, 改一边必须改另一边 (参考尺寸缩放、镜像、擦除脚印、预乘)。
 *
 * 顶点着色器: 固定四边形的四角在 NDC 的 -1..1, reference_width/height > 0 时按
 * "贴图像素 / 参考尺寸" 把四边形缩到内容区 (0 表示不缩放, 直接铺满视口), mirror
 * 只翻转 x。片元着色器: 全透明像素直接 discard (避免在透明区写入混合残渣), 两个
 * 擦除椭圆用 uv (0..1) 判定, 命中就把 RGB 拉白 (配合下面的"擦除"混合模式去掉叠加
 * 层里画上去的左右手脚印), 最后把 RGB 预乘 alpha。
 */
static const char *vertex_source =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv;\n"
    "out vec2 tex; uniform bool mirror;\n"
    "uniform sampler2D image; uniform int reference_width,reference_height;\n"
    "void main(){vec2 p=pos;"
    "if(reference_width>0&&reference_height>0){"
    "vec2 extent=vec2(textureSize(image,0))/vec2(reference_width,reference_height);"
    "p=(p+vec2(1,-1))*extent+vec2(-1,1);}"
    "if(mirror)p.x=-p.x;gl_Position=vec4(p,0,1);tex=uv;}";
static const char *fragment_source =
    "#version 330 core\n"
    "in vec2 tex; out vec4 color; uniform sampler2D image;\n"
    "uniform bool erase_left;uniform bool erase_right;\n"
    "void main(){color=texture(image,tex);if(color.a<=.001)discard;"
    "vec2 dl=(tex-vec2(.700,.515))/vec2(.080,.170);"
    "vec2 dr=(tex-vec2(.275,.397))/vec2(.070,.160);"
    "bool l=dot(dl,dl)<1.;bool r=dot(dr,dr)<1.;"
    "if((erase_left&&l)||(erase_right&&r))color.rgb=vec3(1);color.rgb*=color.a;}";

/*
 * 创建顺序与失败语义是有讲究的:
 *  1. 先建 Mver 指针子层 (它是 overlay 的一部分, 失败就整体失败, 不能留一个
 *     "指针层永远画不出来"的 overlay 让用户以为功能坏了);
 *  2. Vulkan 后端直接早退, 只返回一个 gl 全 0 的空壳 —— 于是 destroy 里所有 GL
 *     调用都必须按"对象名非 0"守卫, 否则会调到空函数指针上;
 *  3. GL 路径上任一步失败都走 bongo_cat_overlay_destroy, 而不是各写各的清理:
 *     创建到一半的对象 (mver 子层/程序) 由同一处回收, 避免漏删。
 */
BongoCatOverlay *bongo_cat_overlay_create(BongoCatError *error) {
    BongoCatOverlay *value = calloc(1, sizeof(*value));
    if (!value) return NULL;
    value->mver_pointer = bongo_cat_mver_pointer_overlay_create(error);
    if (!value->mver_pointer) {
        free(value);
        return NULL;
    }
    /* Vulkan 后端没有 OpenGL 上下文: 着色器/VAO/VBO 都不创建, 绘制走
       vk_backend 的管线。 */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) return value;
    if (!bongo_cat_gl_load(&value->gl, error)) {
        bongo_cat_overlay_destroy(value);
        return NULL;
    }
    value->program = bongo_cat_gl_program(&value->gl, vertex_source, fragment_source, error);
    if (!value->program) { bongo_cat_overlay_destroy(value); return NULL; }
    value->mirror_location = value->gl.uniform_location(value->program, "mirror");
    value->image_location = value->gl.uniform_location(value->program, "image");
    value->reference_width_location = value->gl.uniform_location(value->program,
        "reference_width");
    value->reference_height_location = value->gl.uniform_location(value->program,
        "reference_height");
    value->erase_left_location = value->gl.uniform_location(value->program, "erase_left");
    value->erase_right_location = value->gl.uniform_location(value->program, "erase_right");
    const float vertices[] = {-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
    value->gl.gen_vertex_arrays(1, &value->vao);
    value->gl.bind_vertex_array(value->vao);
    value->gl.gen_buffers(1, &value->vbo);
    value->gl.bind_buffer(GL_ARRAY_BUFFER, value->vbo);
    value->gl.buffer_data(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    value->gl.enable_attribute(0);
    value->gl.attribute_pointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, NULL);
    value->gl.enable_attribute(1);
    value->gl.attribute_pointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
        (const void *)(sizeof(float) * 2));
    return value;
}

/*
 * 销毁顺序: 先子层与贴图 (它们可能引用后端资源), 再 GL 对象, 最后释放结构体。
 * 三个 if 守卫不是多余的: Vulkan 后端下 gl 成员全是 NULL 且对象名全为 0, 少了
 * 守卫就会调用空函数指针。
 */
void bongo_cat_overlay_destroy(BongoCatOverlay *value) {
    if (!value) return;
    bongo_cat_mver_pointer_overlay_destroy(value->mver_pointer);
    bongo_cat_overlay_clear_textures(value);
    if (value->vbo) value->gl.delete_buffers(1, &value->vbo);
    if (value->vao) value->gl.delete_vertex_arrays(1, &value->vao);
    if (value->program) value->gl.delete_program(value->program);
    free(value);
}

/*
 * 换模型时调用: 清掉所有贴图与目录信息, 但**不动** program/vao/vbo —— 那些是
 * 一次性创建的 GL 对象, 与具体模型无关, 重建反而要多付一次编译开销。
 * 注意它不会替你清 mver_pointer 子层, 换模型请走 bongo_cat_overlay_load。
 */
void bongo_cat_overlay_clear(BongoCatOverlay *value) {
    if (!value) return;
    bongo_cat_overlay_clear_textures(value);
    bongo_cat_mver_pointer_overlay_clear(value->mver_pointer);
    value->directory[0] = '\0';
}

/* Mver 指针层的总开关: 模型自带指针 (model_pointer_preferred) 时让位, 两者同时
   出现会画出两套指针。三个查询函数都走这个判断, 保证"能不能画"和"要不要喂数据"
   永远一致 —— 否则会出现状态被写了但没人画的隐形状态。 */
bool bongo_cat_overlay_mver_pointer_enabled(const BongoCatOverlay *value) {
    return value && !value->model_pointer_preferred &&
        bongo_cat_mver_pointer_overlay_enabled(value->mver_pointer);
}
bool bongo_cat_overlay_mver_pointer_left_handed(const BongoCatOverlay *value) {
    return bongo_cat_overlay_mver_pointer_enabled(value) &&
        bongo_cat_mver_pointer_overlay_left_handed(value->mver_pointer);
}
/* 输入层每帧调用; 未启用时静默丢弃 (调用方不必自己判断), 这样"模型没开指针层"
   与"指针层没准备好"两条路径的行为完全一样, 不会出现悬空的按键状态。 */
void bongo_cat_overlay_set_mver_pointer(BongoCatOverlay *value,
    float x_ratio, float y_ratio, bool left, bool right, bool side) {
    if (bongo_cat_overlay_mver_pointer_enabled(value))
        bongo_cat_mver_pointer_overlay_set(value->mver_pointer, x_ratio,
            y_ratio, left, right, side);
}
