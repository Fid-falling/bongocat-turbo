#version 450

/*
 * 径向菜单的片元着色器。
 *
 * 菜单里混着两类纹理, 必须分开处理 (与 ui.frag 同一套约定):
 *  - 单通道字形图集 (dial 的字体是 R8): GL 那边用 TEXTURE_SWIZZLE 取成
 *    (1,1,1,r), 这里等价地只取 r 当覆盖率、RGB 置 1 —— 漏掉这一步时
 *    R8 会被当成 RGBA 采样成 (r,0,0,1), 文字就会显示成黑块;
 *  - 普通 RGBA 纹理 (菜单图标/封面): 整体采样。
 * 输出先预乘 alpha, 配合管线里的 ONE / ONE_MINUS_SRC_ALPHA 混合, 等价于
 * OpenGL 菜单的 blend_func_separate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE,
 * ONE_MINUS_SRC_ALPHA)。
 */

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D image;

layout(push_constant, std430) uniform Push {
    vec2 scale;
    vec2 offset;
    /* 1 = 普通 RGBA 纹理; 0 = 单通道遮罩 (取 r 当覆盖率) */
    int color_texture;
} projection;

void main() {
    vec4 sample_color = texture(image, texcoord);
    vec4 tint = projection.color_texture != 0
        ? vertex_color * sample_color
        : vertex_color * vec4(1.0, 1.0, 1.0, sample_color.r);
    color = vec4(tint.rgb * tint.a, tint.a);
}
