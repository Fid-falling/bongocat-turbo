#include "mver_pointer_overlay_internal.h"
#include "bongo_cat/image.h"
#include "bongo_cat/json.h"
#include "bongo_cat/model.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

/*
 * 着色器以字符串内嵌而不是外部文件: 这两个程序是渲染后端内部实现细节, 没有对外
 * 的可定制需求, 内嵌可以避免"资源目录缺失/被改坏"导致指针层整体失效。要改它们
 * 必须同时改 Vulkan 侧 (src/render/vk/shaders/pointer.*), 两边语义必须一致。
 *
 * 顶点格式契约: pos 2f + uv 2f + tint 4f = 8 个 float, 与下面 create 里的
 * attribute_pointer 步长、以及绘制侧的 PointerVertex 必须严格对应; 片元里
 * discard 全透明像素, 并且把 RGB 预乘上 alpha —— 与 OpenGL/Vulkan 两侧的混色
 * 模式 (ONE, ONE_MINUS_SRC_ALPHA) 配套, 少乘一次就会出现亮边。
 */
static const char *vertex_source =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;layout(location=1) in vec2 uv;"
    "layout(location=2) in vec4 tint;out vec2 tex;out vec4 color;"
    "void main(){gl_Position=vec4(pos,0,1);tex=uv;color=tint;}";

static const char *fragment_source =
    "#version 330 core\n"
    "in vec2 tex;in vec4 color;out vec4 outputColor;uniform sampler2D image;"
    "uniform bool textured;void main(){vec4 value=color;"
    "if(textured)value*=texture(image,tex);if(value.a<=.001)discard;"
    "value.rgb*=value.a;outputColor=value;}";

/*
 * 释放全部五张贴图并把句柄整块清零。用 (BongoCatPointerTexture){0} 而不是只调用
 * 释放函数, 是因为句柄里还缓存着 width/height: 只释放不清零会留下"没贴图但有
 * 尺寸"的状态, 绘制侧的 bongo_cat_texture_ready 判断一旦被绕过就会按旧尺寸画。
 * enabled 一并清掉, 保证"没资源"和"要画"这两件事不会同时为真。
 */
static void clear_textures(BongoCatMverPointerOverlay *value) {
    BongoCatPointerTexture *textures[] = {&value->arm, &value->device,
        &value->left, &value->right, &value->side};
    for (size_t index = 0; index < sizeof(textures) / sizeof(textures[0]); ++index) {
        bongo_cat_texture_release(&textures[index]->texture);
        *textures[index] = (BongoCatPointerTexture){0};
    }
    value->enabled = false;
}

BongoCatMverPointerOverlay *bongo_cat_mver_pointer_overlay_create(
    BongoCatError *error) {
    BongoCatMverPointerOverlay *value = calloc(1, sizeof(*value));
    if (!value) return NULL;
    /* Vulkan 后端没有 GL 上下文: 着色器/顶点缓冲都不创建 */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) return value;
    if (!bongo_cat_gl_load(&value->gl, error)) {
        free(value);
        return NULL;
    }
    value->program = bongo_cat_gl_program(&value->gl, vertex_source,
        fragment_source, error);
    if (!value->program) {
        free(value);
        return NULL;
    }
    value->textured_location = value->gl.uniform_location(value->program, "textured");
    value->image_location = value->gl.uniform_location(value->program, "image");
    value->gl.gen_vertex_arrays(1, &value->vao);
    value->gl.bind_vertex_array(value->vao);
    value->gl.gen_buffers(1, &value->vbo);
    value->gl.bind_buffer(GL_ARRAY_BUFFER, value->vbo);
    value->gl.enable_attribute(0);
    value->gl.attribute_pointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, NULL);
    value->gl.enable_attribute(1);
    value->gl.attribute_pointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8,
        (const void *)(sizeof(float) * 2));
    value->gl.enable_attribute(2);
    value->gl.attribute_pointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 8,
        (const void *)(sizeof(float) * 4));
    value->gl.bind_vertex_array(0);
    return value;
}

void bongo_cat_mver_pointer_overlay_destroy(BongoCatMverPointerOverlay *value) {
    if (!value) return;
    clear_textures(value);
    if (value->vbo) value->gl.delete_buffers(1, &value->vbo);
    if (value->vao) value->gl.delete_vertex_arrays(1, &value->vao);
    if (value->program) value->gl.delete_program(value->program);
    free(value);
}
void bongo_cat_mver_pointer_overlay_clear(BongoCatMverPointerOverlay *value) {
    if (value) clear_textures(value);
}

/*
 * 资源路径来自模型包 (用户可自行编辑的元数据), 必须当成不可信输入: 绝对路径、
 * 盘符、以及 ".." 路径段都会被拒绝, 否则一个模型包就能让程序去读包外的任意文件。
 *
 * 判定 ".." 时要求它左右都是路径分隔符 (或字符串首尾), 而不是直接 strstr(".."):
 * 后者会把 "hand..off.png" 这种完全合法的文件名误判成穿越。
 */
static bool safe_relative(const char *path) {
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' || strchr(path, ':'))
        return false;
    const char *part = path;
    while ((part = strstr(part, "..")) != NULL) {
        bool left = part == path || part[-1] == '/' || part[-1] == '\\';
        bool right = !part[2] || part[2] == '/' || part[2] == '\\';
        if (left && right) return false;
        part += 2;
    }
    return true;
}

/*
 * required 的语义: 必需贴图 (手臂/设备) 缺失就让整层加载失败, 可选贴图 (左右键,
 * 侧键) 缺失只是少画那一层 —— 返回 !required 而不是 false, 这样只画了手臂的简易
 * 皮肤仍然可用。尺寸在加载时抄一份进句柄, 绘制侧就不必每帧问后端要。
 */
static bool load_texture(const char *directory, const char *relative,
    bool required, BongoCatPointerTexture *texture, BongoCatError *error) {
    char path[BONGO_CAT_PATH_CAP];
    if (!relative || !relative[0]) return !required;
    if (!safe_relative(relative) ||
        !bongo_cat_path_join(path, sizeof(path), directory, relative) ||
        !bongo_cat_path_is_file(path)) {
        if (required) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Missing Mver pointer asset: %s", relative);
        return !required;
    }
    if (!bongo_cat_texture_from_file(&texture->texture, path, error)) return false;
    texture->width = texture->texture.width;
    texture->height = texture->texture.height;
    return true;
}
/* 元数据是用户可编辑的: 字段缺失/类型不对时一律回落到默认值, 而不是让 NaN 或
   越界值进入几何计算 —— 几何一旦变成 NaN, 整个覆盖层会静默消失, 极难排查。 */
static double number_or(yyjson_val *value, double fallback) {
    return yyjson_is_num(value) ? yyjson_get_num(value) : fallback;
}

/* 颜色在元数据里是 0..255 的整数, 这里钳制后归一化成 0..1 (着色器要的比例) */
static float color(yyjson_val *pointer, const char *name) {
    double value = number_or(yyjson_obj_get(pointer, name), 0.0);
    if (value < 0.0) value = 0.0;
    if (value > 255.0) value = 255.0;
    return (float)(value / 255.0);
}

/*
 * 从模型目录的元数据里装载 Mver 指针层。时序与契约:
 *  - 先 clear_textures: 支持对同一个句柄换模型反复 load, 旧贴图必须先释放,
 *    否则每换一次模型就漏五张纹理;
 *  - 元数据读不出来 / 没有 standardPointer / enabled 不为 true: 都算"这个模型不用
 *    指针层", 直接成功返回 (enabled 保持 false), 让调用方继续跑正常渲染路径;
 *  - 真的开始装载后, 任何一项不满足 (参考尺寸非法, scale 非有限, 必需贴图缺失)
 *    都判定整体失败: 清空并返回 false, 绝不留下"半套资源 + enabled=true"。
 */
bool bongo_cat_mver_pointer_overlay_load(BongoCatMverPointerOverlay *value,
    const char *directory, BongoCatError *error) {
    if (!value || !directory) return false;
    clear_textures(value);
    char metadata[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_model_adapter_metadata_path(directory, metadata,
        sizeof(metadata))) return false;
    yyjson_doc *document = bongo_cat_json_read_file(metadata, 0, NULL);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *pointer = yyjson_obj_get(root, "standardPointer");
    yyjson_val *render = yyjson_obj_get(root, "render");
    bool requested = yyjson_is_obj(pointer) &&
        yyjson_is_true(yyjson_obj_get(pointer, "enabled"));
    if (!requested) {
        yyjson_doc_free(document);
        return true;
    }
    value->reference_width = (int)number_or(
        yyjson_obj_get(render, "referenceWidth"), 1400.0);
    value->reference_height = (int)number_or(
        yyjson_obj_get(render, "referenceHeight"), 1400.0);
    value->mouse = yyjson_is_true(yyjson_obj_get(pointer, "mouse"));
    value->left_handed = yyjson_is_true(yyjson_obj_get(pointer, "leftHanded"));
    value->geometry.offset_x = (float)number_or(yyjson_obj_get(pointer, "offsetX"), 0.0);
    value->geometry.offset_y = (float)number_or(yyjson_obj_get(pointer, "offsetY"), 0.0);
    value->geometry.hand_offset_x = (float)number_or(
        yyjson_obj_get(pointer, "handOffsetX"), 0.0);
    value->geometry.hand_offset_y = (float)number_or(
        yyjson_obj_get(pointer, "handOffsetY"), 0.0);
    value->scale = (float)number_or(yyjson_obj_get(pointer, "scale"), 1.0);
    value->line_red = color(pointer, "lineRed");
    value->line_green = color(pointer, "lineGreen");
    value->line_blue = color(pointer, "lineBlue");
    /* 先摆到画面正中: 装载完成到第一次 set() 之间可能已经画过一帧, 用 0,0 会让
       指针从左上角窜一下 */
    value->x_ratio = value->y_ratio = 0.5f;
    const char *arm = yyjson_get_str(yyjson_obj_get(pointer, "arm"));
    const char *device = yyjson_get_str(yyjson_obj_get(pointer, "device"));
    const char *left = yyjson_get_str(yyjson_obj_get(pointer, "left"));
    const char *right = yyjson_get_str(yyjson_obj_get(pointer, "right"));
    const char *side = yyjson_get_str(yyjson_obj_get(pointer, "side"));
    /* 短路与全有或全无: 必需贴图在后两个 load_texture 里, 前面的条件不成立时它们
       根本不会执行, 所以不会出现"贴图加载了一半"的情况; 无论从哪一步失败, 下面
       统一的 clear_textures 都把已加载的部分收干净。 */
    bool valid = value->reference_width > 0 && value->reference_height > 0 &&
        isfinite(value->scale) && load_texture(directory, arm, true,
            &value->arm, error) && load_texture(directory, device, true,
            &value->device, error) && load_texture(directory, left, false,
            &value->left, error) && load_texture(directory, right, false,
            &value->right, error) && load_texture(directory, side, false,
            &value->side, error);
    value->enabled = valid;
    yyjson_doc_free(document);
    if (!valid) clear_textures(value);
    return valid;
}

bool bongo_cat_mver_pointer_overlay_enabled(
    const BongoCatMverPointerOverlay *value) {
    return value && value->enabled;
}

bool bongo_cat_mver_pointer_overlay_left_handed(
    const BongoCatMverPointerOverlay *value) {
    return value && value->enabled && value->left_handed;
}

/* 只写状态, 不做任何绘制, 因此可以在帧外调用 (输入层就是这么用的)。
   未启用时也照写: enabled 只由 load 决定, 不能让输入路径把它打开或关掉。 */
void bongo_cat_mver_pointer_overlay_set(BongoCatMverPointerOverlay *value,
    float x_ratio, float y_ratio, bool left, bool right, bool side) {
    if (!value) return;
    value->x_ratio = x_ratio;
    value->y_ratio = y_ratio;
    value->left_down = left;
    value->right_down = right;
    value->side_down = side;
}
