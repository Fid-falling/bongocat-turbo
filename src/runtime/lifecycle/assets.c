/* 资源目录定位。查找顺序: 程序目录旁边的 assets/ -> (缺资源时) 把内嵌资源解到
   cache/embedded-assets-<version>/assets -> Debug 构建才回退到开发目录。
   必须整套关键文件都在才算找到 —— 半套资源目录比"找不到"更难查。 */
#include "runtime.h"
#include "bongo_cat/path.h"
#include "bongo_cat/platform.h"

#include <stdio.h>

/* 最小必需集合: 图标 / UI 符号 / 标准 Live2D 模型与纹理 / 至少一份 locale。
   判据是"文件存在且非空", 因为打包最常出的错是零字节文件。 */
static bool required_assets(const char *root) {
    if (!root || !root[0]) return false;
    const char *files[] = {
        "bongocat.png", "ui-symbols.png", "ui-symbols@4x.png",
        "catime.png", "vlaina.jpg", "locales/en-US.json",
        "models/standard/cat.model3.json",
        "models/standard/demomodel.moc3",
        "models/standard/demomodel.1024/texture_00.png"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        uint64_t size = 0;
        if (!bongo_cat_path_join(path, sizeof(path), root, files[i]) ||
            !bongo_cat_path_file_size(path, &size) || !size) return false;
    }
    return true;
}

/* 拼出 root/assets 并确认整套资源在; 任何一步失败都返回 false, 由调用方决定再去哪找。 */
static bool asset_directory(char *output, size_t capacity, const char *root) {
    return bongo_cat_path_join(output, capacity, root, "assets") &&
        bongo_cat_path_is_dir(output) && required_assets(output);
}

/* 定位失败时 asset_root 保持空串, 由调用方 (app.c) 报错; 这里只负责填 asset_root 与
   locale_root 两棵树。 */
BongoCatResult bongo_cat_app_locate_assets(BongoCatApp *app,
    BongoCatError *error) {
    app->asset_root[0] = '\0'; app->locale_root[0] = '\0';
    const char *base = SDL_GetBasePath();
    if (!asset_directory(app->asset_root, sizeof(app->asset_root), base ? base : "")) {
        char cache[BONGO_CAT_PATH_CAP], name[64];
        snprintf(name, sizeof(name), "embedded-assets-%s", BONGO_CAT_VERSION);
        if (bongo_cat_path_join(cache, sizeof(cache), app->cache_root, name)) {
            BongoCatError embedded = {0};
            if (bongo_cat_platform_embedded_assets(cache, &embedded) == BONGO_CAT_OK) {
                asset_directory(app->asset_root, sizeof(app->asset_root), cache);
            } else if (embedded.message[0]) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", embedded.message);
            }
        }
    }
/* 只有 Debug 构建才回退开发目录: Release 包必须自带资源, 否则会静默用到开发者的路径。 */
#ifndef NDEBUG
    if (!required_assets(app->asset_root) && required_assets(BONGO_CAT_DEV_ASSET_ROOT))
        snprintf(app->asset_root, sizeof(app->asset_root), "%s", BONGO_CAT_DEV_ASSET_ROOT);
#endif
    if (!required_assets(app->asset_root)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Required application assets are missing or incomplete");
        return BONGO_CAT_ERROR_IO;
    }
    if (!bongo_cat_path_join(app->locale_root, sizeof(app->locale_root),
        app->asset_root, "locales") || !bongo_cat_path_is_dir(app->locale_root)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Application locale assets are missing");
        return BONGO_CAT_ERROR_IO;
    }
    return BONGO_CAT_OK;
}
