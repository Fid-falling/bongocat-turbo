#include "windows_snapshot_internal.h"

#include <windowsx.h>

/*
 * 预览窗口的输入转发: 预览窗口盖在源窗口上面, 用户的鼠标其实落在预览窗口上, 必须由它
 * 判断"这个点是不是猫的身体", 并把鼠标消息原样转交给源窗口 —— 这样 SDL 那边完全不必
 * 知道预览窗口的存在。
 */
static const wchar_t snapshot_property[] = L"BongoCat.InteractionSnapshot";

/* 命中测试: 先把源窗口客户区坐标换算到图像坐标, 再换算成抓图缓冲里的像素下标;
   y 轴要翻转(抓回来的缓冲是自下而上的 GL 行序), 最后按 alpha > 8 判定命中。
   越界一律算不命中 —— 参数来自鼠标消息, 不能假设它一定在范围内。 */
bool bongo_cat_windows_snapshot_hit(BongoCatWindowsSnapshot *s, float x, float y) {
    RECT rect;
    if (!s || !GetWindowRect(s->source_handle, &rect) ||
        rect.right <= rect.left || rect.bottom <= rect.top ||
        x < 0 || y < 0 || x >= rect.right - rect.left || y >= rect.bottom - rect.top)
        return false;
    SDL_FRect destination = s->custom_destination ? s->destination :
        (SDL_FRect){(float)rect.left, (float)rect.top,
            (float)(rect.right - rect.left), (float)(rect.bottom - rect.top)};
    x += rect.left - destination.x;
    y += rect.top - destination.y;
    if (x < 0 || y < 0 || x >= destination.w || y >= destination.h) return false;
    int px = SDL_clamp((int)(x * s->width / destination.w), 0, s->width - 1);
    int py = s->height - 1 - SDL_clamp(
        (int)(y * s->height / destination.h), 0, s->height - 1);
    return s->pixels[((size_t)py * s->width + px) * 4 + 3] > 8;
}

/* 按当前光标位置刷新预览窗口的穿透状态(WS_EX_TRANSPARENT): 光标在猫身上时窗口要接收
   鼠标, 移开后立刻恢复穿透, 否则桌面上会留下一块"看不见却点得到"的区域。 */
void bongo_cat_windows_snapshot_pointer(BongoCatWindowsSnapshot *s) {
    POINT point;
    if (!s || !GetCursorPos(&point) || !ScreenToClient(s->source_handle, &point)) return;
    bool hit = bongo_cat_windows_snapshot_hit(s, (float)point.x, (float)point.y);
    LONG_PTR style = GetWindowLongPtrW(s->handle, GWL_EXSTYLE);
    LONG_PTR next = hit ? style & ~WS_EX_TRANSPARENT : style | WS_EX_TRANSPARENT;
    if (next != style) SetWindowLongPtrW(s->handle, GWL_EXSTYLE, next);
}

/* 预览窗口的窗口过程: WM_NCHITTEST 决定穿透(命中返回 HTCLIENT, 否则 HTTRANSPARENT),
   WM_MOUSEACTIVATE 返回 MA_NOACTIVATE 以免抢焦点, 鼠标消息把坐标从预览窗口映射回源
   窗口后原样转发 —— 滚轮消息的坐标本来就是屏幕坐标, 不能再映射一次(见下面原有的
   英文注释)。 */
static LRESULT CALLBACK snapshot_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    BongoCatWindowsSnapshot *s = GetPropW(window, snapshot_property);
    if (!s) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_NCHITTEST) {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(s->source_handle, &point);
        return bongo_cat_windows_snapshot_hit(s, (float)point.x, (float)point.y)
            ? HTCLIENT : HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
        /* Wheel coordinates are already screen-relative. Other mouse messages
           must look as though SDL received them on the real pet window. */
        if (message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) {
            POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(window, s->source_handle, &point, 1);
            lparam = MAKELPARAM(point.x, point.y);
        }
        return SendMessageW(s->source_handle, message, wparam, lparam);
    }
    return CallWindowProcW(s->original, window, message, wparam, lparam);
}

/* 绑定: 先把状态对象挂到窗口属性上(窗口过程要靠它才能工作), 再换窗口过程。 */
bool bongo_cat_windows_snapshot_bind_input(BongoCatWindowsSnapshot *s) {
    if (!SetPropW(s->handle, snapshot_property, s)) return false;
    s->original = (WNDPROC)GetWindowLongPtrW(s->handle, GWLP_WNDPROC);
    return SetWindowLongPtrW(s->handle, GWLP_WNDPROC, (LONG_PTR)snapshot_proc) != 0;
}

/* 解绑: 换回原过程并摘掉属性。必须在销毁窗口之前调用 —— 窗口没了就取不到属性。 */
void bongo_cat_windows_snapshot_unbind_input(BongoCatWindowsSnapshot *s) {
    if (s->original)
        SetWindowLongPtrW(s->handle, GWLP_WNDPROC, (LONG_PTR)s->original);
    RemovePropW(s->handle, snapshot_property);
}
