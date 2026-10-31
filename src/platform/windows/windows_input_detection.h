#ifndef BONGO_CAT_WINDOWS_INPUT_DETECTION_H
#define BONGO_CAT_WINDOWS_INPUT_DETECTION_H

#ifdef _WIN32
#include <stdbool.h>
#include <windows.h>

/*
 * "现在该不该把鼠标位置当成相对位移来累加"的判定(纯逻辑, 不碰 Win32 API, 便于单独测试)。
 *
 * 规则只有两条, 都是**状态**而不是推测:
 *   1. 光标被隐藏 (GetCursorInfo 的 flags 里没有 CURSOR_SHOWING) —— 游戏独占鼠标时最常见的
 *      做法 (SetCursor(NULL) / ShowCursor(FALSE));
 *   2. 光标被裁进一个 2x2 以内的矩形 (ClipCursor 的老式做法, 个别游戏不藏光标只裁剪)。
 * 两条都不成立就回到绝对坐标。**不做**运动累积、不看到底回到了哪里、不算证据窗口 ——
 * 那些判据在"玩家甩鼠标"时会把锁定反复推翻, 表现就是模型位置来回跳。
 *
 * reason 只用于日志。
 */
typedef enum WindowsPointerReason {
    WINDOWS_POINTER_DESKTOP,
    WINDOWS_POINTER_HIDDEN,
    WINDOWS_POINTER_CLIPPED
} WindowsPointerReason;

typedef struct WindowsPointerObservation {
    HWND foreground;
    DWORD pid;
    bool foreign, position_known, monitor_known, clip_known, cursor_known;
    POINT position;
    RECT monitor, clip;
    DWORD cursor_flags;
} WindowsPointerObservation;

/* 由模型线程独占, 不需要加锁。 */
typedef struct WindowsPointerDetection {
    HWND foreground;
    DWORD pid;
    bool initialized, relative;
    WindowsPointerReason reason;
} WindowsPointerDetection;

/* 判定"这个裁剪矩形是不是小到等于锁定"(1x1 / 2x2 的裁剪是游戏惯用手法)。单独导出
   是为了能脱离探测流程直接测试这条判据。 */
bool bongo_cat_windows_pointer_clip_locked(const RECT *clip);
/* 判定原因的可读名字 (日志用)。 */
const char *bongo_cat_windows_pointer_reason_name(WindowsPointerReason reason);
/* 给出结论: true = 用相对位移累加, false = 用绝对坐标。
   前台属于本进程、或光标状态不可知时一律 false (不猜)。 */
bool bongo_cat_windows_pointer_detect(WindowsPointerDetection *state,
    const WindowsPointerObservation *sample);
#endif
#endif
