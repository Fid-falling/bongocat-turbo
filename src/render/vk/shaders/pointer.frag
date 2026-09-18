#version 450

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D image;

layout(push_constant, std430) uniform Push {
    int textured;
} params;

void main() {
    vec4 tint = vertex_color;
    color = params.textured != 0 ? tint * texture(image, texcoord) : tint;
    /* 顶点色本身就是预乘 alpha, 这里再乘一次 alpha 保持与 OpenGL 的
       GL_ONE / GL_ONE_MINUS_SRC_ALPHA 一致。 */
    color.rgb *= color.a;
}
