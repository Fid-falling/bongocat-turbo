#include "windows_layered.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"
#include "windows_layered_internal.h"
#include "windows_capture.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <windows.h>

/*
 * "第二个窗口"的分层呈现接入点 —— 目前只有右键轮盘(src/ui/dial)用它, 以及两个主窗口
 * 与轮盘共用的辅助件(总开关、代理窗口的输入转发)。
 *
 * 为什么需要这个文件: 代理分层呈现原先只挂在 BongoCatPlatform::presenter 上, 而轮盘是
 * 自带模态循环的独立窗口 (dial_window.c), 根本没有 BongoCatPlatform —— 于是主窗口透明
 * 了、轮盘背景还是恒定的黑。核心呈现器已经改成按窗口持有(见 windows_layered.c 与
 * windows_layered_internal.h), 这里只做三件事:
 *   1) 给一个 SDL 窗口建/放它自己的呈现器实例, 并顺手打开代理模式;
 *   2) 收拢两个"跨窗口共用"的东西: 环境变量总开关、代理窗口的鼠标消息转发;
 *   3) 处理轮盘特有的差异 —— 它先画一帧再 ShowWindow, 隐藏窗口不算呈现失败。
 * Vulkan 渲染与开关关闭时本文件完全不参与, 轮盘照旧走 SDL_GL_SwapWindow。
 */

/*
 * 是否启用代理分层呈现 (GL 模式下的逐像素透明)。
 *
 * Windows 上普通窗口的 GL 缓冲 alpha 会被 DWM 忽略, 所以 UI 透明的唯一办法就是那条
 * 代理路径: 把真实窗口压成全透明, 再用一个 WS_EX_LAYERED 代理窗口以 ULW_ALPHA 显示
 * 抓到的帧。
 *
 * 三个输入, 优先级从高到低:
 *   1) BONGO_CAT_GL_TRANSPARENCY: 调试用的强制覆盖, "1" 强制开、"0" 强制关。
 *      用户可感知的设置项是下面那个, 环境变量只留给排查问题的人;
 *   2) configured: 设置界面的[透明背景] (window.glTransparency), 默认关闭 ——
 *      用户在 OBS 里按标题采集"真实窗口"时, 一开透明画面就卡住(见 windows_pointer.c
 *      的长注释), 所以不做成默认行为;
 *   3) 渲染后端: 这套呈现抓的是 GL 前缓冲 (windows_layered_frame.c 的 glReadPixels),
 *      Vulkan 下没有可读的前缓冲, 强开只会让代理窗口显示一块无效内存合成的黑块,
 *      所以非 OpenGL 后端一律返回 false。
 *
 * 主窗口与轮盘共用这一个判断, 否则会出现"猫透明了、菜单还是黑底"的半开状态。
 */
bool bongo_cat_windows_layered_requested(bool configured) {
    if (bongo_cat_renderer_active() != BONGO_CAT_RENDERER_OPENGL) return false;
    const char *value = SDL_getenv("BONGO_CAT_GL_TRANSPARENCY");
    if (value && value[1] == '\0') {
        if (value[0] == '1') return true;
        if (value[0] == '0') return false;
    }
    return configured;
}

/*
 * 代理窗口的输入转发。
 *
 * 代理模式下真实窗口被压成全透明 + WS_EX_TRANSPARENT (见 windows_layered.c 的
 * make_source_transparent), 于是它收不到任何鼠标消息 —— 而轮盘菜单靠的正是窗口消息,
 * 主窗口的滚轮同样如此(拖动已经改成全局轮询, 见 window_drag.c, 不再依赖这里)。
 *
 * 代理窗口是自己注册的类 (BongoCat.LayeredPresenter), 所以给它子类化一个窗口过程,
 * 把鼠标消息用 PostMessage 转投给真实窗口: SDL 照常产出鼠标事件, 上层一行都不用改。
 * 用 PostMessage 而不是 SendMessage, 免得在窗口过程中重入。
 *
 * 两条必须同时成立的细节, 少一条转发就"到位但没反应":
 *   - 代理窗口创建时带 WS_EX_TRANSPARENT(历史遗留, 见 ensure_proxy), 那会让命中测试
 *     直接跳到桌面, 点击根本落不到代理窗口上 —— 这里必须摘掉, 把"要不要穿透"交还给
 *     应用自己的逐像素命中测试;
 *   - 点击落在代理窗口上时 SDL 的真实窗口并不会被激活, 而 SDL 默认丢弃"未聚焦窗口"的
 *     鼠标按下, 所以要放开 SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH。
 * 窗口过程与两个属性都挂在各自的 HWND 上, 所以主窗口与轮盘可以各有一份, 互不干扰。
 */
static const wchar_t proxy_source_property[] = L"BongoCat.LayeredSource";
static const wchar_t proxy_input_proc_property[] = L"BongoCat.LayeredInputProc";
/* 上一次已经转投过的鼠标位置 (打包成属性值, NULL = 还没有转过)。打包时两个坐标都 +1,
   这样 (0,0) 打包出来也不是 NULL —— 属性值 NULL 与"没转过"没法区分。 */
static const wchar_t proxy_last_move_property[] = L"BongoCat.LayeredLastMove";

static LRESULT CALLBACK layered_proxy_input_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        HWND source = (HWND)GetPropW(window, proxy_source_property);
        if (source) {
            /*
             * 位置和上一次转投的一模一样就丢掉 —— 系统会把同一个 WM_MOUSEMOVE 反复
             * 重发: 分层窗口每次提交(UpdateLayeredWindow)都会让系统重新判定光标下的
             * 窗口, 于是又投一条。实测 976 条里只有 4 个不同坐标。
             *
             * 原样转投的代价全在 SDL 侧: 每条都变成一条"位置没变"的 MOUSE_MOTION,
             * 轮盘会拿它重新选中光标下那一项 —— 键盘刚用方向键选好的项立刻被拉回去
             * (实测按三次右键选中项一直停在 0, 而不是 0->1->2), 主窗口那边则是白跑
             * 命中测试。真实移动一定改变坐标, 不会被误丢; 光标移开再回到同一像素也
             * 不会丢, 因为中间那条已经把记账刷新了。
             */
            LONG packed = (LONG)((((LONG)LOWORD(lparam) + 1) << 16) |
                ((LONG)HIWORD(lparam) + 1));
            if ((LONG)(LONG_PTR)GetPropW(window, proxy_last_move_property) == packed)
                return 0;
            SetPropW(window, proxy_last_move_property, (HANDLE)(LONG_PTR)packed);
            PostMessageW(source, message, wparam, lparam);
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        HWND source = (HWND)GetPropW(window, proxy_source_property);
        if (source) {
            /* 代理窗口与真实窗口矩形一致, 客户区坐标可以直接转投; 滚轮消息本来就是
               屏幕坐标, 同样不需要换算。 */
            PostMessageW(source, message, wparam, lparam);
            return 0;
        }
        break;
    }
    default: break;
    }
    WNDPROC previous = (WNDPROC)GetPropW(window, proxy_input_proc_property);
    return CallWindowProcW(previous ? previous : DefWindowProcW, window, message,
        wparam, lparam);
}

/* 幂等: 代理窗口只在启用代理时创建, 装过一次就不再装 (属性就是标记)。 */
void bongo_cat_windows_layered_forward_input(HWND source) {
    HWND proxy = bongo_cat_windows_layered_proxy(source);
    if (!source || !proxy) return;
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    LONG_PTR proxy_ex_style = GetWindowLongPtrW(proxy, GWL_EXSTYLE);
    if (proxy_ex_style & WS_EX_TRANSPARENT)
        SetWindowLongPtrW(proxy, GWL_EXSTYLE,
            proxy_ex_style & ~WS_EX_TRANSPARENT);
    if (GetPropW(proxy, proxy_input_proc_property)) return;
    WNDPROC previous = (WNDPROC)GetWindowLongPtrW(proxy, GWLP_WNDPROC);
    if (!previous) return;
    SetPropW(proxy, proxy_source_property, (HANDLE)source);
    SetPropW(proxy, proxy_input_proc_property, (HANDLE)previous);
    if (!SetWindowLongPtrW(proxy, GWLP_WNDPROC, (LONG_PTR)layered_proxy_input_proc)) {
        RemovePropW(proxy, proxy_source_property);
        RemovePropW(proxy, proxy_input_proc_property);
    }
}

/*
 * 给轮盘这类独立窗口挂上它自己的呈现器。
 *
 * 三处顺序/归属上的讲究, 每一条都是实测定下来的:
 *   1) proxy_owner 必须在 activate 之前设好 —— 代理窗口只在创建那一刻认 owner。
 *   2) owner 选"主窗口(菜单宿主)的代理窗口", 没有才退回宿主的真实窗口。这是两个顶层
 *      代理窗口之间唯一的硬约束手段: owned window 永远在自己 owner 之上, 而主窗口那边
 *      的置前(BringWindowToTop/SetWindowPos)随时会把主代理抬起来。刚开始只在自己的代理
 *      上做 SetWindowPos(HWND_TOP), 结果是"菜单画得好好的, 鼠标却打在猫身上": 主代理
 *      一旦被抬起就把菜单代理整块盖住, 菜单收不到点击 → 真实窗口失焦 → 菜单按设计关闭。
 *   3) topmost 也要提前写进实例(代理窗口的 WS_EX_TOPMOST 只在创建时生效); 轮盘窗口本身
 *      是 SDL_WINDOW_ALWAYS_ON_TOP, 代理不置顶会被别人的窗口盖住。
 * visible 提前置真: 它表示"应用希望这个窗口可见", 与窗口此刻是否真的显示(创建时是
 * SDL_WINDOW_HIDDEN)是两件事, 后者由 present 里的 IsWindowVisible 判断。
 */
void *bongo_cat_windows_layered_window_attach(SDL_Window *window, SDL_Window *owner,
    bool topmost) {
    if (!window) return NULL;
    BongoCatWindowsLayered *value = bongo_cat_windows_layered_create(window);
    if (!value) return NULL;
    HWND owner_window = bongo_cat_windows_layered_window_handle(owner);
    if (owner_window) {
        HWND owner_proxy = bongo_cat_windows_layered_proxy(owner_window);
        value->proxy_owner = owner_proxy ? owner_proxy : owner_window;
    }
    value->topmost = topmost;
    value->visible = true;
    /* 菜单是模态的: 整块矩形都要吃点击, 否则点在扇区外会穿到猫身上(见 solidify_hit_area)。 */
    value->solid_hit_area = true;
    bongo_cat_windows_layered_set_click_through(value, true);
    bongo_cat_windows_layered_forward_input(
        bongo_cat_windows_layered_native_window(value));
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[probe] 轮盘呈现路径: 代理分层=%d", value->proxy != NULL);
    return value;
}

/*
 * 轮盘的每帧呈现入口。与主窗口不同的只有一处: 窗口还没显示时直接返回 true。
 * 轮盘是"先画一帧 -> 再 SDL_ShowWindow"的顺序, 而隐藏窗口没有可暴露的帧(核心的
 * present 会返回 false 并保持代理隐藏)。把这种情况上报成失败, 调用方(dial_window.c
 * 的 create)会把整个菜单判为打不开 —— 那是错的。
 */
bool bongo_cat_windows_layered_window_present(void *presenter, int width, int height) {
    BongoCatWindowsLayered *value = presenter;
    if (!value) return true;
    HWND source = bongo_cat_windows_layered_native_window(value);
    if (!source || !IsWindowVisible(source) || IsIconic(source)) return true;
    return bongo_cat_windows_layered_present(value, width, height);
}

/*
 * 把轮盘的代理窗口抬到最前。
 *
 * 这里只能传 HWND_TOP, 不能传 HWND_TOPMOST —— 这是个实测踩出来的坑: 代理窗口创建时
 * 就带了 WS_EX_TOPMOST, 而 SetWindowPos(HWND_TOPMOST) 对"已经是顶层"的窗口只在
 * "是否置顶"上有意义, 不改变它在顶层窗口带内的次序。HWND_TOP 才是"放到最前", 顶层
 * 样式保持不变。真正保证它不被主窗口代理压住的是 owner 链(见 attach)。
 */
void bongo_cat_windows_layered_window_raise(void *presenter) {
    BongoCatWindowsLayered *value = presenter;
    if (!value || !value->proxy) return;
    SetWindowPos(value->proxy, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
#endif
