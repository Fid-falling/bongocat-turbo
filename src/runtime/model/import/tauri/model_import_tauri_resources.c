/* 源包资源查找。三个根按固定顺序试: assets (预览/按键资源所在的层, 可能是父目录)、
 * directory (model3.json 所在目录)、package_root (用户选中的包根); 每个根先试
 * <root>/resources/<name> 再试 <root>/<name>, 因为 Tauri 两种布局都存在。 */
#include "model_import_tauri_internal.h"

#include "bongo_cat/path.h"


/* 命中时返回 true, 且 path 一定是存在的文件; 未命中时 path 的内容是未定义的 (查找过程
 * 会反复覆盖它), 调用方只能看返回值。 */
bool bongo_cat_tauri_find_resource_file(
    const BongoCatImportCandidate *candidate, const char *name,
    char *path, size_t capacity) {
    if (!candidate || !name || !path || !capacity) return false;
    const char *roots[] = {candidate->assets, candidate->directory,
        candidate->package_root};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (!roots[i][0]) continue;
        char resources[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(resources, sizeof(resources), roots[i],
                "resources") && bongo_cat_path_join(path, capacity,
                resources, name) && bongo_cat_path_is_file(path)) return true;
        if (bongo_cat_path_join(path, capacity, roots[i], name) &&
            bongo_cat_path_is_file(path)) return true;
    }
    return false;
}

/* 目录版查找, 用于 left-keys/right-keys 这些按键目录。 */
bool bongo_cat_tauri_find_resource_directory(
    const BongoCatImportCandidate *candidate, const char *name,
    char *path, size_t capacity) {
    if (!candidate || !name || !path || !capacity) return false;
    const char *roots[] = {candidate->assets, candidate->directory,
        candidate->package_root};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (!roots[i][0]) continue;
        char resources[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(resources, sizeof(resources), roots[i],
                "resources") && bongo_cat_path_join(path, capacity,
                resources, name) && bongo_cat_path_is_dir(path)) return true;
        if (bongo_cat_path_join(path, capacity, roots[i], name) &&
            bongo_cat_path_is_dir(path)) return true;
    }
    return false;
}

/* 包级文件查找 (bongocat.mver.json): 根的顺序与上面相反, directory 优先, 而且刻意不探
 * resources/ —— 包清单只可能和模型目录同级。 */

bool bongo_cat_tauri_find_package_file(
    const BongoCatImportCandidate *candidate, const char *name,
    char *path, size_t capacity) {
    if (!candidate || !name || !path || !capacity) return false;
    const char *roots[] = {candidate->directory, candidate->assets,
        candidate->package_root};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i)
        if (roots[i][0] && bongo_cat_path_join(path, capacity, roots[i], name) &&
            bongo_cat_path_is_file(path)) return true;
    return false;
}
