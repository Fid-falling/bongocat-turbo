#version 450

/*
 * 圆角遮罩: 一个覆盖全屏的三角形, 片元着色器算出圆角矩形的覆盖率。
 * 与 OpenGL 后端 window_background.c 里那个用 gl_VertexID 拼出来的全屏三角形
 * 等价; 遮罩关于中心对称, 所以 Vulkan 与 OpenGL 的 gl_FragCoord.y 方向差异
 * 不影响结果。
 */

void main() {
    /* Vulkan 里的内建变量叫 gl_VertexIndex (OpenGL 那边是 gl_VertexID) */
    vec2 p = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0,
        gl_VertexIndex == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
