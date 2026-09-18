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
