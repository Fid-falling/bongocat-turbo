#version 450

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D image;

layout(push_constant, std430) uniform Push {
    vec2 reference;
    vec2 image_size;
    int mirror;
    int erase_left;
    int erase_right;
} params;

void main() {
    color = texture(image, texcoord);
    if (color.a <= 0.001) discard;
    vec2 dl = (texcoord - vec2(0.700, 0.515)) / vec2(0.080, 0.170);
    vec2 dr = (texcoord - vec2(0.275, 0.397)) / vec2(0.070, 0.160);
    bool left = dot(dl, dl) < 1.0;
    bool right = dot(dr, dr) < 1.0;
    if ((params.erase_left != 0 && left) || (params.erase_right != 0 && right))
        color.rgb = vec3(1.0);
    /* 输出预乘 alpha, 与 OpenGL 路径的 GL_ONE / GL_ONE_MINUS_SRC_ALPHA 对应 */
    color.rgb *= color.a;
}
