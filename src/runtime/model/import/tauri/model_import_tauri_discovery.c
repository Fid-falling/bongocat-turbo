/*
 * Tauri 源包的两条发现路径: exact 只看用户选中目录的直接子项 (选中即包根),
 * recursive 递归下钻 (选中的是装着多个包的容器目录)。
 * 两者都往调用方的 discovery 里追加候选, 并共用一条排除规则: 已经属于 Mver 包的
 * model3.json 不再当 Tauri 源 —— 否则同一个模型会以 Tauri 源的身份被再导入一次
 * (Mver 包里本来就放着 Live2D 模型和它的静态兜底模板)。
 * 返回值 1/0/负值 = 命中/未命中/出错, 与其它格式适配器一致; ambiguous 一旦置位,
 * 出错文案就换成“一个目录里有多份清单”, 候选构造失败的原因只能靠它区分。
 */
#include "model_import_tauri_internal.h"
#include "../model_import_manifest.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* Mver 规范包的输入图指纹: img/<mode>/<hand|lefthand|righthand>/0.png 存在即算。
 * 标准模式用 hand, 另两种模式是左右手两组 (与 Mver 侧的判据逐字一致)。 */
static bool mver_input_assets(const char *directory) {
    char path[BONGO_CAT_PATH_CAP], group[BONGO_CAT_PATH_CAP];
    static const char *const groups[] = {"hand", "lefthand", "righthand"};
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i)
        if (bongo_cat_path_join(group, sizeof(group), directory, groups[i]) &&
            bongo_cat_path_join(path, sizeof(path), group, "0.png") &&
            bongo_cat_path_is_file(path)) return true;
    return false;
}

/* 模式目录的判据: 名字必须是 standard/keyboard/gamepad 之一, 父目录叫 img, 且带输入图。 */
static bool mver_mode_directory(const char *directory) {
    const char *name = bongo_cat_path_name(directory);
    if (!name || (SDL_strcasecmp(name, "standard") != 0 &&
            SDL_strcasecmp(name, "keyboard") != 0 &&
            SDL_strcasecmp(name, "gamepad") != 0)) return false;
    char image_root[BONGO_CAT_PATH_CAP];
    return bongo_cat_import_parent_path(directory, image_root,
            sizeof(image_root)) &&
        SDL_strcasecmp(bongo_cat_path_name(image_root), "img") == 0 &&
        mver_input_assets(directory);
}

/* 这份 model3.json 是不是已经躺在某个 Mver 包里? 从它所在目录向上最多探 4 层
 * (与 Mver 的 find_package 同深度), 命中就说明它是包的组成部分, 不是 Tauri 源。 */
static bool inside_mver_model(const char *directory) {
    char current[BONGO_CAT_PATH_CAP];
    int written = snprintf(current, sizeof(current), "%s",
        directory ? directory : "");
    if (written < 0 || (size_t)written >= sizeof(current)) return false;
    for (int depth = 0; depth < 4; ++depth) {
        if (mver_mode_directory(current)) return true;
        char parent[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_import_parent_path(current, parent, sizeof(parent)))
            break;
        snprintf(current, sizeof(current), "%s", parent);
    }
    return false;
}

/* 枚举回调: 只认 .model3.json 文件, 且清单与它引用的资源都要通过校验才登记候选;
 * 返回 FAILURE 会让整次枚举失败 (调用方据此报错)。 */
static BongoCatPathVisit discover_model(void *userdata,
    const char *dirname, const char *name) {
    BongoCatImportDiscovery *discovery = userdata;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), dirname, name))
        return BONGO_CAT_PATH_FAILURE;
    if (!bongo_cat_path_is_file(path) ||
        !bongo_cat_import_has_suffix(name, ".model3.json"))
        return BONGO_CAT_PATH_CONTINUE;
    if (inside_mver_model(dirname)) return BONGO_CAT_PATH_CONTINUE;
    if (bongo_cat_import_manifest_valid(dirname, name, NULL) &&
        !bongo_cat_import_tauri_add_candidate(discovery, dirname, name))
        return BONGO_CAT_PATH_FAILURE;
    return BONGO_CAT_PATH_CONTINUE;
}

/* 递归下钻: 跳过隐藏项 (以 . 开头, 含 macOS 的 ._ 资源叉), 深度上限 8; 计数借
 * discovery->depth 传递, 因此枚举中途返回也是平衡的。 */
static BongoCatPathVisit discover_recursive_item(void *userdata,
    const char *dirname, const char *name) {
    BongoCatImportDiscovery *discovery = userdata;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), dirname, name))
        return BONGO_CAT_PATH_FAILURE;
    if (bongo_cat_path_is_file(path))
        return discover_model(userdata, dirname, name);
    if (!bongo_cat_path_is_dir(path) || discovery->depth >= 8 || name[0] == '.')
        return BONGO_CAT_PATH_CONTINUE;
    discovery->depth++;
    bool ok = bongo_cat_path_enumerate(path, discover_recursive_item,
        discovery);
    discovery->depth--;
    return ok ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
}

/* 精确发现: 只看直接子项, 所以选中的目录就是包根 —— 成功时把这一批候选的
 * package_root 统一改写成 source。返回 -1 表示枚举失败 (也可能只是清单歧义),
 * 只有 1 才代表真有候选, 0 不算失败。 */
int bongo_cat_import_tauri_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    if (!source || !discovery || !bongo_cat_path_is_dir(source)) return 0;
    if (!discovery->source_name[0]) snprintf(discovery->source_name,
        sizeof(discovery->source_name), "%s", bongo_cat_path_name(source));
    size_t before = discovery->count;
    if (!bongo_cat_path_enumerate(source, discover_model, discovery)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            discovery->ambiguous
                ? "A model directory contains multiple model3 manifests"
                : "Cannot inspect nearby model directory");
        return -1;
    }
    for (size_t i = before; i < discovery->count; ++i)
        snprintf(discovery->candidates[i].package_root,
            sizeof(discovery->candidates[i].package_root), "%s", source);
    return discovery->count > before ? 1 : 0;
}

/* 递归发现: 候选可能来自互不相干的子包, 所以每个候选保留自己的 package_root, 不在
 * 这里改写。返回语义与 exact 相同: 1 = 命中, 0 = 没找到, -1 = 扫描出错。 */

int bongo_cat_import_tauri_discover_recursive(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    if (!source || !discovery || !bongo_cat_path_is_dir(source)) return 0;
    if (!discovery->source_name[0]) snprintf(discovery->source_name,
        sizeof(discovery->source_name), "%s", bongo_cat_path_name(source));
    size_t before = discovery->count;
    if (!bongo_cat_path_enumerate(source, discover_recursive_item, discovery)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            discovery->ambiguous
                ? "A model directory contains multiple model3 manifests"
                : "Cannot scan model directory or it contains too many models");
        return -1;
    }
    return discovery->count > before ? 1 : 0;
}
