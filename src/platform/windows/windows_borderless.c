#include "windows_borderless.h"
#include "windows_capture.h"
#include "windows_keys.h"
#include "windows_tray.h"

#ifdef _WIN32
#include <stdlib.h>

/*
 * 无边框窗口的窗口过程与状态标记。
 *
 * 所有状态都挂在窗口属性(SetPropW)上, 而不是文件级静态变量: 同一进程里可能同时
 * 存在多个窗口(主窗口、设置窗口、代理窗口), 用属性才能天然按窗口隔离, 而且窗口
 * 销毁时由系统自动清理。
 * 属性名与定时器 ID 是这套机制的对外标识(别的模块按名字读), 改动等于换协议。
 */
static const wchar_t original_proc_property[] = L"BongoCat.BorderlessWindowProc";
static const wchar_t click_through_property[] = L"BongoCat.ClickThrough";
static const wchar_t menu_binding_property[] = L"BongoCat.MenuBinding";
static const wchar_t drag_binding_property[] = L"BongoCat.DragBinding";
static const wchar_t preserve_screen_property[] = L"BongoCat.PreserveScreenPixels";
#define BONGO_CAT_MENU_PREVIEW_TIMER ((UINT_PTR)0xBC4E)
#define BONGO_CAT_DRAG_MODAL_TIMER ((UINT_PTR)0xBC50)
#define BONGO_CAT_DRAG_FRAME_INTERVAL_MS 16
#define BONGO_CAT_CLICK_THROUGH_FORCED ((INT_PTR)1)
#define BONGO_CAT_CLICK_THROUGH_TRANSPARENT_PIXEL ((INT_PTR)2)

bool bongo_cat_windows_borderless_hit_transparent(bool forced,
    bool pointer_transparent, bool right_button_down) {
    return forced || (pointer_transparent && !right_button_down);
}

typedef struct WindowsMenuBinding {
    BongoCatMenuPreview preview;
    void (*tick)(void *userdata);
    void *userdata;
} WindowsMenuBinding;

typedef struct WindowsDragBinding {
    BongoCatModalTick tick;
    void *userdata;
    ULONGLONG last_tick_ms;
    bool ticking;
} WindowsDragBinding;

/* 拖动期间按 16ms 节流手动泵一帧: 拖动会让系统进入模态消息循环, 主循环的定时器
   被饿死, 不主动出帧画面就停住了。ticking 是重入保护 —— 出帧过程本身可能又收到
   消息(例如 WM_PAINT), 不挡住会递归。 */
static void drag_tick(WindowsDragBinding *drag) {
    if (!drag || !drag->tick || drag->ticking) return;
    ULONGLONG now = GetTickCount64();
    if (drag->last_tick_ms && now - drag->last_tick_ms <
        BONGO_CAT_DRAG_FRAME_INTERVAL_MS) return;
    drag->last_tick_ms = now;
    drag->ticking = true;
    drag->tick(drag->userdata);
    drag->ticking = false;
}

/* 去掉标题栏、边框、系统菜单、最小化/最大化按钮, 换成 WS_POPUP; 剩下的非客户区
   由窗口过程里的 WM_NCCALCSIZE / WM_NCPAINT / WM_NCACTIVATE 处理成"没有边框"。 */
static LONG_PTR borderless_style(LONG_PTR style) {
    return (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
}

/*
 * 链式窗口过程: 先处理自己的消息, 处理不了再交回原过程(原过程指针存在窗口属性里)。
 * 分支顺序有讲究: 托盘消息放最前(托盘依赖消息循环), 采集消息放在自己的命中测试
 * 之前, 因为采集的样式事务会改窗口样式, 两者会互相影响。
 */
static LRESULT CALLBACK borderless_window_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    bongo_cat_windows_tray_handle_message(message);
    WNDPROC original = (WNDPROC)GetPropW(window, original_proc_property);
    WindowsMenuBinding *menu = GetPropW(window, menu_binding_property);
    WindowsDragBinding *drag = GetPropW(window, drag_binding_property);
    if (message == WM_NCCALCSIZE && wparam && lparam &&
        GetPropW(window, preserve_screen_property)) {
        NCCALCSIZE_PARAMS *sizes = (NCCALCSIZE_PARAMS *)lparam;
        RECT previous_client = sizes->rgrc[2];
        /* SDL must still calculate the new client rectangle and update its
           border bookkeeping. Its redraw/alignment flags are replaced only
           for our synchronous move-and-resize transaction. */
        CallWindowProcW(original ? original : DefWindowProcW,
            window, message, wparam, lparam);
        RECT overlap;
        if (IntersectRect(&overlap, &previous_client, &sizes->rgrc[0])) {
            /* Both rectangles use screen coordinates. Equal source and
               destination preserve the old frame in place, rather than
               shifting it with the new window origin before the GL swap. */
            sizes->rgrc[1] = overlap;
            sizes->rgrc[2] = overlap;
            return WVR_VALIDRECTS;
        }
        return WVR_REDRAW;
    }
    if (bongo_cat_windows_capture_handle_message(window, message, wparam)) {
        return 0;
    } else if (message == WM_NCHITTEST) {
        INT_PTR click_through = (INT_PTR)GetPropW(
            window, click_through_property);
        if (bongo_cat_windows_borderless_hit_transparent(
                click_through == BONGO_CAT_CLICK_THROUGH_FORCED,
                click_through == BONGO_CAT_CLICK_THROUGH_TRANSPARENT_PIXEL,
                bongo_cat_windows_right_button_down() ||
                    GetCapture() == window)) return HTTRANSPARENT;
    } else if (message == WM_RBUTTONDOWN &&
        (INT_PTR)GetPropW(window, click_through_property) ==
            BONGO_CAT_CLICK_THROUGH_TRANSPARENT_PIXEL) {
        SetCapture(window);
    /* 屏蔽最小化: 无边框窗口被最小化后会变成任务栏上的空壳, 恢复时透明合成也常
       出错。产品上的"隐藏"走 SDL_HideWindow, 不需要系统最小化。 */
    } else if (message == WM_SYSCOMMAND &&
        (wparam & 0xFFF0u) == SC_MINIMIZE) {
        return 0;
    /* SDL 或系统有时会把边框样式改回来(切换全屏、DPI 变化等)。这里在
       WM_STYLECHANGING 改掉"新样式", 再在 WM_STYLECHANGED 复查一次实际值 ——
       只拦其中一个会被某些路径绕过。 */
    } else if (message == WM_STYLECHANGING && wparam == (WPARAM)GWL_STYLE && lparam) {
        STYLESTRUCT *styles = (STYLESTRUCT *)lparam;
        styles->styleNew = (DWORD)borderless_style(styles->styleNew);
    } else if (message == WM_STYLECHANGED && wparam == (WPARAM)GWL_STYLE) {
        LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        LONG_PTR fixed = borderless_style(style);
        if (fixed != style) SetWindowLongPtrW(window, GWL_STYLE, fixed);
    } else if (message == WM_NCPAINT) {
        return 0;
    } else if (message == WM_NCACTIVATE) {
        return TRUE;
    } else if (message == WM_MOUSEACTIVATE) {
        return MA_ACTIVATE;
    /* 菜单高亮预览: 光标在菜单项上移动时通知上层, 让猫做出对应动作。
       flags == 0xffff 且 lparam 为空是"菜单关闭"的约定值, 不能当普通项处理。 */
    } else if (message == WM_MENUSELECT && menu && menu->preview) {
        UINT id = LOWORD(wparam), flags = HIWORD(wparam);
        if (flags == 0xffff && !lparam) return CallWindowProcW(
            original ? original : DefWindowProcW, window, message, wparam, lparam);
        if ((flags & (MF_POPUP | MF_SEPARATOR)) || !id) {
            menu->preview(menu->userdata, BONGO_CAT_MENU_NONE);
        } else {
            BongoCatMenuAction action = (BongoCatMenuAction)id;
            menu->preview(menu->userdata, action);
        }
    } else if (message == WM_TIMER &&
        wparam == BONGO_CAT_MENU_PREVIEW_TIMER && menu && menu->tick) {
        menu->tick(menu->userdata);
        return 0;
    } else if (message == WM_TIMER &&
        wparam == BONGO_CAT_DRAG_MODAL_TIMER && drag && drag->tick) {
        drag_tick(drag);
        return 0;
    }
    LRESULT result = CallWindowProcW(original ? original : DefWindowProcW,
        window, message, wparam, lparam);
    if (message == WM_RBUTTONUP && GetCapture() == window)
        ReleaseCapture();
    /* Move messages keep frames flowing when the low-priority timer is starved. */
    if (drag && (message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE))
        drag_tick(drag);
    return result;
}

/*
 * 拖动窗口: ReleaseCapture 之后伪造一次 WM_NCLBUTTONDOWN/HTCAPTION, 把拖动交给
 * 系统的模态循环 —— 这是无边框窗口能拿到系统级拖动的唯一办法。代价是拖动期间我们
 * 被挡在消息循环里面, 所以必须:
 *   - 先通过 SetPropW 挂上回调并启动定时器, 让窗口过程能在模态循环里继续出帧;
 *   - SendMessage 返回后立刻 KillTimer + 移除属性, 再补一次回调把最后一帧画完。
 * 属性里存的是栈上 binding 的地址, 因此必须在 SendMessage 返回后马上摘掉, 否则
 * 窗口过程会读到已经失效的栈内存。
 */
void bongo_cat_windows_begin_drag(HWND window, BongoCatModalTick modal_tick,
    void *userdata) {
    if (!window) return;
    WindowsDragBinding binding = {modal_tick, userdata};
    bool bound = modal_tick && SetPropW(window, drag_binding_property, &binding);
    if (bound) {
        drag_tick(&binding);
        SetTimer(window, BONGO_CAT_DRAG_MODAL_TIMER,
            BONGO_CAT_DRAG_FRAME_INTERVAL_MS, NULL);
    }
    ReleaseCapture();
    SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    if (bound) {
        KillTimer(window, BONGO_CAT_DRAG_MODAL_TIMER);
        RemovePropW(window, drag_binding_property);
        modal_tick(userdata);
    }
}

/* 绑定是堆分配的(窗口过程通过属性拿到指针), 清理顺序必须是先杀定时器、再摘属性、
   最后 free —— 反了会出现"定时器回调拿到已 free 的 userdata"。 */
static void clear_menu_binding(HWND window) {
    WindowsMenuBinding *menu = window ?
        GetPropW(window, menu_binding_property) : NULL;
    if (!menu) return;
    KillTimer(window, BONGO_CAT_MENU_PREVIEW_TIMER);
    RemovePropW(window, menu_binding_property);
    free(menu);
}

/* 注册 / 更新菜单预览回调(preview 与 tick 都为空表示注销)。重复调用会先清掉旧
   绑定, 所以可以安全地反复调用。 */
void bongo_cat_windows_menu_preview(HWND window, BongoCatMenuPreview preview,
    void (*tick)(void *userdata), void *userdata) {
    if (!window) return;
    clear_menu_binding(window);
    if (!preview && !tick) return;
    WindowsMenuBinding *menu = calloc(1, sizeof(*menu));
    if (!menu) return;
    menu->preview = preview;
    menu->tick = tick;
    menu->userdata = userdata;
    if (!SetPropW(window, menu_binding_property, menu)) {
        free(menu);
        return;
    }
    if (tick) SetTimer(window, BONGO_CAT_MENU_PREVIEW_TIMER, 16, NULL);
}

/* 幂等安装: 已经存在 original_proc_property 就直接返回, 否则先改样式再挂过程。
   先改样式是刻意的 —— 改样式会触发 WM_STYLECHANGED, 那时我们的过程还没接上,
   不会自己和自己打架。 */
void bongo_cat_windows_borderless_install(HWND window) {
    if (!window || GetPropW(window, original_proc_property)) return;
    LONG_PTR style = borderless_style(GetWindowLongPtrW(window, GWL_STYLE));
    SetWindowLongPtrW(window, GWL_STYLE, style);
    WNDPROC original = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (!original || !SetPropW(window, original_proc_property, (HANDLE)original)) return;
    SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)borderless_window_proc);
}

/* 完整还原: 清菜单绑定、杀定时器、摘掉拖动属性、把窗口过程换回原函数, 并移除所有
   标记。漏掉任何一步, 下一次 install 都会因为"属性已存在"而被误判为重复安装。 */
void bongo_cat_windows_borderless_uninstall(HWND window) {
    if (!window) return;
    clear_menu_binding(window);
    KillTimer(window, BONGO_CAT_DRAG_MODAL_TIMER);
    RemovePropW(window, drag_binding_property);
    WNDPROC original = (WNDPROC)GetPropW(window, original_proc_property);
    if (!original) return;
    SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)original);
    RemovePropW(window, original_proc_property);
    RemovePropW(window, click_through_property);
    RemovePropW(window, preserve_screen_property);
}

/* 开关"改尺寸时保住屏幕上的旧像素"。只有装过无边框过程(原过程属性存在)才生效;
   否则窗口过程里没人处理 NCCALCSIZE, 设了也没有人读。 */
bool bongo_cat_windows_borderless_preserve_screen(HWND window, bool enabled) {
    if (!window) return false;
    if (!enabled) {
        RemovePropW(window, preserve_screen_property);
        return true;
    }
    return GetPropW(window, original_proc_property) &&
        SetPropW(window, preserve_screen_property, (HANDLE)1);
}

/* 设置穿透强度。两种表示要同时维护: 窗口属性(给 WM_NCHITTEST 用, 立即生效)和
   WS_EX_TRANSPARENT(给系统与 DWM 用)。只有 forced 才加扩展样式 —— "透明处穿透"
   若也加了, 整窗都不再接收鼠标, 就没法按像素判断了。 */
void bongo_cat_windows_borderless_set_click_through(HWND window,
    bool forced, bool pointer_transparent) {
    if (!window) return;
    INT_PTR mode = forced ? BONGO_CAT_CLICK_THROUGH_FORCED :
        pointer_transparent ? BONGO_CAT_CLICK_THROUGH_TRANSPARENT_PIXEL : 0;
    if (mode) SetPropW(window, click_through_property, (HANDLE)mode);
    else RemovePropW(window, click_through_property);
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    LONG_PTR next = forced ? style | WS_EX_TRANSPARENT :
        style & ~WS_EX_TRANSPARENT;
    /* The hit-test property is immediate; a frame refresh flickers OpenGL windows. */
    if (next != style) SetWindowLongPtrW(window, GWL_EXSTYLE, next);
}
#endif
