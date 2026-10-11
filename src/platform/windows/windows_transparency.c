#include "windows_capture.h"
#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"

/*
 * 透明窗口(带 alpha 合成)的安装与修复, 以及"只在录屏软件里显示"。
 *
 * 背景: SDL 的 OpenGL 窗口默认走不透明的 DWM 合成路径, 而桌宠要求客户区的透明区域真的
 * 透出桌面和它后面的窗口。做法是给窗口开启 DWM 的 blur-behind(传一个空 region 表示"客户区
 * 不做不透明合成"), 再把窗口从系统自动加上的圆角与边框里摘出来。
 * 这些合成状态并不稳定: 换分辨率、改 DPI、换主题、资源管理器重启都会让 DWM 重建表面,
 * 所以需要一套"消息驱动 + 兜底修复"的机制(见 schedule_repair)。
 * 所有状态都挂成窗口属性(SetPropW), 因而能在同一进程里按窗口区分; 用户可见的日志只在首次
 * 失败时打一条, 免得每帧刷屏。
 */
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <dwmapi.h>

#ifndef DWMWA_CLOAK
/* 老版本 SDK 的 dwmapi.h 里还没有这个名字 (Windows 10 2004+ 才有)。 */
#define DWMWA_CLOAK 13
#endif

static const wchar_t capture_only_property[] = L"BongoCat.CaptureOnly";

static HWND platform_native_window(BongoCatPlatform *platform) {
    if (!platform || !platform->window) return NULL;
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* 三个窗口属性的分工: transparent_property 标记"这是需要保持透明的窗口"; proc_property
   保存被我们替换下来的原始窗口过程; repair_property 表示"已经排过一次异步修复", 用来给
   PostMessage 去重 —— 连续收到多条 DWM 通知时只需要修一次。 */
static UINT transparency_repair_message;
static const wchar_t transparent_property[] = L"BongoCat.TransparentWindow";
static const wchar_t transparency_proc_property[] =
    L"BongoCat.TransparentWindowProc";
static const wchar_t transparency_repair_property[] =
    L"BongoCat.TransparentWindowRepairPending";
static bool transparency_warning_emitted;

static bool is_transparent(HWND window) {
    return window && GetPropW(window, transparent_property) != NULL;
}

/* 登记 / 注销"透明窗口"标记。关闭时连修复标记一起清掉: 那个标记只在窗口仍然是透明窗口时
   才有意义, 留着会让后续的修复请求打在一个已经不需要修复的窗口上。 */
void bongo_cat_windows_capture_mark_transparent(HWND window, bool enabled) {
    if (!window) return;
    if (enabled) SetPropW(window, transparent_property, (HANDLE)1);
    else {
        RemovePropW(window, transparent_property);
        RemovePropW(window, transparency_repair_property);
    }
}

static void register_message(void) {
    if (!transparency_repair_message)
        transparency_repair_message = RegisterWindowMessageW(
            L"BongoCat.TransparentWindow.Repair");
}

/* 链式窗口过程: 先让采集模块处理(它会拦掉 WM_ERASEBKGND 之类), 再交回原过程。
   原过程指针存在窗口属性里; 万一取不到就退到 DefWindowProcW, 至少不让消息丢掉。 */
static LRESULT CALLBACK transparency_window_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    if (bongo_cat_windows_capture_handle_message(window, message, wparam))
        return 0;
    WNDPROC original = (WNDPROC)GetPropW(window, transparency_proc_property);
    return CallWindowProcW(original ? original : DefWindowProcW, window,
        message, wparam, lparam);
}

/* 幂等安装: 已经有 proc 属性就直接返回。必须先记录原过程再替换, 顺序反了会把我自己的过程
   当成原过程存起来, 卸载时变成自引用。SetWindowLongPtr 失败时把属性摘掉, 保持状态一致。 */
void bongo_cat_windows_capture_install_transparency_handler(HWND window) {
    if (!window || GetPropW(window, transparency_proc_property)) return;
    WNDPROC original = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (!original || !SetPropW(window, transparency_proc_property,
        (HANDLE)original)) return;
    if (!SetWindowLongPtrW(window, GWLP_WNDPROC,
        (LONG_PTR)transparency_window_proc))
        RemovePropW(window, transparency_proc_property);
}

/*
 * 重新建立 DWM 的透明合成。三个要点:
 *   - blur-behind 传一个空 region((-1,-1)-(0,0))是"客户区不做不透明合成"的惯用手法;
 *   - 成功之后还要显式关掉圆角与边框, 因为 DWM 可能在 shell/DPI 变化后自己把它们加回来;
 *   - region 用完必须 DeleteObject, 否则每次修复都漏一个 GDI 对象。
 * 失败只在第一次记警告: 这个函数会被消息驱动反复调用, 每次都记会淹没日志。
 */
bool bongo_cat_windows_capture_restore_transparency(HWND window) {
    if (!window) return false;
    HRGN region = CreateRectRgn(-1, -1, 0, 0);
    if (!region) return false;
    DWM_BLURBEHIND blur = {0};
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = TRUE;
    blur.hRgnBlur = region;
    HRESULT result = DwmEnableBlurBehindWindow(window, &blur);
    DeleteObject(region);
    if (SUCCEEDED(result)) {
        /* DWM may restore a border or rounded corner after a shell/DPI
           transition. Keep transparent popup windows borderless as well. */
        const int no_rounding = 1; /* DWMWCP_DONOTROUND */
        const DWORD no_border = 0xfffffffeu; /* DWMWA_COLOR_NONE */
        DwmSetWindowAttribute(window, 33, &no_rounding, sizeof(no_rounding));
        DwmSetWindowAttribute(window, 34, &no_border, sizeof(no_border));
    }
    if (FAILED(result) && !transparency_warning_emitted) {
        transparency_warning_emitted = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Cannot restore Windows transparent composition (0x%08lx)",
            (unsigned long)result);
    }
    return SUCCEEDED(result);
}

void bongo_cat_windows_capture_repair_transparency(HWND window) {
    if (!is_transparent(window)) return;
    if (bongo_cat_windows_capture_restore_transparency(window)) {
        /* A composition reset need not produce a paint message. */
        InvalidateRect(window, NULL, FALSE);
    }
}

/* 排一次异步修复: 用 PostMessage 而不是直接调用, 是为了让修复发生在当前这轮消息处理结束、
   窗口状态稳定之后。repair_property 保证同一时刻只有一个待处理请求(PostMessage 失败时
   要把标记撤掉, 否则这个窗口再也不会被修复)。 */
static void schedule_repair(HWND window) {
    if (!is_transparent(window)) return;
    register_message();
    if (!transparency_repair_message ||
        GetPropW(window, transparency_repair_property)) return;
    if (SetPropW(window, transparency_repair_property, (HANDLE)1) &&
        !PostMessageW(window, transparency_repair_message, 0, 0))
        RemovePropW(window, transparency_repair_property);
}

/*
 * 消息分发。三条规则:
 *   - 透明窗口必须自己吞掉 WM_ERASEBKGND: SDL 默认的擦背景会把客户区填成不透明的黑;
 *   - 只在一组"合成状态可能被重置"的消息上排修复, 刻意不处理 WM_WINDOWPOSCHANGED(见下面
 *     原有的英文注释: 普通移动/缩放本来就保住透明, 每次修复反而会让动画表面反复失效);
 *   - 收到我们自己的修复消息时先摘标记再修复, 于是修复过程中若又收到通知, 还能再排一次。
 */
bool bongo_cat_windows_capture_handle_transparency_message(
    HWND window, UINT message, WPARAM wparam) {
    (void)wparam;
    register_message();
    if (is_transparent(window)) {
        /* SDL's default erase handler fills the client area with black. */
        if (message == WM_ERASEBKGND) return true;
        /* Ordinary moves/resizes preserve transparency. Repairing on every
           WM_WINDOWPOSCHANGED repeatedly invalidates the animated surface. */
        switch (message) {
        case WM_DWMCOMPOSITIONCHANGED:
        case WM_DWMNCRENDERINGCHANGED:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
        case WM_THEMECHANGED:
        case WM_NCACTIVATE:
        case WM_SHOWWINDOW:
        case WM_STYLECHANGED:
            schedule_repair(window);
            break;
        default:
            break;
        }
    }
    if (transparency_repair_message && message == transparency_repair_message) {
        RemovePropW(window, transparency_repair_property);
        bongo_cat_windows_capture_repair_transparency(window);
        return true;
    }
    return false;
}

/* Windows 支持"只在录屏软件里显示": 靠 DWM 的 cloak 实现, 见下面那段块注释。 */
bool bongo_cat_platform_capture_only_supported(void) { return true; }

/*
 * "只在录屏软件里显示": 用 DWM 把窗口从桌面合成里摘掉 (DWMWA_CLOAK)。
 *
 * 被隐藏的窗口本身照常渲染、照常呈现, 只是 DWM 不再把它画到屏幕上, 因此
 * 采集侧仍然拿得到画面:
 *   - 游戏采集 (Graphics Hook) 直接读交换链/GL 呈现, 与窗口是否可见无关;
 *   - 窗口采集读的是窗口自己的内容, 也与桌面是否显示无关。
 * 桌面这边则看不到任何东西 —— 主窗口在桌面上完全消失。
 *
 * 因为窗口在桌面上不可见, 点击穿透必须整体生效 (见 windows_pointer.c):
 * 否则用户会在看不见的窗口上点到一只隐形的猫。
 */
bool bongo_cat_platform_set_capture_only(BongoCatPlatform *platform, bool enabled) {
    HWND window = platform_native_window(platform);
    if (!window) return false;
    /* 属性值取 1/2 而不是 0/1: GetPropW 返回 NULL 表示"没有这个属性", 与"值恰好是 0"
       无法区分, 所以两种状态各用一个非 0 值。 */
    /* 属性已经是我们想要的状态时不再打扰 DWM: 这个函数会被反复调用。 */
    HANDLE applied = GetPropW(window, capture_only_property);
    if (applied && (applied == (HANDLE)(INT_PTR)2) == enabled) {
        platform->capture_only = enabled;
        return true;
    }
    BOOL cloak = enabled ? TRUE : FALSE;
    HRESULT result = DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak,
        sizeof(cloak));
    if (FAILED(result)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Cannot %s the capture-only window hiding (0x%08lx)",
            enabled ? "enable" : "disable", (unsigned long)result);
        return false;
    }
    SetPropW(window, capture_only_property, (HANDLE)(INT_PTR)(enabled ? 2 : 1));
    platform->capture_only = enabled;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] capture-only=%d (仅在录屏软件中显示: %s, 桌面%s显示, 采集仍可捕获)",
        enabled ? 1 : 0, enabled ? "已开启" : "已关闭", enabled ? "不再" : "恢复");
    /* 采集状态是代理分层的第二个输入(桌面上看不见时不启用代理): 它变了必须立刻重下发,
       否则代理窗口会跟着"桌面不显示"一起失效, 或者恢复显示后一直空着。 */
    bongo_cat_platform_apply_transparency(platform);
    return true;
}
#endif
