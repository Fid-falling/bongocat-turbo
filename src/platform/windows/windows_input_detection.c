#include "windows_input_detection.h"

#ifdef _WIN32
#include <math.h>
#include <stdlib.h>

/* 判"在内侧"而不是"在范围内": 留 4 像素边距。贴边的光标通常意味着游戏把鼠标困在
   中心之后又撞到了边界, 那不属于正常桌面移动。 */
static bool interior(POINT point, const RECT *bounds) {
    const int margin = 4;
    return (long long)point.x > (long long)bounds->left + margin &&
        (long long)point.x < (long long)bounds->right - 1 - margin &&
        (long long)point.y > (long long)bounds->top + margin &&
        (long long)point.y < (long long)bounds->bottom - 1 - margin;
}

/* 日志用: 判定原因的可读名字 (排查"为什么游戏模式没生效"时就靠它)。 */
const char *bongo_cat_windows_pointer_reason_name(WindowsPointerReason reason) {
    switch (reason) {
    case WINDOWS_POINTER_CLIPPED: return "clipped";
    case WINDOWS_POINTER_HIDDEN: return "hidden";
    case WINDOWS_POINTER_STALLED: return "stalled";
    case WINDOWS_POINTER_RECENTERED: return "recentered";
    case WINDOWS_POINTER_DESKTOP: default: return "desktop";
    }
}

bool bongo_cat_windows_pointer_clip_locked(const RECT *clip) {
    if (!clip || clip->right < clip->left || clip->bottom < clip->top)
        return false;
    /* Ordered LONG endpoints have an exact DWORD distance, even across zero.
       Keep the subtraction unsigned instead of widening signed coordinates. */
    DWORD width = (DWORD)clip->right - (DWORD)clip->left;
    DWORD height = (DWORD)clip->bottom - (DWORD)clip->top;
    return width <= 2 && height <= 2;
}

/* 单次观察能给出的最强提示(还不是结论): 极小裁剪 > 光标隐藏 > 桌面。真正下结论要等
   累积到足够的运动证据, 见下面的 detect。 */
static WindowsPointerReason lock_hint(const WindowsPointerObservation *sample) {
    if (sample->clip_known && bongo_cat_windows_pointer_clip_locked(&sample->clip))
        return WINDOWS_POINTER_CLIPPED;
    if (sample->cursor_known && sample->cursor_flags == 0)
        return WINDOWS_POINTER_HIDDEN;
    return WINDOWS_POINTER_DESKTOP;
}

/* 开启一个新的观察窗口: 重置锚点与全部累加量。窗口长度由 detect 里的 128/256ms
   阈值决定 —— 窗口太短会把正常抖动误判成锁定, 太长则会让第一次按键的延迟很明显。 */
static void begin_window(WindowsPointerDetection *state, POINT point,
    ULONGLONG now_ms) {
    state->anchor = point;
    state->window_ms = now_ms;
    state->raw_x = state->raw_y = 0;
    state->absolute_x = state->absolute_y = 0;
    state->moving_samples = state->returns = 0;
    state->away = false;
    state->edge_seen = false;
    state->excursion = 0;
    state->anchor_hits = 0;
    /* 注意: pinned_windows 故意**不**在这里清零 —— 它统计的是"连续多少个窗口",
       窗口本身就应该是它的计数单位。*/
}

/*
 * 状态机主体。时间阈值(32 / 128 / 192 / 256 ms)与样本数阈值(2 / 4)是照着"人按下并
 * 移动鼠标"的时间尺度定的: 在极小裁剪里持续 32ms 才算直接证据, 128ms 且至少 4 个
 * 运动报文才算有足够证据。
 * 判定为锁定之后不会因为"鼠标停住"而自动解除 —— 停手时设备仍在报位移(被 recenter),
 * 只有真实的光标旅行才能推翻它(见末尾原有的英文注释)。
 * 每个窗口结束时都会 begin_window 重新计数, 因此证据是逐窗累积的。
 */
bool bongo_cat_windows_pointer_detect(WindowsPointerDetection *state,
    const WindowsPointerObservation *sample, ULONGLONG now_ms) {
    if (!state || !sample) return false;
    if (!sample->foreign || !sample->foreground || !sample->position_known) {
        *state = (WindowsPointerDetection){0};
        return false;
    }
    /* Never reuse movement from another foreground window or a suspended loop. */
    if (!state->initialized || state->foreground != sample->foreground ||
        state->pid != sample->pid || now_ms < state->last_ms ||
        now_ms - state->last_ms > 500) {
        *state = (WindowsPointerDetection){.initialized = true,
            .foreground = sample->foreground, .pid = sample->pid,
            .last_ms = now_ms};
        begin_window(state, sample->position, now_ms);
        return false;
    }
    state->last_ms = now_ms;
    state->raw_x += sample->raw_x;
    state->raw_y += sample->raw_y;
    state->absolute_x += sample->absolute_x;
    state->absolute_y += sample->absolute_y;
    if (sample->motion_packets) state->moving_samples++;
    long long dx = llabs((long long)sample->position.x - state->anchor.x);
    long long dy = llabs((long long)sample->position.y - state->anchor.y);
    long long distance = dx > dy ? dx : dy;
    if (distance > state->excursion) state->excursion = distance;
    if (dx <= 2 && dy <= 2) state->anchor_hits++;
    if (distance > 2) state->away = true;
    else if (state->away) { state->returns++; state->away = false; }
    bool inside = sample->monitor_known &&
        interior(sample->position, &sample->monitor) &&
        interior(state->anchor, &sample->monitor) &&
        (!sample->clip_known || (interior(sample->position, &sample->clip) &&
            interior(state->anchor, &sample->clip)));
    if (!inside) state->edge_seen = true;

    WindowsPointerReason hint = lock_hint(sample);
    if (hint == WINDOWS_POINTER_CLIPPED) {
        if (!state->hint_pending) {
            state->hint_pending = true;
            state->hint_ms = now_ms;
        }
        /* Only a tiny clip is direct evidence. Hidden cursors may still travel
           normally, so they use the same motion checks as visible cursors. */
        if (state->relative || (now_ms - state->hint_ms >= 32 &&
            state->moving_samples >= 2)) {
            state->relative = true;
            state->reason = hint;
            state->evidence_ms = now_ms;
        }
    } else state->hint_pending = false;

    ULONGLONG window_age = now_ms - state->window_ms;
    if (window_age >= 128 && (state->moving_samples >= 4 || window_age >= 256)) {
        /* Net device counts reject ordinary back-and-forth movement. The
           monitor and clipping edges reject a cursor pushed against a border. */
        /* Device counts and absolute-device pixels remain separate units. */
        bool raw_travel = fabs(state->raw_x) >= 32 || fabs(state->raw_y) >= 32 ||
            fabs(state->absolute_x) >= 32 || fabs(state->absolute_y) >= 32;
        bool evidence = !state->edge_seen && raw_travel && state->moving_samples >= 4;
        bool stalled = evidence && distance <= 1 && state->excursion <= 1;
        bool recentered = evidence && state->returns >= 2 && state->excursion <= 48;
        /* 光标隐藏 + 位置在整窗内一动不动: 这是 Minecraft 这类"用 SetCursorPos 把光标
           拉回中心"的游戏的特征 (它们不用 ClipCursor, 也就不满足上面的极小区判据)。
           这条证据不需要设备位移报文 —— 有些游戏会独占鼠标, 我们的 RIDEV_INPUTSINK 收
           不到报文, 但"隐藏且钉死"本身就说明用户在游戏里而不是在桌面上移动鼠标。
           两窗 (≈256ms) 才算, 免得读长文/看视频时隐藏光标被误判。 */
        bool pinned = hint == WINDOWS_POINTER_HIDDEN && !state->edge_seen &&
            distance <= 1 && state->excursion <= 1;
        state->pinned_windows = pinned ? state->pinned_windows + 1 : 0;
        if (state->pinned_windows >= 2 && !state->relative) {
            state->relative = true;
            state->reason = WINDOWS_POINTER_HIDDEN;
            state->evidence_ms = now_ms;
        }
        if (hint != WINDOWS_POINTER_CLIPPED && (stalled || recentered)) {
            state->relative = true;
            state->reason = hint == WINDOWS_POINTER_HIDDEN ? WINDOWS_POINTER_HIDDEN :
                stalled ? WINDOWS_POINTER_STALLED : WINDOWS_POINTER_RECENTERED;
            state->evidence_ms = now_ms;
        } else if (hint != WINDOWS_POINTER_CLIPPED && state->relative &&
            now_ms - state->evidence_ms >= 192) {
            /* 解锁条件按"当初为什么锁上"分开:
               - HIDDEN (游戏用 SetCursorPos 把光标钉住): 游戏里光标本来就一直在中心附近
                 抖两三像素, 只要它还藏着就不能因为"位置动了 3 像素"解锁 —— 否则锁定状态
                 每秒翻好几次, 模型位置被反复重置回那个假中心, 表现就是"一抽一抽";
                 只有光标重新出现 (桌面) 并且真的移动了才算离开游戏。
               - 其它 (极小区裁剪 / 停滞 / 回中): 沿用"位置真的动了"这条判据。 */
            bool unlock = false;
            if (state->reason == WINDOWS_POINTER_HIDDEN) {
                /* 隐藏光标时用"整窗最大偏移"而不是"这一帧比窗口起点远 2 像素": 游戏把光标
                   recenter 造成的三两像素抖动不该解锁, 只有真正的光标旅行 (几十像素以上)
                   或者光标重新出现在桌面上, 才算离开游戏。 */
                /* "来回跑"才是 recenter 的指纹: 游戏里每帧都有 (光标被拽走再拉回中心),
                   而桌面上真的在移动鼠标 (哪怕光标是藏起来的) 是单程的。所以只有
                   "光标重新出现" 或 "整窗偏移很大且一次都没回过起点" 才算离开游戏。 */
                unlock = hint == WINDOWS_POINTER_DESKTOP ||
                    (state->excursion > 24 && state->anchor_hits == 0);
            } else {
                unlock = distance > 2 ||
                    state->reason == WINDOWS_POINTER_CLIPPED;
            }
            state->diag_unlocked = unlock;
            if (unlock) {
                state->relative = false;
                state->reason = WINDOWS_POINTER_DESKTOP;
            }
        }
        state->diag_excursion = state->excursion;
        state->diag_distance = distance;
        state->diag_returns = state->returns;
        state->diag_anchor_hits = state->anchor_hits;
        state->diag_hint = (int)hint;
        /* A stationary inferred lock stays active until actual cursor travel
           disproves it; stopping the mouse must not reset the virtual position. */
        begin_window(state, sample->position, now_ms);
    }
    return state->relative;
}
#endif
