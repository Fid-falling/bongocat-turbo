#version 450

/*
 * 通用 2D 图元 (鼠标指针叠加: 手臂、按键圆点、线段)。与 OpenGL 后端的
 * mver_pointer_overlay_draw.c 一一对应: 顶点自带颜色, textured=0 时只用顶点色。
 * 同样在末尾对 y 取反, 以沿用 OpenGL 约定下算好的顶点坐标。
 */

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;

layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 vertex_color;

layout(push_constant, std430) uniform Push {
    int textured;
} params;

void main() {
    gl_Position = vec4(position.x, -position.y, 0.0, 1.0);
    texcoord = uv;
    vertex_color = color;
}
