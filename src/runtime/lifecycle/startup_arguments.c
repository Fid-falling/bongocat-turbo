/*
 * 命令行解析, 分两类:
 *   - 用户/系统参数: --autostart, --storage-root=, --cache-root=, --nearby-root=,
 *     --secondary-pet= / --secondary-position= (多宠子进程由主进程拉起);
 *   - --ci-* 冒烟参数: 只影响这一次运行, 用来无人值守地驱动设置窗口、导入模型、跑
 *     自测; 它们不写回配置 (config_store 在 smoke 下不落盘)。
 * 不认识的参数静默忽略 —— Windows 会往命令行里塞一些系统注入的参数。
 */
#include "runtime.h"
#include "bongo_cat/i18n.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows_package.h"
#endif

static bool store_argument(char *target, size_t capacity, const char *value,
    const char *name, BongoCatError *error) {
    int length = snprintf(target, capacity, "%s", value ? value : "");
    if (length >= 0 && (size_t)length < capacity) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
        "%s path is too long", name);
    return false;
}

/* nearby_root 只是"额外多扫一个目录"的来源, 取不到就不设 (不算失败); 真正由
   --nearby-root= 给的路径太长才报错。 */
static bool prepare_nearby_root(BongoCatApp *app, BongoCatError *error) {
#ifdef _WIN32
    /* A packaged full-trust app is commonly launched with System32 as its
       current directory.  That is not a user model directory, and scanning
       it can walk protected Windows management folders (for example
       appmgmt\MACHINE), causing startup to fail or consume the scan budget.
       Nearby model discovery remains available through an explicit
       --nearby-root argument and for unpackaged/portable launches. */
    char *current = NULL;
    const char *root = NULL;
    if (!bongo_cat_windows_is_packaged()) {
        current = SDL_GetCurrentDirectory();
        root = current && current[0] ? current : SDL_GetBasePath();
    }
#else
    const char *root = SDL_GetBasePath();
#endif
    bool stored = !root || store_argument(app->nearby_root,
        sizeof(app->nearby_root), root, "Nearby model", error);
#ifdef _WIN32
    SDL_free(current);
#endif
    return stored;
}

/* 先算 nearby_root (它要用工作目录/程序目录, 跟命令行无关), 再逐条解析: 带路径的参数
   写不下时直接失败 —— 被静默截断的路径比启动失败难查得多。 */
bool bongo_cat_startup_arguments(BongoCatApp *app, int argc, char **argv,
    BongoCatError *error) {
    if (!prepare_nearby_root(app, error)) return false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--autostart") == 0) app->autostart_launch = true;
        else if (strcmp(arg, "--ci-smoke") == 0) app->smoke = true;
        else if (strcmp(arg, "--ci-preferences") == 0) app->smoke_preferences = true;
        else if (strcmp(arg, "--ci-preference-shortcut") == 0)
            app->smoke_preference_shortcut = true;
        else if (strcmp(arg, "--ci-preference-model-select") == 0)
            app->smoke_preference_model_select = true;
        else if (strcmp(arg, "--ci-remove-imported") == 0) app->smoke_remove_imported = true;
        else if (strcmp(arg, "--ci-shortcuts") == 0) app->smoke_shortcuts = true;
        else if (strcmp(arg, "--ci-menu") == 0) app->smoke_menu = true;
        else if (strcmp(arg, "--ci-ignore-global-input") == 0) app->smoke_ignore_global_input = true;
        else if (strcmp(arg, "--ci-pass-through") == 0) app->smoke_pass_through = true;
        else if (strcmp(arg, "--ci-context-menu") == 0) app->smoke_context_menu = true;
        else if (strcmp(arg, "--ci-frame-series") == 0) app->smoke_frame_series = true;
        else if (strcmp(arg, "--ci-runtime-flow") == 0) app->smoke_runtime_flow = true;
        else if (strcmp(arg, "--ci-freeze-model") == 0) app->smoke_freeze_model = true;
        else if (strncmp(arg, "--ci-preference-page=", 21) == 0) {
            int page = atoi(arg + 21);
            if (page >= 0 && page < 4) app->smoke_preference_page = page;
        } else if (strncmp(arg, "--ci-language=", 14) == 0) {
            BongoCatLanguage language;
            if (bongo_cat_language_parse(arg + 14, &language))
                app->smoke_language = language;
        } else if (strncmp(arg, "--ci-theme=", 11) == 0) {
            const char *name = arg + 11;
            for (int value = 0; value <= BONGO_CAT_THEME_DARK; ++value)
                if (strcmp(name, bongo_cat_theme_name((BongoCatTheme)value)) == 0)
                    app->smoke_theme = value;
        } else if (strncmp(arg, "--ci-exit-ms=", 13) == 0) {
            uint64_t delay = strtoull(arg + 13, NULL, 10);
            app->smoke_deadline_ns = delay > UINT64_MAX / 1000000ull
                ? UINT64_MAX : delay * 1000000ull;
        } else if (strncmp(arg, "--storage-root=", 15) == 0) {
            if (!store_argument(app->storage_root, sizeof(app->storage_root),
                arg + 15, "Storage", error)) return false;
        } else if (strncmp(arg, "--cache-root=", 13) == 0) {
            if (!store_argument(app->cache_root_override,
                sizeof(app->cache_root_override), arg + 13,
                "Cache", error)) return false;
        } else if (strncmp(arg, "--nearby-root=", 14) == 0) {
            if (!store_argument(app->nearby_root, sizeof(app->nearby_root),
                arg + 14, "Nearby model", error)) return false;
        } else if (strncmp(arg, "--secondary-pet=", 16) == 0) {
            if (!store_argument(app->secondary_model_id,
                sizeof(app->secondary_model_id), arg + 16,
                "Secondary model", error)) return false;
            app->secondary_pet = app->secondary_model_id[0] != '\0';
        } else if (strncmp(arg, "--secondary-position=", 21) == 0) {
            char *separator = NULL, *end = NULL;
            long x = strtol(arg + 21, &separator, 10);
            long y = separator && *separator == ',' ?
                strtol(separator + 1, &end, 10) : 0;
            app->secondary_origin_known = separator != arg + 21 && end &&
                end != separator + 1 && !*end && x >= INT_MIN && x <= INT_MAX &&
                y >= INT_MIN && y <= INT_MAX;
            if (app->secondary_origin_known) {
                app->secondary_origin_x = (int)x;
                app->secondary_origin_y = (int)y;
            }
        } else if (strncmp(arg, "--ci-import=", 12) == 0) {
            if (!store_argument(app->smoke_import_path, sizeof(app->smoke_import_path),
                arg + 12, "Import", error)) return false;
        } else if (strncmp(arg, "--ci-model=", 11) == 0) {
            if (!store_argument(app->smoke_model, sizeof(app->smoke_model),
                arg + 11, "Model", error)) return false;
        } else if (strncmp(arg, "--ci-viewer-trace=", 18) == 0) {
            if (!store_argument(app->smoke_viewer_trace,
                sizeof(app->smoke_viewer_trace), arg + 18,
                "Viewer trace", error)) return false;
        } else if (strncmp(arg, "--ci-live2d-scenario=", 21) == 0 &&
            !store_argument(app->smoke_live2d_scenario,
                sizeof(app->smoke_live2d_scenario), arg + 21,
                "Scenario", error)) return false;
    }
/* 没显式给 --ci-exit-ms= 的冒烟运行统一 1.5s 后自动退出, 免得 CI 挂住。 */
    if (app->smoke && !app->smoke_deadline_ns)
        app->smoke_deadline_ns = 1500000000ull;
    return true;
}
