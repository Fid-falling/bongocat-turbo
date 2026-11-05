#version 450

/*
 * 叠加层四边形。着色器语义与 OpenGL 后端 src/render/overlay.c 内嵌的 GLSL 保持
 * 一致, 只做两处后端差异处理:
 *  - Vulkan 的裁剪空间 Y 向下, 顶点数据仍按 OpenGL 约定 (+Y 向上), 所以末尾
 *    对 y 取反, 这样镜像/参考尺寸/UV 的算法可以逐行照搬。
 *  - textureSize() 在 Vulkan 里不可用, 纹理尺寸由 push constant 传入。
 */

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec2 texcoord;

layout(push_constant, std430) uniform Push {
    mat4 transform;   /* 叠加层整体的旋转+缩放 (与模型同一个角度, 见 overlay_transform.h) */
    vec2 reference;   /* reference_width, reference_height */
    vec2 image_size;  /* 纹理像素尺寸 */
    int mirror;
    int erase_left;
    int erase_right;
    int premultiply;
    int keycap_only;
    int padding[3];
} params;

void main() {
    vec2 p = position;
    if (params.reference.x > 0.0 && params.reference.y > 0.0) {
        vec2 extent = params.image_size / params.reference;
        p = (p + vec2(1.0, -1.0)) * extent + vec2(-1.0, 1.0);
    }
    if (params.mirror != 0) p.x = -p.x;
    /* 先在 OpenGL 约定的裁剪空间里乘宿主的旋转矩阵 (矩阵就是按 GL 空间推的, 见
       overlay_transform.h), 最后一步才翻到 Vulkan 的 Y 向下。顺序不能反: 先翻后乘再翻
       等于把整层上下镜像了一遍 —— 0 度时就会看到气泡倒过来 (真实踩过)。 */
    vec4 q = params.transform * vec4(p, 0.0, 1.0);
    gl_Position = vec4(q.x, -q.y, 0.0, 1.0);
    texcoord = uv;
}
