#version 450

layout(location = 0) out vec4 color;

layout(push_constant, std430) uniform Push {
    int width;
    int height;
    int radius_milli;
} params;

void main() {
    vec2 half_size = vec2(params.width, params.height) * 0.5;
    float radius = float(params.radius_milli) * 0.001;
    vec2 q = abs(gl_FragCoord.xy - half_size) - (half_size - radius);
    float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float coverage = clamp(0.5 - distance, 0.0, 1.0);
    color = vec4(coverage);
}
