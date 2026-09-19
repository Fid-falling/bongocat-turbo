#version 450

/*
 * 设置界面的片元着色器。
 *
 * 字体图集与图标缓存是单通道 (R8) 纹理, OpenGL 那边用 TEXTURE_SWIZZLE 把 RGBA
 * 取成 (1,1,1,r)。这里直接等价地写出来, 于是混合状态可以用标准的
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA (与 GL 的 blend_func_separate 一致)。
 */

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D atlas;

void main() {
    float coverage = texture(atlas, texcoord).r;
    color = vertex_color * vec4(1.0, 1.0, 1.0, coverage);
}
