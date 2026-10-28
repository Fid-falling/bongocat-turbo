/*
 * 逻辑/像素尺寸换算的单元测试。
 *
 * 背景: 200% DPI 的自检报告里曾出现 "宠物窗口 564x493 像素 (564x493 逻辑)" —— 看上去
 * 像 OpenGL 后端在 200% 下没把窗口像素翻倍。实际是自检把 SDL_GetWindowSize (Windows 的
 * DPI 感知窗口上就是物理像素) 当成逻辑尺寸打印; 窗口/视口本身一直是 564x493 物理像素。
 *
 * 修完后这里锁死那条对账关系: 像素 = 逻辑 x 布局缩放。数值取自用户的真实报告与 VK 后端
 * 的历史日志, 换算一改 (比如换回截断取整、或把 100% 也除一遍) 就会失败。
 */
#include "ui_backend.h"

#include <stdio.h>

static int failures;

static void expect_size(const char *name, int window_w, int window_h,
    float layout, int want_w, int want_h) {
    int width = -1, height = -1;
    bongo_cat_ui_logical_from_window(window_w, window_h, layout, &width, &height);
    if (width != want_w || height != want_h) {
        fprintf(stderr, "%s: got %dx%d want %dx%d\n", name, width, height,
            want_w, want_h);
        failures++;
    }
}

int main(void) {
    /* 用户报的那组: 200% 下窗口 564x493 像素, 逻辑应是它的一半 (493/2 = 246.5 要
       进位成 247, 才与用户自己算的 ≈282x247 对得上)。 */
    expect_size("windows-200", 564, 493, 2.0f, 282, 247);
    /* VK 后端的历史日志: 窗口=像素=551x482、缩放 2.00, 日志里打出 逻辑=276x241。 */
    expect_size("vulkan-200-log", 551, 482, 2.0f, 276, 241);
    /* 100%: 逻辑等于像素。这条保证修复没把未缩放的情况也除一遍。 */
    expect_size("windows-100", 564, 493, 1.0f, 564, 493);
    /* 125% / 150%: 与 [ui] 设置窗口 日志同源的例子 (1125x850 @1.25 -> 900x680)。 */
    expect_size("windows-125", 1125, 850, 1.25f, 900, 680);
    expect_size("windows-150", 846, 740, 1.5f, 564, 493);
    /* 布局缩放拿不到 (0/负) 时按 1 算, 绝不能除出 0 或负数。 */
    expect_size("zero-layout", 564, 493, 0.0f, 564, 493);
    expect_size("negative-layout", 564, 493, -2.0f, 564, 493);
    /* 最小化时 SDL 可能给 0 尺寸: 结果至少为 1, 供布局与输入安全使用。 */
    expect_size("minimized", 0, 0, 2.0f, 1, 1);
    return failures ? 10 : 0;
}
