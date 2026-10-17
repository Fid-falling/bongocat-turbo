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
    /* 1 = 贴图是直通 alpha (要在这里预乘); 0 = 贴图本身已经是预乘值。
       这个声明必须与调用点 (vk_draw.c) 的口径一致, 理由见文件末尾。 */
    int premultiply;
    /*
     * 1 = 多键显示里非首个按键的"第二通道" (只对**没有**自带气泡素材的模型有用: 一张整画布
     * 图 = 手 + 一块彩色按键高亮)。判据是"够亮 + 够彩": 先丢掉整体偏暗的像素 (max < 0.5),
     * 再比饱和度 (max-min)。只比饱和度不行 —— 那是纯相对判据, 近黑像素 (1,0,0) 的
     * max-min 与 max 都是 1/255, 比值恒为 1 会被判成"高饱和", 手部描边整片保留、淹没真正
     * 的键帽。这段与 OpenGL 侧 overlay.c 的片元着色器是同一套规则, 改一边必须改另一边。
     *
     * 自带气泡素材的模型 (每键一张"气泡 + 手"整图) 不走这条通道: 它的多键显示在 CPU 侧
     * 合成 (合并成一个装着 "A + S" 的气泡, 或每个键各占一个气泡), 见
     * src/render/overlay_bubble.c。这里只管把合成好的整张图当普通贴图画出去。
     */
    int keycap_only;
} params;

void main() {
    color = texture(image, texcoord);
    if (color.a <= 0.001) discard;
    if (params.keycap_only != 0) {
        float mx = max(color.r, max(color.g, color.b));
        float mn = min(color.r, min(color.g, color.b));
        if (mx < 0.5 || mx - mn <= 0.45 * mx) discard;
    }
    vec2 dl = (texcoord - vec2(0.700, 0.515)) / vec2(0.080, 0.170);
    vec2 dr = (texcoord - vec2(0.275, 0.397)) / vec2(0.070, 0.160);
    bool left = dot(dl, dl) < 1.0;
    bool right = dot(dr, dr) < 1.0;
    if ((params.erase_left != 0 && left) || (params.erase_right != 0 && right))
        color.rgb = vec3(1.0);
    /*
     * 输出预乘 alpha, 与 OpenGL 路径的 GL_ONE / GL_ONE_MINUS_SRC_ALPHA 对应 —— 但
     * 只能乘一次, 所以按贴图来源区分:
     *
     *  - 叠加层贴图 (背景/按键/特效) 在 Vulkan 侧是按直通 alpha 上传的 (契约见
     *    render_texture.h: premultiply 只在 GL 侧生效), 必须在这里乘;
     *  - Live2D 的离屏结果本身已经是预乘值 (贴图在 cubism_vulkan_texture.cpp 里按
     *    预乘上传, SDK 的 Vulkan 着色器也按预乘输出), 再乘一次就变成 color*a^2:
     *    模型半透明边缘比 OpenGL 暗一档, 而 GL 与 VK 的 alpha 覆盖完全一致 ——
     *    就是实测到的 "k≈2.0、差异只落在半透明区" 的那个 bug。
     */
    if (params.premultiply != 0)
        color.rgb *= color.a;
}
