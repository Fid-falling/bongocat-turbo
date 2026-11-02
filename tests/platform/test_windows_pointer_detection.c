#include "test.h"
#include "windows_input_detection.h"
#include <limits.h>

/* 判定规则: 光标隐藏 / 被裁进 2x2 就切相对位移; 但切换需要新状态**持续**
   BONGO_CAT_POINTER_SWITCH_MS (防抖) —— 光标在某些游戏里会逐帧 hidden/shown 来回翻,
   跟着翻会把模型位置反复重置 (用户看到的"一抽一抽")。这几条用例把两件事一起钉住。 */

static WindowsPointerObservation observation(void) {
    return (WindowsPointerObservation){.foreground = (HWND)(uintptr_t)1,
        .pid = 2, .foreign = true, .position_known = true, .monitor_known = true,
        .clip_known = true, .cursor_known = true, .cursor_flags = CURSOR_SHOWING,
        .position = {960, 540}, .monitor = {0, 0, 1920, 1080},
        .clip = {0, 0, 1920, 1080}};
}

/* 以 16ms 为步长持续喂同一个观察, 返回最后一次的结论。 */
static bool hold(WindowsPointerDetection *state,
    const WindowsPointerObservation *sample, ULONGLONG *clock, unsigned ms) {
    bool result = false;
    for (unsigned elapsed = 0; elapsed <= ms; elapsed += 16) {
        *clock += 16;
        result = bongo_cat_windows_pointer_detect(state, sample, *clock);
    }
    return result;
}

static void test_clip_bounds(void) {
    static const struct {
        RECT clip;
        bool locked;
    } cases[] = {
        {{-1920, -1080, 0, 0}, false},
        {{-1920, -1080, -1, -1}, false},
        {{-1920, -1080, 1920, 1080}, false},
        {{0, 0, 1920, 1080}, false},
        {{-2, -2, 0, 0}, true},
        {{-1, -1, 1, 1}, true},
        {{960, 540, 961, 541}, true},
        {{0, 0, 0, 0}, true},
        {{-3, -2, 0, 0}, false},
        {{-2, -3, 0, 0}, false},
        {{1, 0, 0, 1}, false},
        {{0, 1, 1, 0}, false},
        {{LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX}, false},
        {{LONG_MIN, LONG_MIN, LONG_MIN + 2, LONG_MIN + 2}, true},
        {{LONG_MAX - 2, LONG_MAX - 2, LONG_MAX, LONG_MAX}, true}
    };
    CHECK(!bongo_cat_windows_pointer_clip_locked(NULL));
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bool locked = bongo_cat_windows_pointer_clip_locked(&cases[i].clip);
        if (locked != cases[i].locked)
            fprintf(stderr, "clip bounds case=%u locked=%d\n", i, (int)locked);
        CHECK(locked == cases[i].locked);
    }
}

static void test_hidden_and_shown_switch(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    ULONGLONG clock = 1000;
    sample.cursor_flags = 0;
    /* 防抖: 只喂一帧不够 */
    clock += 16;
    CHECK(!bongo_cat_windows_pointer_detect(&state, &sample, clock));
    CHECK(hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 64));
    CHECK(state.reason == WINDOWS_POINTER_HIDDEN);
    sample.cursor_flags = CURSOR_SHOWING;
    CHECK(!hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 64));
    CHECK(state.reason == WINDOWS_POINTER_DESKTOP);
}

/* 逐帧来回翻 (实测游戏里 30ms 内 hidden -> shown -> hidden): 一次都不许切换。 */
static void test_flicker_never_switches(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    ULONGLONG clock = 5000;
    for (int round = 0; round < 40; ++round) {
        sample.cursor_flags = (round % 2) ? CURSOR_SHOWING : 0;
        clock += 16;
        bool locked = bongo_cat_windows_pointer_detect(&state, &sample, clock);
        if (locked) fprintf(stderr, "flicker switched at round=%d\n", round);
        CHECK(!locked);
    }
    /* 之后真的稳定隐藏 -> 正常切换 */
    sample.cursor_flags = 0;
    CHECK(hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 64));
}

static void test_clipped_means_relative(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    ULONGLONG clock = 20000;
    sample.clip = (RECT){960, 540, 962, 542};
    CHECK(hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 64));
    CHECK(state.reason == WINDOWS_POINTER_CLIPPED);
    sample.clip = sample.monitor;
    CHECK(!hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 64));
    /* 光标隐藏 + 极小区同时成立时报裁剪 (更强的那个原因) */
    sample.cursor_flags = 0;
    sample.clip = (RECT){960, 540, 961, 541};
    CHECK(hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 64));
    CHECK(state.reason == WINDOWS_POINTER_CLIPPED);
}

static void test_motion_does_not_change_the_decision(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    ULONGLONG clock = 40000;
    sample.cursor_flags = 0;
    CHECK(hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 32));
    /* 光标在游戏里被 recenter 之前会飞出去几百像素: 结论必须一直是"相对", 不能抖 */
    for (int i = 0; i < 400; ++i) {
        sample.position.x = (i % 40 < 20) ? 960 : 960 + (i % 300);
        sample.position.y = 540 + (i % 120) - 60;
        clock += 16;
        CHECK(bongo_cat_windows_pointer_detect(&state, &sample, clock));
    }
    /* 光标显示着的时候, 位置怎么动都还是绝对 */
    sample.cursor_flags = CURSOR_SHOWING;
    CHECK(!hold(&state, &sample, &clock, BONGO_CAT_POINTER_SWITCH_MS + 32));
    for (int i = 0; i < 200; ++i) {
        sample.position.x = 100 + i * 5;
        clock += 16;
        CHECK(!bongo_cat_windows_pointer_detect(&state, &sample, clock));
    }
}

static void test_foreign_and_unknown_cursor(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    ULONGLONG clock = 90000;
    sample.cursor_flags = 0;
    /* 前台是自己进程: 永远不切相对 */
    sample.foreign = false;
    CHECK(!hold(&state, &sample, &clock, 400));
    CHECK(state.reason == WINDOWS_POINTER_DESKTOP);
    /* 光标状态取不到: 不猜 */
    sample.foreign = true;
    sample.cursor_known = false;
    sample.clip_known = false;
    CHECK(!hold(&state, &sample, &clock, 400));
    /* 空样本/空状态也要安全退出 */
    CHECK(!bongo_cat_windows_pointer_detect(&state, NULL, clock));
    CHECK(!bongo_cat_windows_pointer_detect(NULL, &sample, clock));
}

void test_windows_pointer_detection(void) {
    test_clip_bounds();
    test_hidden_and_shown_switch();
    test_flicker_never_switches();
    test_clipped_means_relative();
    test_motion_does_not_change_the_decision();
    test_foreign_and_unknown_cursor();
}
