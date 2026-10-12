/*
 * --ci-menu 的托盘自测: 走一遍真实菜单回调, 每项点两次 (验证改回去也能恢复), 最后
 * 检查状态与进入时一致; Windows 上还会先模拟一次 Windows shell 重启。
 *
 * 单独一个文件是为了守住单文件 500 行 (cmake/CheckLines.cmake), 也因为它只在冒烟
 * 运行里被调用 —— tray.c 的主路径 (建/销毁/同步) 与它无关。
 *
 * 一条容易漏的纪律: 每一项点完之后必须让"进入自测时的状态"成立, 包括**渲染后端**。
 * 那一项的回调只写配置、运行中不切后端, 所以这里点一次之后要手动把配置改回去,
 * 否则冒烟运行会留下一个用户从没选过的后端设置。
 */
#include "tray_internal.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

bool bongo_cat_tray_self_test(BongoCatTray *tray) {
    if (!tray || !tray->handle || !tray->visible || !tray->pass_through ||
        !tray->always_on_top || !tray->preferences || !tray->renderer ||
        !tray->exit) return false;
    SDL_Log("Tray self-test: entries ready");
    int count = 0;
    const SDL_TrayEntry **entries = SDL_GetTrayEntries(
        SDL_GetTrayEntryParent(tray->preferences), &count);
    if (!entries || count < 1 || entries[0] != tray->preferences) return false;
#ifdef _WIN32
    bongo_cat_tray_on_restore(tray);
    bongo_cat_tray_sync(tray);
    if (tray->restore_requested || !tray->handle || !tray->preferences ||
        !tray->renderer || !tray->visible || !tray->pass_through ||
        !tray->always_on_top || !tray->exit) return false;
    SDL_Log("Tray self-test: shell restart restored");
#endif
    BongoCatApp *app = tray->app;
    bool visible = app->session.window.visible;
    bool pass_through = app->settings.window.pass_through;
    bool always_on_top = app->settings.window.always_on_top;
    BongoCatRenderer renderer = app->settings.model.renderer;
    bongo_cat_tray_on_visible(tray, tray->visible);
    bongo_cat_tray_on_visible(tray, tray->visible);
    bongo_cat_tray_on_pass_through(tray, tray->pass_through);
    bongo_cat_tray_on_pass_through(tray, tray->pass_through);
    bongo_cat_tray_on_always_on_top(tray, tray->always_on_top);
    bongo_cat_tray_on_always_on_top(tray, tray->always_on_top);
    SDL_Log("Tray self-test: state actions restored");
    /*
     * 渲染后端那一项: 回调只写配置, 运行中不切后端。断言的是"配置变成了这一个窗口
     * 该切到的目标", 而不是"配置值变了" —— 配置本来就等于目标时 (例如命令行
     * --renderer=opengl 压着一个 renderer=vulkan 的配置) 后者永远是假的。
     * 点完必须自己恢复, 否则冒烟运行会留下一个用户从没选过的后端设置。
     */
    const BongoCatRenderer target = bongo_cat_tray_renderer_target(tray);
    bongo_cat_tray_on_renderer(tray, tray->renderer);
    bool renderer_switched = app->settings.model.renderer == target;
    app->settings.model.renderer = renderer;
    tray->state_valid = false;
    bongo_cat_tray_sync(tray);
    SDL_Log("Tray self-test: renderer entry applied=%d target=%s restored=%s",
        renderer_switched, bongo_cat_renderer_name(target),
        bongo_cat_renderer_name(renderer));
    bongo_cat_tray_on_preferences(tray, tray->preferences);
    bool preferences = bongo_cat_preferences_visible(app->preferences);
    SDL_Log("Tray self-test: preferences visible=%d", preferences);
    bongo_cat_preferences_close(app->preferences);
    SDL_Log("Tray self-test: preferences closed");
    bool result = preferences && bongo_cat_modal_frame_self_test() &&
        renderer_switched && app->session.window.visible == visible &&
        app->settings.window.pass_through == pass_through &&
        app->settings.window.always_on_top == always_on_top &&
        app->settings.model.renderer == renderer;
    /*
     * 失败时把每一项单独打出来 (只在这一条自测里, 不在逐帧路径上): 合成一个 result=0
     * 的布尔值对排查毫无帮助 —— 上一次就是靠这条分解才定位到"渲染后端那一项被拒"。
     */
    if (!result)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Tray self-test breakdown: preferences=%d modal=%d renderer_switched=%d "
            "visible=%d pass_through=%d always_on_top=%d renderer_restored=%d",
            preferences, bongo_cat_modal_frame_self_test(), renderer_switched,
            app->session.window.visible == visible,
            app->settings.window.pass_through == pass_through,
            app->settings.window.always_on_top == always_on_top,
            app->settings.model.renderer == renderer);
    SDL_Log("Tray self-test: result=%d", result);
    return result;
}
