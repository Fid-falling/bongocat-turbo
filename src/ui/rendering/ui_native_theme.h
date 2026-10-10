#ifndef BONGO_CAT_UI_NATIVE_THEME_H
#define BONGO_CAT_UI_NATIVE_THEME_H

#include <stdbool.h>

typedef struct SDL_Window SDL_Window;

/* 把窗口的"原生"部分 (标题栏、菜单、系统绘制的边框) 调成与界面主题一致。
   非 Windows 平台上这三个函数是空实现, 调用方不必加条件编译。
   *_native 变体接收裸 HWND, 给已经脱离 SDL_Window 的路径用 (例如菜单弹出时)。 */
void bongo_cat_ui_native_theme_apply(SDL_Window *window, bool dark);
void bongo_cat_ui_native_menu_prepare(SDL_Window *window, bool dark);
void bongo_cat_ui_native_menu_prepare_native(void *handle, bool dark);

#endif
