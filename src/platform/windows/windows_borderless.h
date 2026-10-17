#ifndef BONGO_CAT_WINDOWS_BORDERLESS_H
#define BONGO_CAT_WINDOWS_BORDERLESS_H

#ifdef _WIN32
#include "bongo_cat/platform.h"
#include <windows.h>

/*
 * 无边框 + 点击穿透窗口的接口。
 *
 * 做法是"改样式 + 链一层 WNDPROC": 去掉 WS_CAPTION/WS_THICKFRAME 等边框样式
 * (换成 WS_POPUP), 再把自己的窗口过程接在原来那条前面, 这样 SDL 依然以为自己在
 * 管理一个普通窗口。
 * 穿透有两种强度: forced = 整窗穿透(只在录屏可见之类的场景), pointer_transparent
 * = "按像素透明处穿透", 后者在命中测试时逐像素看 alpha, 并且按下右键时会临时取消
 * 穿透, 以便用户还能拖动窗口。
 * install / uninstall 必须配对, 且 install 幂等(重复调用直接返回)。
 */
void bongo_cat_windows_borderless_install(HWND window);
void bongo_cat_windows_borderless_uninstall(HWND window);
bool bongo_cat_windows_borderless_preserve_screen(HWND window, bool enabled);
/* 穿透判定被单独抽成纯函数, 便于直接推演和测试各种组合: 强制穿透永远穿透;
   "透明处穿透"在按住右键(或窗口正持有 capture)时失效。 */
bool bongo_cat_windows_borderless_hit_transparent(bool forced,
    bool pointer_transparent, bool right_button_down);
void bongo_cat_windows_borderless_set_click_through(HWND window,
    bool forced, bool pointer_transparent);
/* 直接呈现(无代理窗口, 即 Vulkan)时的逐像素穿透: 单独切换 WS_EX_TRANSPARENT。
   注意它让整窗对鼠标"失明", 只能配合"应用每帧从全局轮询重新判定 pointer_transparent"
   来用 —— 主窗口收不到鼠标消息也没关系, 光标落到猫上后下一帧就会清掉。 */
void bongo_cat_windows_borderless_set_pixel_transparent(HWND window, bool enabled);
void bongo_cat_windows_menu_preview(HWND window, BongoCatMenuPreview preview,
    void (*tick)(void *userdata), void *userdata);
void bongo_cat_windows_begin_drag(HWND window, BongoCatModalTick modal_tick,
    void *userdata);
#endif

#endif
