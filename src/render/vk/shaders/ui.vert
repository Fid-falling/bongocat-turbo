#version 450

/*
 * 设置界面 (Nuklear) 的顶点着色器。
 *
 * 顶点坐标是逻辑像素, 由 push constant 里的投影映射到裁剪空间。与 OpenGL 版的
 * 区别只有 y 方向: OpenGL 的投影把 y=0 放到上方 (NDC +1), Vulkan 的裁剪空间
 * y 向下, 所以这里不取负号, 直接用 2y/h - 1。
 */

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;

layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 vertex_color;

layout(push_constant, std430) uniform Push {
    vec2 scale;    /* 2/width, 2/height */
    vec2 offset;   /* -1, -1 */
} projection;

void main() {
    gl_Position = vec4(position * projection.scale + projection.offset, 0.0, 1.0);
    texcoord = uv;
    vertex_color = color;
}
