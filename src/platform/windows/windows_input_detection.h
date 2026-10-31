#ifndef BONGO_CAT_WINDOWS_INPUT_DETECTION_H
#define BONGO_CAT_WINDOWS_INPUT_DETECTION_H

#ifdef _WIN32
#include <stdbool.h>
#include <windows.h>

/*
 * "鼠标是不是被别的程序锁住 / 独占"的判定状态机(纯逻辑, 不碰 Win32 API, 便于单独
 * 测试)。
 *
 * 输入是每次探测采到的观察(WindowsPointerObservation), 输出是"现在该不该改用相对
 * 位移模式"。证据分三层:
 *   - 直接证据: 光标被裁剪进一个极小的矩形(游戏锁定鼠标的典型做法);
 *   - 间接证据: 光标被隐藏, 或者屏幕上看不到移动但设备持续报告位移(被 recenter
 *     回屏幕中心);
 *   - 反证: 光标真的移动到显示器/裁剪区边界之外, 或者有真实的往返行程。
 * reason 会写进日志, 排查"游戏模式为什么没生效"时先看它。
 */
typedef enum WindowsPointerReason {
    WINDOWS_POINTER_DESKTOP,
    WINDOWS_POINTER_HIDDEN,
    WINDOWS_POINTER_CLIPPED,
    WINDOWS_POINTER_STALLED,
    WINDOWS_POINTER_RECENTERED
} WindowsPointerReason;

typedef struct WindowsPointerObservation {
    HWND foreground;
    DWORD pid;
    bool foreign, position_known, monitor_known, clip_known, cursor_known;
    POINT position;
    RECT monitor, clip;
    DWORD cursor_flags;
    double raw_x, raw_y;
    double absolute_x, absolute_y;
    unsigned long long motion_packets;
} WindowsPointerObservation;

/* Owned by the model thread; device statistics are transferred under its lock. */
typedef struct WindowsPointerDetection {
    HWND foreground;
    DWORD pid;
    bool initialized, relative, hint_pending, away, edge_seen;
    ULONGLONG last_ms, window_ms, hint_ms, evidence_ms;
    POINT anchor;
    double raw_x, raw_y;
    double absolute_x, absolute_y;
    unsigned moving_samples, returns;
    /* 连续"光标隐藏 + 位置钉死"的窗口数。游戏 (Minecraft 这类 GLFW 游戏就是) 用
       SetCursorPos 每帧把光标拉回窗口中心而不是 ClipCursor, 此时设备位移报文可能因为
       别的进程独占鼠标而收不到 —— 但只要"隐藏且钉死"持续两窗, 就足以判定被锁。 */
    unsigned pinned_windows;
    /* 本窗口里"回到起点附近 (≤2px)"的采样次数。游戏逐帧 recenter 时它一直很大; 桌面上
       真的在移动鼠标 (哪怕光标是隐藏的) 时它是 0 —— 解锁判据靠它区分这两种情况。 */
    unsigned anchor_hits;
    /* 上一个窗口结束时的证据快照 (begin_window 会清零累积量, 日志要的是清零前的值) */
    long long diag_excursion;
    long long diag_distance;
    unsigned diag_returns;
    unsigned diag_anchor_hits;
    int diag_hint;
    bool diag_unlocked;
    long long excursion;
    WindowsPointerReason reason;
} WindowsPointerDetection;

/* 判定"这个裁剪矩形是不是小到等于锁定"(1x1 / 2x2 的裁剪是游戏惯用手法)。单独导出
   是为了能脱离探测流程直接测试这条判据。 */
bool bongo_cat_windows_pointer_clip_locked(const RECT *clip);
/* 判定原因的可读名字 (日志用)。 */
const char *bongo_cat_windows_pointer_reason_name(WindowsPointerReason reason);
/* 累积一次观察并给出结论。state 由调用方(模型线程)独占, 不需要加锁。
   返回 true 表示建议使用相对位移; 前台窗口变了或间隔太久会重置状态机并返回 false。 */
bool bongo_cat_windows_pointer_detect(WindowsPointerDetection *state,
    const WindowsPointerObservation *sample, ULONGLONG now_ms);
#endif
#endif
