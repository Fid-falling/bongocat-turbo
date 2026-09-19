#version 450

/*
 * 径向菜单的顶点着色器。
 *
 * 顶点坐标是菜单窗口的逻辑像素 (原点左上), 用 push constant 映射到裁剪空间:
 *   scale = (2/width, 2/height), offset = (-1, -1)
 * 与设置界面 (ui.vert) 的映射一致 —— 它的 +y 朝下, 正好对应菜单的像素坐标。
 *
 * 顶点颜色是直通 (非预乘) 的 RGBA8, 片元着色器里再预乘, 与 OpenGL 菜单用的
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA 混合等价。
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
