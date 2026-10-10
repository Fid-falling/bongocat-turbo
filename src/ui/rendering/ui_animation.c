/*
 * 补间动画的状态表。设计上有三个约束:
 *  1. 调用方是绘制代码, 不能持有句柄 —— 所以状态按 (context, id 字符串) 查表,
 *     id 必须字面量稳定 (ui_catime_tabs.c 用 snprintf 拼 "sidebar-active-N" 就是
 *     这个原因), 拼写一变就等于新动画, 会看到突兀的跳变。
 *  2. 表是固定容量的, 满了就按 last_ns 淘汰最久没被碰过的一条。这是有意的: 界面
 *     上的动画数量随布局变化, 让绘制代码负责注销反而更容易漏。
 *  3. 必须每帧调用才会推进。调用方 (preferences_present.c) 反过来用
 *     animations_active() 决定要不要继续出帧, 所以"是否还在动"的判断必须保守:
 *     宁可多画一帧, 也别提前说停 —— 那会让动画停在半路。
 */
#include "ui_animation.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct AnimationSlot {
    struct nk_context *context;
    char id[128];
    float current;
    float start;
    float target;
    float duration_ms;
    uint64_t start_ns;
    uint64_t last_ns;
    uint64_t hold_until_ns;
    BongoCatUIEasing easing;
    bool used;
} AnimationSlot;

static AnimationSlot slots[160];

/*
 * 查/建槽位。新建时把 current/start/target 全部对齐到当前 target —— 首次出现
 * 不能有动画, 否则窗口一打开所有控件会从 0 滑进来。target 只在这里用于初始化,
 * 之后由 animate_eased() 负责改。返回 NULL 表示连淘汰都腾不出位置 (理论上只有
 * 表全被别的 context 占满时才会), 调用方退化成"直接返回 target"。
 */
static AnimationSlot *slot_for(struct nk_context *context, const char *id,
    float target) {
    AnimationSlot *empty = NULL, *oldest = NULL;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        AnimationSlot *slot = &slots[i];
        if (slot->used && slot->context == context && !strcmp(slot->id, id))
            return slot;
        if (!slot->used && !empty) empty = slot;
        if (slot->used && (!oldest || slot->last_ns < oldest->last_ns))
            oldest = slot;
    }
    if (!empty) empty = oldest;
    if (!empty) return NULL;
    memset(empty, 0, sizeof(*empty));
    empty->used = true; empty->context = context;
    empty->current = empty->start = empty->target = target;
    empty->start_ns = empty->last_ns = SDL_GetTicksNS();
    snprintf(empty->id, sizeof(empty->id), "%s", id);
    return empty;
}

/* 三次贝塞尔求值: 给定控制点的某一维坐标与参数 t, 返回该维的值 (也就是 CSS
   cubic-bezier 的那套公式)。ease() 里只用来算 y 维。 */
static float cubic(float first, float second, float value) {
    float inverse = 1.0f - value;
    return 3.0f * inverse * inverse * value * first +
        3.0f * inverse * value * value * second + value * value * value;
}

static float cubic_derivative(float first, float second, float value) {
    float inverse = 1.0f - value;
    return 3.0f * inverse * inverse * first +
        6.0f * inverse * value * (second - first) +
        3.0f * value * value * (1.0f - second);
}

/*
 * 牛顿迭代反解 x(t)=progress, 再取 y(t)。迭代 6 次足够 (误差 <1e-4), 比查表省事
 * 也不用维护表; 导数接近 0 时提前退出并接受当前参数, 否则会除出巨大步长再被
 * clamp 拉回来, 结果在端点附近抖动。每次迭代后都 clamp 到 [0,1], 保证单调。
 */
static float bezier(float progress, float x1, float y1, float x2, float y2) {
    float parameter = progress;
    for (int i = 0; i < 6; ++i) {
        float derivative = cubic_derivative(x1, x2, parameter);
        if (fabsf(derivative) < .0001f) break;
        parameter -= (cubic(x1, x2, parameter) - progress) / derivative;
        parameter = NK_CLAMP(0.0f, parameter, 1.0f);
    }
    return cubic(y1, y2, parameter);
}

float bongo_cat_ui_ease(BongoCatUIEasing easing, float progress) {
    progress = NK_CLAMP(0.0f, progress, 1.0f);
    switch (easing) {
    case BONGO_CAT_UI_EASE_LINEAR: return progress;
    case BONGO_CAT_UI_EASE_STANDARD:
        return bezier(progress, .25f, .1f, .25f, 1.0f);
    case BONGO_CAT_UI_EASE_SWIFT:
        return bezier(progress, .16f, 1.0f, .3f, 1.0f);
    case BONGO_CAT_UI_EASE_SPRING:
        return bezier(progress, .34f, 1.56f, .64f, 1.0f);
    default: {
        float remaining = 1.0f - progress;
        return 1.0f - remaining * remaining * remaining;
    }
    }
}

/*
 * 通用补间。关键行为: 目标变化时从"当前值"重新起跑 (retarget), 而不是跳到起点 ——
 * 连续悬停/取消悬停时靠这一点才不会闪。已到位就直接返回, 避免每帧做无意义的
 * 浮点累加 (也避免 current 永远差一丁点而让 animations_active() 一直报"还在动")。
 */
float bongo_cat_ui_animate_eased(struct nk_context *context,
    const char *id, float target, float duration_ms, BongoCatUIEasing easing) {
    AnimationSlot *slot = slot_for(context, id, target);
    if (!slot) return target;
    uint64_t now = SDL_GetTicksNS();
    if (slot->target != target) {
        slot->start = slot->current;
        slot->target = target;
        slot->duration_ms = duration_ms;
        slot->start_ns = now;
    }
    slot->easing = easing;
    slot->last_ns = now;
    if (slot->current == target) return target;
    float elapsed_ms = (float)(now - slot->start_ns) / 1000000.0f;
    float progress = slot->duration_ms > 0 ? elapsed_ms / slot->duration_ms : 1.0f;
    progress = NK_CLAMP(0.0f, progress, 1.0f);
    float eased = bongo_cat_ui_ease(easing, progress);
    slot->current = slot->start + (slot->target - slot->start) * eased;
    if (progress >= 1.0f || fabsf(slot->current - target) < .001f)
        slot->current = target;
    return slot->current;
}

float bongo_cat_ui_animate(struct nk_context *context, const char *id,
    float target, float duration_ms) {
    return bongo_cat_ui_animate_eased(context, id, target, duration_ms,
        BONGO_CAT_UI_EASE_OUT_CUBIC);
}

/*
 * 一次性脉冲 (点击反馈之类): triggered 时立刻置 1 并保持 hold_ms, 之后用 fade_ms
 * 淡回 0。触发那一帧直接返回 1.0f, 保证再短的点击也能被看见。之后即使调用方不再
 * 传 triggered, 也要继续每帧调用来推进淡出。
 */
float bongo_cat_ui_animate_pulse(struct nk_context *context,
    const char *id, bool triggered, float hold_ms, float fade_ms) {
    AnimationSlot *slot = slot_for(context, id, 0.0f);
    if (!slot) return triggered ? 1.0f : 0.0f;
    uint64_t now = SDL_GetTicksNS();
    slot->last_ns = now;
    slot->easing = BONGO_CAT_UI_EASE_STANDARD;
    if (triggered) {
        slot->current = slot->start = slot->target = 1.0f;
        slot->duration_ms = fade_ms;
        slot->start_ns = now;
        slot->hold_until_ns = now + (uint64_t)(hold_ms * 1000000.0f);
        return 1.0f;
    }
    if (now < slot->hold_until_ns) return 1.0f;
    if (slot->target != 0.0f) {
        slot->start = slot->current;
        slot->target = 0.0f;
        slot->duration_ms = fade_ms;
        slot->start_ns = now;
    }
    if (slot->current == 0.0f) return 0.0f;
    float elapsed_ms = (float)(now - slot->start_ns) / 1000000.0f;
    float progress = slot->duration_ms > 0.0f ?
        elapsed_ms / slot->duration_ms : 1.0f;
    progress = NK_CLAMP(0.0f, progress, 1.0f);
    slot->current = slot->start * (1.0f - bongo_cat_ui_ease(
        slot->easing, progress));
    if (progress >= 1.0f || slot->current < .001f) slot->current = 0.0f;
    return slot->current;
}

/*
 * 是否还需要继续出帧。除了"当前值 != 目标值", 还要处理一种情况: 控件中途消失、
 * 再也没人来推它, 槽位就永远停在中间值 —— 那会让界面永远认为自己在动, 一路空转
 * 重绘。所以超过 duration + 50ms 没人碰过的槽位直接判定到位并跳过 (不删槽位,
 * 下次出现时仍然从 target 开始)。
 * 注意这里不修改 used/last_ns, 只是读。
 */
bool bongo_cat_ui_animations_active(const struct nk_context *context) {
    uint64_t now = SDL_GetTicksNS();
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        AnimationSlot *slot = &slots[i];
        if (!slot->used || slot->context != context) continue;
        if (now < slot->hold_until_ns) return true;
        uint64_t stale_ns = (uint64_t)(slot->duration_ms + 50.0f) * 1000000ULL;
        if (now - slot->last_ns > stale_ns) {
            slot->current = slot->target;
            continue;
        }
        if (fabsf(slot->current - slot->target) >= .001f) return true;
    }
    return false;
}

void bongo_cat_ui_animations_reset(const struct nk_context *context) {
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i)
        if (slots[i].used && slots[i].context == context)
            memset(&slots[i], 0, sizeof(slots[i]));
}
