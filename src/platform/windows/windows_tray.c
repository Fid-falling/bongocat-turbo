#include "bongo_cat/platform.h"
#include "windows_popup.h"
#include "windows_tray.h"
#include "bongo_cat/tray.h"

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>

#define BONGO_CAT_TRAY_MODAL_TIMER ((UINT_PTR)0xBC4F)
/*
 * 通过接管 SDL 托盘窗口的窗口过程来实现托盘交互。
 *
 * 为什么这么做: SDL 把 SDL_Tray* 放在它自己 message-only 窗口的 GWLP_USERDATA 里,
 * 又不暴露窗口过程, 只能按类名找到那个窗口再链一层自己的过程。绑定对象存成窗口属性,
 * 与原过程指针一起保存, 卸载时还要把过程换回去。
 * 正因为是"认窗口"的做法, 绑定前必须严格校验(见 bind_tray_window): 必须是本进程的、
 * 类名为 "Message"、且 USERDATA 正好等于预期的 SDL_Tray* —— 缺一条就可能挂到别人的
 * 窗口上。
 */
static const wchar_t tray_binding_property[] = L"BongoCat.TrayBinding";
static UINT taskbar_created_message;
static void *restore_tray;
static BongoCatTrayRestore restore_callback;
static void *restore_userdata;

/* 绑定包: 原窗口过程 + 回调 + userdata。窗口过程通过窗口属性拿它, 所以它的生命周期
   必须覆盖整段绑定期间(堆分配, 卸载时 free)。 */
typedef struct WindowsTrayBinding {
    WNDPROC original;
    BongoCatTrayClick left_click;
    BongoCatModalTick modal_tick;
    void *userdata;
} WindowsTrayBinding;

/* WM_USER+1 是 Shell_NotifyIcon 的回调消息(SDL 用的就是它), lParam 低位是鼠标消息。
   右键时先准备菜单、起一个 16ms 定时器让猫继续出帧, 再调用原过程真正弹出菜单 —— 菜单
   是模态的, 这期间主循环收不到消息, 不主动泵帧画面就会僵住; 菜单结束后补一次
   popup_complete 把焦点还回去。 */
static LRESULT CALLBACK tray_window_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    WindowsTrayBinding *binding = GetPropW(window, tray_binding_property);
    WNDPROC original = binding ? binding->original : NULL;
    if (message == WM_USER + 1 && LOWORD(lparam) == WM_LBUTTONUP &&
        binding && binding->left_click) {
        binding->left_click(binding->userdata);
        return 0;
    }
    if (message == WM_USER + 1 && LOWORD(lparam) == WM_CONTEXTMENU && binding) {
        bongo_cat_tray_prepare_menu(binding->userdata, window);
        if (binding->modal_tick) {
            binding->modal_tick(binding->userdata);
            SetTimer(window, BONGO_CAT_TRAY_MODAL_TIMER, 16, NULL);
        }
        LRESULT result = CallWindowProcW(original ? original : DefWindowProcW,
            window, message, wparam, lparam);
        KillTimer(window, BONGO_CAT_TRAY_MODAL_TIMER);
        bongo_cat_windows_popup_complete(window);
        return result;
    }
    if (message == WM_TIMER && wparam == BONGO_CAT_TRAY_MODAL_TIMER &&
        binding && binding->modal_tick) {
        binding->modal_tick(binding->userdata);
        return 0;
    }
    return CallWindowProcW(original ? original : DefWindowProcW,
        window, message, wparam, lparam);
}

/* 卸载: 杀定时器、把窗口过程换回原函数(先确认当前挂的确实是我们, 否则会覆盖掉别人
   插进来的过程)、摘掉属性、free 绑定包。 */
static void unbind_tray_window(HWND window) {
    WindowsTrayBinding *binding = GetPropW(window, tray_binding_property);
    if (!binding) return;
    KillTimer(window, BONGO_CAT_TRAY_MODAL_TIMER);
    if ((WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC) == tray_window_proc)
        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)binding->original);
    RemovePropW(window, tray_binding_property);
    free(binding);
}

/* 绑定前的三重校验见文件头注释。已经绑定过就先解绑(幂等); 两个回调都为空时直接返回。
   末尾对 SetWindowLongPtr 做 SetLastError 检查 —— 它返回旧值 0 也可能是成功。 */
static void bind_tray_window(HWND window, void *tray,
    BongoCatTrayClick left_click, BongoCatModalTick modal_tick,
    void *userdata) {
    wchar_t name[32]; DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() ||
        !GetClassNameW(window, name, (int)(sizeof(name) / sizeof(name[0]))) ||
        wcscmp(name, L"Message") != 0 ||
        (void *)GetWindowLongPtrW(window, GWLP_USERDATA) != tray) return;
    unbind_tray_window(window);
    if (!left_click && !modal_tick) return;
    WindowsTrayBinding *binding = calloc(1, sizeof(*binding));
    if (!binding) return;
    binding->original = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
    binding->left_click = left_click;
    binding->modal_tick = modal_tick;
    binding->userdata = userdata;
    if (!binding->original || binding->original == tray_window_proc ||
        !SetPropW(window, tray_binding_property, binding)) {
        free(binding);
        return;
    }
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(window, GWLP_WNDPROC,
        (LONG_PTR)tray_window_proc) && GetLastError() != ERROR_SUCCESS) {
        RemovePropW(window, tray_binding_property);
        free(binding);
    }
}

/* 注册回调并刷新所有托盘窗口上的绑定。restore 回调只在首次注册时记住; 注销时只有
   "传进来的 tray 就是当初注册的那个"才清掉, 免得误清别的实例的回调(测试会同时创建
   多个托盘)。 */
void bongo_cat_platform_set_tray_callbacks(void *tray,
    BongoCatTrayClick left_click, BongoCatModalTick modal_tick,
    BongoCatTrayRestore restore, void *userdata) {
    if (!taskbar_created_message)
        taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    if (tray && restore) {
        restore_tray = tray;
        restore_callback = restore;
        restore_userdata = userdata;
    } else if (!tray || tray == restore_tray) {
        restore_tray = NULL;
        restore_callback = NULL;
        restore_userdata = NULL;
    }
    /* Pinned SDL stores SDL_Tray* in its private message window userdata. */
    HWND window = NULL;
    while ((window = FindWindowExW(HWND_MESSAGE, window, L"Message", NULL)) != NULL)
        bind_tray_window(window, tray, left_click, modal_tick, userdata);
}

/* 资源管理器重启后系统会广播 TaskbarCreated, 托盘图标需要重建。这条消息由窗口过程
   转发进来(windows_borderless.c 里最先处理的就是它)。 */
void bongo_cat_windows_tray_handle_message(UINT message) {
    if (!taskbar_created_message)
        taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbar_created_message && message == taskbar_created_message &&
        restore_callback)
        restore_callback(restore_userdata);
}
#endif
