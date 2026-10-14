#ifndef BONGO_CAT_WINDOW_DISPLAY_GEOMETRY_H
#define BONGO_CAT_WINDOW_DISPLAY_GEOMETRY_H

/*
 * 显示器几何的纯计算部分 (window_display_geometry.c): 判断窗口与显示器并集的关系、
 * 把矩形夹进某块显示器、以及这些判断的自检。
 *
 * 为什么单独一个文件: window_display.c 已经贴着 500 行的策略上限 (见
 * cmake/CheckLines.cmake), 而这些函数是"不碰窗口、不读 app 状态"的纯几何 —— 搬出来
 * 之后 window_display.c 只留"什么时候该动窗口"。自检跟着实现走, 免得两边分家。
 *
 * 所有权约定: 这些函数只做计算, 不移动窗口、不改 app 状态; 需要动窗口的地方一律回到
 * window_display.c 的 clamp_to_display / drag_to / recover_to_display。
 */
#include "bongo_cat/common.h"
#include <SDL3/SDL_rect.h>

/* 栈上定长数组的容量约定: 枚举到的显示器超过这个数就放弃判断 —— 宁愿不动窗口。 */
#define DISPLAY_RECT_CAP 32

bool bongo_cat_window_intersects_bounds(const SDL_Rect *window,
    const SDL_Rect *bounds, int count);
/* 窗口是否与任一显示器相交。*known 区分"确实都不相交"与"枚举不到显示器": 后者调用方
   不能动窗口 —— 在没有输出的环境里没有可夹取的边界。 */
bool bongo_cat_window_intersects_displays(const SDL_Rect *window, bool *known);
/* 把位置夹进一块显示器的 bounds; 纯计算, 不碰窗口。 */
SDL_Point bongo_cat_window_fitted_position(const SDL_Rect *window,
    const SDL_Rect *bounds);
/* 窗口矩形是否被若干显示器矩形的并集完整覆盖 (允许跨显示器拼接与负坐标)。 */
bool bongo_cat_window_bounds_covered(const SDL_Rect *window,
    const SDL_Rect *bounds, int count);

#endif
