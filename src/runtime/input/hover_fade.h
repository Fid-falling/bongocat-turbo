/*
 * 悬停隐藏的淡入淡出数学: 一条 C1 连续的 smoothstep 曲线 (phase -> 不透明度系数) 与两
 * 个反解/推进入口。相位没有物理单位, 只表示"过渡进行到几分之几"; 实际不透明度 =
 * hover_fade_opacity * curve(phase), 所以改曲线形状不会破坏"终值等于用户设定值"。
 * 反解 (phase) 与推进 (step) 必须使用同一条曲线: 两处不一致会让用户改设置或鼠标折返
 * 时画面跳变。两者都是纯函数, 不读应用状态, 因此测试与冒烟脚本可以直接驱动它们。
 * 20 次二分足够把相位收敛到 1e-6 以内, 远超一帧能看出的差别。
 */
#ifndef BONGO_CAT_HOVER_FADE_H
#define BONGO_CAT_HOVER_FADE_H

#include <stdbool.h>

/* 起始与结束处斜率为 0, 所以淡入淡出不会出现"突然起步/硬停"的折角。 */
static inline float bongo_cat_hover_fade_curve(float phase) {
    return phase * phase * (3.0f - 2.0f * phase);
}

/* Recover the curve position from the last successfully displayed opacity. */
/* 用上次真正显示出来的不透明度反解相位: 用户中途改设置或鼠标折返时要接着当前位置继续,
   而不是从 0 或 1 重新开始。输入超出 [0,1] 时钳到端点。 */
static inline float bongo_cat_hover_fade_phase(float opacity) {
    if (opacity <= 0.0f) return 0.0f;
    if (opacity >= 1.0f) return 1.0f;
    float low = 0.0f, high = 1.0f;
    for (int i = 0; i < 20; ++i) {
        float mid = (low + high) * 0.5f;
        if (bongo_cat_hover_fade_curve(mid) < opacity) low = mid;
        else high = mid;
    }
    return (low + high) * 0.5f;
}

/* 按 elapsed 推进相位并钳在 [0,1]; duration <= 0 表示"不要动画", 直接返回终值。
   elapsed <= 0 时原样返回: 倒退的时间戳不能把相位拉回去。 */

static inline float bongo_cat_hover_fade_step(float phase, bool hidden,
    float elapsed, float duration) {
    if (duration <= 0.0f) return hidden ? 0.0f : 1.0f;
    if (elapsed <= 0.0f) return phase;
    phase += (hidden ? -elapsed : elapsed) / duration;
    return phase < 0.0f ? 0.0f : phase > 1.0f ? 1.0f : phase;
}

#endif
