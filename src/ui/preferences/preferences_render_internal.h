#ifndef BONGO_CAT_PREFERENCES_RENDER_INTERNAL_H
#define BONGO_CAT_PREFERENCES_RENDER_INTERNAL_H

#include "bongo_cat/preferences.h"

/*
 * 内部接口: 构建整棵设置窗口 UI 树 (含外壳/侧栏/页面/弹窗)。
 * 返回值是"用户点了关闭", 由 present 决定何时真正关窗 —— 绘制中途不能销毁窗口,
 * 否则 Nuklear 还在用的纹理会在它脚下消失。
 */
bool bongo_cat_preferences_draw_frame(BongoCatPreferences *value,
    float width, float height, bool dark);

#endif
