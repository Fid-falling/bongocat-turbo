#version 450

/*
 * 径向菜单的片元着色器。
 *
 * 与 ui.frag 的区别: 这里做的是普通 RGBA 采样 (菜单会用彩色封面/图标纹理),
 * 不是"把 r 当覆盖率"的单通道字集路径。输出先预乘 alpha, 配合管线里的
 * ONE / ONE_MINUS_SRC_ALPHA 混合, 等价于 OpenGL 菜单的
 * blend_func_separate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA)。
 */

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D image;

void main() {
    vec4 tint = vertex_color * texture(image, texcoord);
    color = vec4(tint.rgb * tint.a, tint.a);
}
