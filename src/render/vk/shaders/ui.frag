#version 450

/*
 * 设置界面的片元着色器。
 *
 * 两类纹理的取样方式不同 (与 OpenGL 路径逐条对齐):
 *  - 单通道遮罩 (字体图集、paint cache): GL 用 TEXTURE_SWIZZLE 取成 (1,1,1,r),
 *    这里等价地只取 R 当覆盖率, RGB 当 1;
 *  - 普通 RGBA 图 (ui-symbols.png 图标图集、logo): 整体采样, 形状在 alpha 里。
 * 以前一律按前者处理, 于是"白色 RGB + alpha 遮罩"的图标图集 coverage 恒为 1,
 * 整个四边形被涂成纯色方块。
 */

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(push_constant, std430) uniform Push {
    vec2 scale;
    vec2 offset;
    int color_texture;
} projection;

void main() {
    vec4 sample_color = texture(atlas, texcoord);
    float coverage = projection.color_texture != 0 ? sample_color.a : sample_color.r;
    vec3 tint = projection.color_texture != 0 ? sample_color.rgb : vec3(1.0);
    color = vertex_color * vec4(tint, coverage);
}
