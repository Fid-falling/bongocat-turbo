/*
 * 语言切换。整个过程是"要么全部成功, 要么完全回滚"的事务:
 * 1. i18n 重载失败 -> 把设置里的语言改回去 (用户看到的下拉不能停在一个没生效的值上);
 * 2. 字体图集重建失败 -> 把 i18n 也切回旧语言。这一条尤其重要: 新语言的字形没进
 * 图集, 界面会整片变成缺字方块, 比"没切成"糟糕得多;
 * 3. 成功后要连带刷新窗口标题和托盘菜单 —— 这两处的文字不归 Nuklear 管, 不主动
 * 同步就会一直显示旧语言。
 * 语言切换必然伴随 paint cache 作废 (文字宽度全变了), 所以这里直接 paint_destroy。
 * font_language 是"当前图集是按哪种语言建的", 用它做幂等判断, 避免每帧重载。
 */
#include "preferences_state.h"
#include "ui_paint.h"
#include "bongo_cat/tray.h"

#include <SDL3/SDL.h>

bool bongo_cat_preferences_reload_language(BongoCatPreferences *value) {
    if (!value || value->font_language == value->app->settings.app.language)
        return false;
    BongoCatLanguage previous = value->font_language;
    BongoCatLanguage requested = value->app->settings.app.language;
    BongoCatError error = {0};
    if (!value->app->i18n || bongo_cat_i18n_reload(value->app->i18n,
        requested, &error) != BONGO_CAT_OK) {
        value->app->settings.app.language = previous;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
        return false;
    }
    value->font_language = requested;
    if (!bongo_cat_preferences_reload_fonts(value)) {
        BongoCatError restore = {0};
        value->app->settings.app.language = previous;
        value->font_language = previous;
        bongo_cat_i18n_reload(value->app->i18n, previous, &restore);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preferences font resources could not be rebuilt");
        return false;
    }
    bongo_cat_ui_paint_destroy(&value->ui);
    if (value->window) SDL_SetWindowTitle(value->window,
        bongo_cat_i18n_get(value->app->i18n,
            "native.preferencesWindowTitle", "BongoCat - Settings"));
    if (value->app->tray) bongo_cat_tray_sync(value->app->tray);
    value->render_dirty = true;
    return true;
}
