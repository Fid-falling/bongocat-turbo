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
 *
 * keycap_only (多键显示的第二通道) 只保留"够亮 + 够彩"的像素: 先丢掉整体偏暗的像素
 * (max < 0.5), 再比饱和度 (max-min)。只比饱和度不行 —— 那是纯相对判据, 近黑像素 (1,0,0)
 * 的 max-min 与 max 都是 1/255, 比值恒等于 1 会被判成"高饱和", 手部描边整片保留、淹没真正
 * 的键帽。加上绝对亮度下限后, 手部笔画与描边是暗的/黑白的 (丢弃), 彩色高亮够亮且 max-min
 * 显著 (保留)。
 *
 * 自带气泡素材的模型 (转换模型) 不走这条通道: 它的气泡里烧死着键名, 靠颜色判据取出来的会是
 * 手部描边而不是气泡。那类模型的多键显示由 overlay_assets.c 在 CPU 侧合成"一个气泡 + A + S"
 * 的整图, 这里只管把合成结果按普通贴图画出去。这段与 Vulkan 侧的 overlay.frag 是同一套规则,
 * 改一边必须改另一边。
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
    "uniform bool erase_left;uniform bool erase_right;uniform bool keycap_only;\n"
    "void main(){color=texture(image,tex);if(color.a<=.001)discard;"
    "if(keycap_only){float mx=max(color.r,max(color.g,color.b));"
    "float mn=min(color.r,min(color.g,color.b));"
    "if(mx<.5||mx-mn<=.45*mx)discard;}"
    "vec2 dl=(tex-vec2(.700,.515))/vec2(.080,.170);"
    "vec2 dr=(tex-vec2(.275,.397))/vec2(.070,.160);"
    "bool l=dot(dl,dl)<1.;bool r2=dot(dr,dr)<1.;"
    "if((erase_left&&l)||(erase_right&&r2))color.rgb=vec3(1);color.rgb*=color.a;}";

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
    /* 多键显示默认打开 (与设置里的默认值一致): 即使还没有人调过
       bongo_cat_overlay_set_key_display, 行为也是"同侧多键都显示"。 */
    value->multi_key = true;
    value->key_limit = BONGO_CAT_MULTI_KEY_LIMIT_DEFAULT;
    /* 默认合并 (与设置里的默认值一致): 一个气泡里用 "+" 连起所有键名 */
    value->bubble_merge = true;
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
    value->keycap_location = value->gl.uniform_location(value->program, "keycap_only");
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

/*
 * 多键显示开关与每侧显示上限 (来自设置)。limit 收在 1..KEYS_CAP: 非 Cubism 构建
 * 与"关掉多键"都退化成每侧只画最早按下的那个键, 由 bongo_cat_overlay_display_limit
 * 统一收口, 这里只管把值钳进合法范围。
 * 值变化时立即对两侧重跑一遍装载: 缩小上限会让超出窗口的键丢掉贴图, 放大则让之前
 * 被挡住的键补位进来 —— 不重跑的话用户改了设置却要等下一次按键才看到变化。
 */
void bongo_cat_overlay_set_key_display(BongoCatOverlay *value, bool multi,
    int limit) {
    if (!value) return;
    if (limit < 1) limit = 1;
    if (limit > BONGO_CAT_OVERLAY_KEYS_CAP) limit = BONGO_CAT_OVERLAY_KEYS_CAP;
    if (value->multi_key == multi && value->key_limit == limit) return;
    value->multi_key = multi;
    value->key_limit = limit;
    bongo_cat_overlay_refresh_keys(value, 0);
    bongo_cat_overlay_refresh_keys(value, 1);
#ifndef BONGO_CAT_HAS_CUBISM
    value->composite_dirty = true;
#endif
}

/*
 * 多键显示的两条画法 (只在自带气泡素材的模型上分叉): true = 合并成一个气泡, 里面用 "+"
 * 把各键名连起来; false = 每个键各占一个气泡并排。切换必须让两侧重新拼 —— 合成图是按上一种
 * 画法画的, 不重拼就会一直是旧样子。这里只是把 dirty 立起来, 真正的重拼在绘制时做
 * (见 overlay_bubble.c 的 refresh_bubble, 那里也要解码整图, 不能每帧干)。
 */
void bongo_cat_overlay_set_bubble_merge(BongoCatOverlay *value, bool merge) {
    if (!value || value->bubble_merge == merge) return;
    value->bubble_merge = merge;
    value->bubble_dirty[0] = true;
    value->bubble_dirty[1] = true;
}
