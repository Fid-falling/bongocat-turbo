#include "windows_input_internal.h"

#ifdef _WIN32
#include <string.h>

/*
 * 鼠标位移的两条通道(接收线程, 由 relative_lock 保护):
 *   - observed_*: 给"游戏模式判定"用的观测累加器, 由 take_observation 取走;
 *   - relative_* / absolute_*: 给模型用的位移累加器, 只在 relative_active 时累加。
 * 两者互不清空对方 —— 判定器读观测不能把模型要用的位移吃掉(反过来也一样)。
 *
 * 相对设备直接累加 lLastX/lLastY; 绝对设备(触摸板、远程桌面)要先按 bounds 把
 * [0,65535] 映射成像素, 再取与上一次的差值 —— 绝对设备本身没有"位移"这个量。
 * 拿不到可信基线时(刚插上、bounds 变了、MOUSE_ATTRIBUTES_CHANGED)只更新基线而
 * 不产生位移, 否则会凭空跳一大段。
 */
void bongo_cat_windows_input_motion(WindowsInputState *state,
    WindowsRawDevice *device, const RAWMOUSE *mouse, const RECT *bounds) {
    AcquireSRWLockExclusive(&state->relative_lock);
    if (!(mouse->usFlags & MOUSE_MOVE_ABSOLUTE)) {
        device->absolute_known = false;
        if (mouse->lLastX || mouse->lLastY) {
            state->observed_x += mouse->lLastX;
            state->observed_y += mouse->lLastY;
            state->observed_motion++;
            if (state->relative_active) {
                state->relative_x += mouse->lLastX;
                state->relative_y += mouse->lLastY;
                state->relative_samples++;
            }
        }
    } else {
        if (!bounds || bounds->right <= bounds->left ||
            bounds->bottom <= bounds->top || mouse->lLastX < 0 ||
            mouse->lLastX > 65535 || mouse->lLastY < 0 || mouse->lLastY > 65535) {
            device->absolute_known = false;
            ReleaseSRWLockExclusive(&state->relative_lock);
            return;
        }
        double x = bounds->left + (double)mouse->lLastX *
            ((double)bounds->right - bounds->left - 1.0) / 65535.0;
        double y = bounds->top + (double)mouse->lLastY *
            ((double)bounds->bottom - bounds->top - 1.0) / 65535.0;
        bool baseline = device->absolute_known &&
            !(mouse->usFlags & MOUSE_ATTRIBUTES_CHANGED) &&
            device->generation == state->generation &&
            memcmp(bounds, &device->absolute_bounds, sizeof(*bounds)) == 0;
        if (baseline && (x != device->absolute_x || y != device->absolute_y)) {
            state->observed_absolute_x += x - device->absolute_x;
            state->observed_absolute_y += y - device->absolute_y;
            state->observed_motion++;
            if (state->relative_active) {
                state->absolute_x += x - device->absolute_x;
                state->absolute_y += y - device->absolute_y;
                state->absolute_samples++;
            }
        }
        device->absolute_x = x;
        device->absolute_y = y;
        device->absolute_bounds = *bounds;
        device->absolute_known = true;
        device->generation = state->generation;
    }
    ReleaseSRWLockExclusive(&state->relative_lock);
}

static void clear_motion(WindowsInputState *state) {
    state->relative_x = state->relative_y = 0;
    state->absolute_x = state->absolute_y = 0.0;
    state->relative_samples = state->absolute_samples = 0;
    state->generation++;
}

/* 清空两组累加器并推进 generation: 主线程看到 generation 变化就知道先前取到的
   观测已经作废(设备表被重建、注册状态变了)。 */
void bongo_cat_windows_input_clear_motion(WindowsInputState *state) {
    AcquireSRWLockExclusive(&state->relative_lock);
    clear_motion(state);
    state->observed_x = state->observed_y = 0;
    state->observed_absolute_x = state->observed_absolute_y = 0;
    state->observed_motion = 0;
    state->observed_generation++;
    ReleaseSRWLockExclusive(&state->relative_lock);
}

/* 取走观测并把观测累加器清零(取走即消费), 返回当前 generation。 */
unsigned long long bongo_cat_windows_input_take_observation(
    WindowsInputState *state, WindowsPointerObservation *sample) {
    if (!state || !sample) return 0;
    AcquireSRWLockExclusive(&state->relative_lock);
    sample->raw_x = (double)state->observed_x;
    sample->raw_y = (double)state->observed_y;
    sample->absolute_x = state->observed_absolute_x;
    sample->absolute_y = state->observed_absolute_y;
    sample->motion_packets = state->observed_motion;
    unsigned long long generation = state->observed_generation;
    state->observed_x = state->observed_y = 0;
    state->observed_absolute_x = state->observed_absolute_y = 0;
    state->observed_motion = 0;
    ReleaseSRWLockExclusive(&state->relative_lock);
    return generation;
}

/* 取走给模型用的位移。可用性判断见下面原有的英文注释(已经投递过来的报文即使与
   注册探测的结论不一致也仍然有效)。相对位移与绝对位移不能相加, 有相对采样时就
   只返回相对值。无论可用与否都会清零累加器 —— 留着只会让位移越积越大。 */
bool bongo_cat_windows_input_take_relative(BongoCatPlatform *platform,
    double *x, double *y, unsigned long long *sample_count) {
    if (!x || !y) return false;
    *x = *y = 0.0;
    if (sample_count) *sample_count = 0;
    WindowsInputState *state = platform ? platform->native : NULL;
    if (!state) return false;
    AcquireSRWLockExclusive(&state->relative_lock);
    /* Delivered packets remain valid even if the registration probe disagrees. */
    bool delivered = state->relative_samples || state->absolute_samples;
    bool available = state->relative_active && (state->receiving || delivered);
    if (available) {
        /* Device counts and absolute-device pixels must never be added together. */
        bool relative = state->relative_samples != 0;
        *x = relative ? (double)state->relative_x : state->absolute_x;
        *y = relative ? (double)state->relative_y : state->absolute_y;
        if (sample_count) *sample_count = relative ? state->relative_samples :
            state->absolute_samples;
    }
    state->relative_x = state->relative_y = 0;
    state->absolute_x = state->absolute_y = 0.0;
    state->relative_samples = state->absolute_samples = 0;
    ReleaseSRWLockExclusive(&state->relative_lock);
    return available;
}

/* 进入"鼠标被独占"的场景时调用: 清掉旧位移并开始累加, 免得把切换瞬间的跳变算
   进去。 */
void bongo_cat_windows_input_reset_relative(BongoCatPlatform *platform) {
    WindowsInputState *state = platform ? platform->native : NULL;
    if (!state) return;
    AcquireSRWLockExclusive(&state->relative_lock);
    clear_motion(state);
    state->relative_active = true;
    ReleaseSRWLockExclusive(&state->relative_lock);
}

/* 离开该场景时调用: 停止累加并清空, 之后 take_relative 一律返回不可用。 */
void bongo_cat_windows_input_release_relative(BongoCatPlatform *platform) {
    WindowsInputState *state = platform ? platform->native : NULL;
    if (!state) return;
    AcquireSRWLockExclusive(&state->relative_lock);
    clear_motion(state);
    state->relative_active = false;
    ReleaseSRWLockExclusive(&state->relative_lock);
}
#endif
