#include "windows_input_detection.h"

#ifdef _WIN32
#include <stddef.h>

/* 判定"这个裁剪矩形是不是小到等于锁定": 1x1 / 2x2 是游戏惯用手法。 */
bool bongo_cat_windows_pointer_clip_locked(const RECT *clip) {
    if (!clip || clip->right < clip->left || clip->bottom < clip->top)
        return false;
    /* Ordered LONG endpoints have an exact DWORD distance, even across zero.
       Keep the subtraction unsigned instead of widening signed coordinates. */
    DWORD width = (DWORD)clip->right - (DWORD)clip->left;
    DWORD height = (DWORD)clip->bottom - (DWORD)clip->top;
    return width <= 2 && height <= 2;
}

const char *bongo_cat_windows_pointer_reason_name(WindowsPointerReason reason) {
    switch (reason) {
    case WINDOWS_POINTER_CLIPPED: return "clipped";
    case WINDOWS_POINTER_HIDDEN: return "hidden";
    case WINDOWS_POINTER_DESKTOP: default: return "desktop";
    }
}

/*
 * 只回答一个问题: 现在该不该用相对位移。
 *
 * 历史教训: 这里曾经有一套"证据累积"状态机 (极小区裁剪是直接证据, 位置钉死是间接证据,
 * 真实旅行是反证)。它在桌面上没问题, 但玩家在游戏里甩鼠标时, 游戏会在 recenter 之前让
 * 光标飞出去几百像素, 采样也常常落在漂移位置上 —— 于是锁定被反复推翻又重建, 每推翻一次
 * 模型位置就退回那个被钉住的假中心, 用户看到的就是"一抽一抽"。
 *
 * 现在只看两个**状态**: 光标隐藏, 或者被裁进 2x2。两者都是游戏独占鼠标的直接表现, 与
 * 光标怎么移动无关, 因此甩多快都不会抖。判断依据少, 反而稳定。
 */
bool bongo_cat_windows_pointer_detect(WindowsPointerDetection *state,
    const WindowsPointerObservation *sample) {
    if (!state) return false;
    if (!sample || !sample->foreign || !sample->cursor_known) {
        *state = (WindowsPointerDetection){0};
        return false;
    }
    bool hidden = sample->cursor_flags == 0;
    bool clipped = sample->clip_known &&
        bongo_cat_windows_pointer_clip_locked(&sample->clip);
    bool relative = hidden || clipped;
    state->initialized = true;
    state->foreground = sample->foreground;
    state->pid = sample->pid;
    state->relative = relative;
    state->reason = clipped ? WINDOWS_POINTER_CLIPPED :
        hidden ? WINDOWS_POINTER_HIDDEN : WINDOWS_POINTER_DESKTOP;
    return relative;
}
#endif
