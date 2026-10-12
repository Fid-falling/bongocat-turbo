/*
 * 存储目录解析, 六棵树: config / data / models / cache / state / logs。
 *
 * 来源优先级:
 *   1. --storage-root= : 六棵树全部挂在它下面 (CI、绿色版、多实例测试);
 *   2. 平台默认: Windows 打包运行用包内存储, 否则 %LOCALAPPDATA%\BongoCat;
 *      macOS 用 ~/Library/{Application Support,Caches,Logs}; Linux 走 XDG_*;
 *   3. 全部失败就返回 false —— 没有降级方案, 因为配置/日志/模型都必须落盘。
 * 副宠进程会把 state_root 换成按模型 id 命名的子目录 (multi_pet), 免得两个进程抢
 * 同一份 session.json。
 * 每个目录都用"真的写一个探针文件"来判定可写, 而不是看权限位。
 */
#include "storage_paths.h"
#include "runtime.h"

#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "windows_package.h"
#endif

static bool child_path(char *target, size_t capacity, const char *root,
    const char *child) {
    return root && root[0] &&
        bongo_cat_path_join(target, capacity, root, child);
}

/* 探针文件判可写: 只读挂载、被安全软件拦住的目录在 Windows 上权限位看着都是正常的。 */
static bool writable_directory(const char *root) {
    char probe[BONGO_CAT_PATH_CAP];
    if (!root || !root[0] || !bongo_cat_path_create_directory(root) ||
        !bongo_cat_path_join(probe, sizeof(probe), root,
            ".bongocat-write-probe")) return false;
    FILE *file = bongo_cat_file_open(probe, "wb");
    bool written = file && fputc('1', file) != EOF;
    if (file && fclose(file) != 0) written = false;
    bongo_cat_file_remove(probe);
    return written;
}

/* 六棵树的路径必须全部拼成功才算解析成功; 任何一棵失败都返回 false, 不做部分降级。 */
static bool roots_under(BongoCatApp *app, const char *root) {
    return child_path(app->config_root, sizeof(app->config_root),
            root, "config") &&
        child_path(app->data_root, sizeof(app->data_root),
            root, "data") &&
        child_path(app->models_root, sizeof(app->models_root),
            root, "models") &&
        child_path(app->cache_root, sizeof(app->cache_root),
            root, "cache") &&
        child_path(app->state_root, sizeof(app->state_root),
            root, "state") &&
        child_path(app->log_root, sizeof(app->log_root),
            root, "logs");
}

static bool isolated_roots(BongoCatApp *app) {
    return roots_under(app, app->storage_root);
}

#ifdef _WIN32
static bool platform_roots(BongoCatApp *app) {
    char local_app[BONGO_CAT_PATH_CAP];
    if (bongo_cat_windows_is_packaged())
        return bongo_cat_windows_package_storage_root(local_app,
            sizeof(local_app)) && roots_under(app, local_app);
    const char *local = SDL_getenv("LOCALAPPDATA");
    return child_path(local_app, sizeof(local_app), local, BONGO_CAT_STORAGE_DIR) &&
        roots_under(app, local_app);
}
#elif defined(__APPLE__)
static bool platform_roots(BongoCatApp *app) {
    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    char library[BONGO_CAT_PATH_CAP], support[BONGO_CAT_PATH_CAP];
    char app_support[BONGO_CAT_PATH_CAP];
    return child_path(library, sizeof(library), home, "Library") &&
        child_path(support, sizeof(support), library, "Application Support") &&
        child_path(app_support, sizeof(app_support), support, BONGO_CAT_STORAGE_DIR) &&
        child_path(app->config_root, sizeof(app->config_root),
            app_support, "config") &&
        child_path(app->data_root, sizeof(app->data_root), app_support, "data") &&
        child_path(app->models_root, sizeof(app->models_root),
            app_support, "models") &&
        child_path(app->state_root, sizeof(app->state_root), app_support, "state") &&
        child_path(app->cache_root, sizeof(app->cache_root),
            library, "Caches/BongoCat") &&
        child_path(app->log_root, sizeof(app->log_root),
            library, "Logs/BongoCat");
}
#else
static bool xdg_root(char *target, size_t capacity, const char *variable,
    const char *home_suffix) {
    const char *configured = SDL_getenv(variable);
    if (configured && configured[0])
        return child_path(target, capacity, configured, "bongocat");
    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    char fallback[BONGO_CAT_PATH_CAP];
    return child_path(fallback, sizeof(fallback), home, home_suffix) &&
        child_path(target, capacity, fallback, "bongocat");
}

static bool platform_roots(BongoCatApp *app) {
    if (!xdg_root(app->config_root, sizeof(app->config_root),
            "XDG_CONFIG_HOME", ".config") ||
        !xdg_root(app->data_root, sizeof(app->data_root),
            "XDG_DATA_HOME", ".local/share") ||
        !xdg_root(app->state_root, sizeof(app->state_root),
            "XDG_STATE_HOME", ".local/state") ||
        !xdg_root(app->cache_root, sizeof(app->cache_root),
            "XDG_CACHE_HOME", ".cache")) return false;
    if (!child_path(app->models_root, sizeof(app->models_root),
        app->data_root, "models")) return false;
    return child_path(app->log_root, sizeof(app->log_root),
        app->state_root, "logs");
}
#endif

/* 失败时把 settings_path/session_path 清空: 调用方据此判断"配置不可用", 而空路径会让
   config_store 直接跳过读写, 而不是往相对路径里写出一份野生配置。 */
bool bongo_cat_storage_paths_prepare(BongoCatApp *app,
    BongoCatError *error) {
    if (!app) return false;
    bool resolved = app->storage_root[0]
        ? isolated_roots(app) : platform_roots(app);
    if (resolved && app->cache_root_override[0]) {
        snprintf(app->cache_root, sizeof(app->cache_root),
            "%s", app->cache_root_override);
    }
    if (resolved) {
        snprintf(app->primary_state_root, sizeof(app->primary_state_root),
            "%s", app->state_root);
    }
    if (resolved && app->secondary_pet) {
        resolved = bongo_cat_multi_pet_state_directory(app->state_root,
            sizeof(app->state_root), app->primary_state_root,
            app->secondary_model_id);
    }
    const char *roots[] = {app->config_root, app->data_root, app->models_root,
        app->cache_root, app->state_root, app->log_root};
    for (size_t i = 0; resolved && i < sizeof(roots) / sizeof(roots[0]); ++i)
        resolved = writable_directory(roots[i]);
    resolved = resolved && child_path(app->settings_path,
        sizeof(app->settings_path), app->config_root, "settings.json") &&
        child_path(app->session_path, sizeof(app->session_path),
            app->state_root, "session.json");
    if (resolved) return true;
    app->settings_path[0] = app->session_path[0] = '\0';
    bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Application storage directories are unavailable or not writable");
    return false;
}

/*
 * 缓存目录里也要有一份 Live2D 的 Vulkan 着色器。
 *
 * 单独复制 exe 的用法下 (用户把 exe 拷到桌面), 桥是从缓存里解出来的资源目录找
 * FrameworkShaders 的。用户指定缓存目录时, 这里保证那份资源确实被解出来 ——
 * 已有一份就直接返回 (内嵌资源的 complete 标记会拦住重复解包)。
 */
static void ensure_cache_shaders(BongoCatApp *app) {
    if (!app || !app->cache_root[0]) return;
    char probe[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(probe, sizeof(probe), app->cache_root,
            "embedded-assets-" BONGO_CAT_VERSION
            "/assets/FrameworkShaders/FragShaderSrc.spv")) return;
    uint64_t size = 0;
    if (bongo_cat_path_file_size(probe, &size) && size) return;
    char target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(target, sizeof(target), app->cache_root,
            "embedded-assets-" BONGO_CAT_VERSION)) return;
    BongoCatError embedded = {0};
    if (bongo_cat_platform_embedded_assets(target, &embedded) == BONGO_CAT_OK) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 缓存目录已解出内嵌资源 (含 FrameworkShaders 着色器): %s",
            target);
    } else if (embedded.message[0]) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", embedded.message);
    }
}

/* 只有显式配置了缓存目录才会重设。换目录失败要回退到默认目录并填 error, 但这里不决定
   是否致命 —— 调用方 (app.c) 只在命令行给了 --cache-root= 时才当失败处理。 */
bool bongo_cat_storage_paths_apply_cache(BongoCatApp *app,
    BongoCatError *error) {
    if (!app) return true;
    /* A command line --cache-root takes precedence over the settings value. */
    const char *configured = app->cache_root_override[0]
        ? app->cache_root_override : app->settings.cache_root;
    if (!configured || !configured[0]) return true;
    char default_root[BONGO_CAT_PATH_CAP];
    snprintf(default_root, sizeof(default_root), "%s", app->cache_root);
    snprintf(app->cache_root, sizeof(app->cache_root), "%s", configured);
    if (!writable_directory(app->cache_root)) {
        snprintf(app->cache_root, sizeof(app->cache_root), "%s", default_root);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Configured cache directory is unavailable or not writable: %s",
            configured);
        return false;
    }
    ensure_cache_shaders(app);
    return true;
}
