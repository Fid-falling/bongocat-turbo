#include "test.h"
#include "windows_input_detection.h"
#include <limits.h>

/* 判定规则只有两条状态: 光标隐藏 / 被裁进 2x2。这里把每条都钉死, 顺便把"运动再多也不
   改变结论"写进用例 —— 之前的"一抽一抽"就是因为让运动参与了判断。 */

static WindowsPointerObservation observation(void) {
    return (WindowsPointerObservation){.foreground = (HWND)(uintptr_t)1,
        .pid = 2, .foreign = true, .position_known = true, .monitor_known = true,
        .clip_known = true, .cursor_known = true, .cursor_flags = CURSOR_SHOWING,
        .position = {960, 540}, .monitor = {0, 0, 1920, 1080},
        .clip = {0, 0, 1920, 1080}};
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
    sample.cursor_flags = 0;
    CHECK(bongo_cat_windows_pointer_detect(&state, &sample));
    CHECK(state.reason == WINDOWS_POINTER_HIDDEN);
    sample.cursor_flags = CURSOR_SHOWING;
    CHECK(!bongo_cat_windows_pointer_detect(&state, &sample));
    CHECK(state.reason == WINDOWS_POINTER_DESKTOP);
    /* 来回切: 每次都是"隐藏=相对, 显示=绝对", 没有任何滞回 */
    for (int i = 0; i < 20; ++i) {
        sample.cursor_flags = i % 2 ? CURSOR_SHOWING : 0;
        bool expected = (i % 2) == 0;
        bool actual = bongo_cat_windows_pointer_detect(&state, &sample);
        if (actual != expected)
            fprintf(stderr, "switch round=%d actual=%d\n", i, (int)actual);
        CHECK(actual == expected);
    }
}

static void test_clipped_means_relative(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    sample.clip = (RECT){960, 540, 962, 542};
    CHECK(bongo_cat_windows_pointer_detect(&state, &sample));
    CHECK(state.reason == WINDOWS_POINTER_CLIPPED);
    sample.clip = sample.monitor;
    CHECK(!bongo_cat_windows_pointer_detect(&state, &sample));
    /* 光标隐藏 + 极小区同时成立时报裁剪 (更强的那个原因) */
    sample.cursor_flags = 0;
    sample.clip = (RECT){960, 540, 961, 541};
    CHECK(bongo_cat_windows_pointer_detect(&state, &sample));
    CHECK(state.reason == WINDOWS_POINTER_CLIPPED);
}

static void test_motion_does_not_change_the_decision(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    sample.cursor_flags = 0;
    /* 光标在游戏里被 recenter 前会飞出去几百像素: 结论必须是"一直是相对", 不能抖 */
    for (int i = 0; i < 400; ++i) {
        sample.position.x = (i % 40 < 20) ? 960 : 960 + (i % 300);
        sample.position.y = 540 + (i % 120) - 60;
        CHECK(bongo_cat_windows_pointer_detect(&state, &sample));
    }
    /* 反过来: 光标显示着的时候, 位置怎么动都还是绝对 */
    sample.cursor_flags = CURSOR_SHOWING;
    for (int i = 0; i < 200; ++i) {
        sample.position.x = 100 + i * 5;
        CHECK(!bongo_cat_windows_pointer_detect(&state, &sample));
    }
}

static void test_foreign_and_unknown_cursor(void) {
    WindowsPointerDetection state = {0};
    WindowsPointerObservation sample = observation();
    sample.cursor_flags = 0;
    /* 前台是自己进程: 永远不切相对 (自己的窗口不会独占鼠标) */
    sample.foreign = false;
    CHECK(!bongo_cat_windows_pointer_detect(&state, &sample));
    CHECK(state.reason == WINDOWS_POINTER_DESKTOP);
    /* 光标状态取不到: 不猜 */
    sample.foreign = true;
    sample.cursor_known = false;
    sample.clip_known = false;
    CHECK(!bongo_cat_windows_pointer_detect(&state, &sample));
    /* 样本为空也要安全退出 */
    CHECK(!bongo_cat_windows_pointer_detect(&state, NULL));
    CHECK(!bongo_cat_windows_pointer_detect(NULL, &sample));
}

void test_windows_pointer_detection(void) {
    test_clip_bounds();
    test_hidden_and_shown_switch();
    test_clipped_means_relative();
    test_motion_does_not_change_the_decision();
    test_foreign_and_unknown_cursor();
}
