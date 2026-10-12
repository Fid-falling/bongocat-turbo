/*
 * 托盘模块的内部接口 (tray.c / tray_renderer.c / tray_self_test.c 共享)。
 *
 * 为什么要拆: cmake/CheckLines.cmake 给每个文件 500 行 (非空行计), tray.c 在加入
 * "切换渲染后端"这一项之后顶到了上限。拆分的界线按"什么时候会被调用"划:
 *   - tray.c        建/销毁/同步托盘本体 (启动与 shell 重启时才走);
 *   - tray_renderer.c 渲染后端逃生项 (用户点菜单时才走);
 *   - tray_self_test.c --ci-menu 的自测 (只在冒烟运行里走)。
 *
 * BongoCatTray 的结构体定义放在这里而不是 tray.c, 是因为后两个文件都要读它的字段
 * (renderer 那一项、state_valid、restore_count)。它是本模块的私有类型, 不要放进
 * include/bongo_cat/tray.h —— 那是给上层用的不透明句柄。
 */
#ifndef BONGO_CAT_TRAY_INTERNAL_H
#define BONGO_CAT_TRAY_INTERNAL_H

#include "bongo_cat/app.h"
#include "bongo_cat/image.h"
#include "bongo_cat/tray.h"
#include "modal_frame.h"

#include <SDL3/SDL.h>

struct BongoCatTray {
    BongoCatApp *app;
    SDL_Tray *handle;
    SDL_TrayEntry *visible;
    SDL_TrayEntry *pass_through;
    SDL_TrayEntry *always_on_top;
    SDL_TrayEntry *preferences;
    /* "切换渲染后端" 逃生入口 (见 tray_renderer.c 的说明) */
    SDL_TrayEntry *renderer;
    /* 那一项的文案缓冲。SDL 建项/改标签时会拷走字符串, 但缓冲要活到那时 (见
       bongo_cat_tray_renderer_label)。 */
    char renderer_label[192];
    SDL_TrayEntry *exit;
    BongoCatImage icon;
    BongoCatModalFrame modal_frame;
    bool state_valid, restore_requested;
    uint64_t restore_after_ns;
    unsigned restore_count;
    bool last_visible, last_pass_through, last_always_on_top;
    /* 脏检查用的"上次贴上去的后端" (文案里写着"当前 -> 目标", 不跟着变就会说谎) */
    int last_renderer;
    BongoCatLanguage last_language;
};

/*
 * 菜单动作。托盘菜单本身不存状态: 真相始终在 app->session / app->settings 里,
 * 回调只负责改它, 再由 bongo_cat_tray_sync 把勾选态贴回去。
 *
 * 这几个不是 static: tray_self_test.c 要逐个真实调用它们 (签名与 SDL_TrayCallback
 * 一致, 所以能直接交给 SDL_SetTrayEntryCallback)。
 */
void bongo_cat_tray_on_visible(void *userdata, SDL_TrayEntry *entry);
void bongo_cat_tray_on_pass_through(void *userdata, SDL_TrayEntry *entry);
void bongo_cat_tray_on_always_on_top(void *userdata, SDL_TrayEntry *entry);
void bongo_cat_tray_on_preferences(void *userdata, SDL_TrayEntry *entry);
void bongo_cat_tray_on_exit(void *userdata, SDL_TrayEntry *entry);
/* Windows shell 重启回调: 只置标志, 由 tray_sync 负责销毁重建 */
void bongo_cat_tray_on_restore(void *userdata);

/* tray_renderer.c: 渲染后端逃生项 */
BongoCatRenderer bongo_cat_tray_renderer_target(const BongoCatTray *tray);
const char *bongo_cat_tray_renderer_label(BongoCatTray *tray);
void bongo_cat_tray_on_renderer(void *userdata, SDL_TrayEntry *entry);

#endif
