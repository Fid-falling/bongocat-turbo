/*
 * Mver 图片补丁的发现器: 补丁只替换 img 下的按键与手势图片, 不含 Live2D 本体, 所以必须先在磁盘上
 * 找到归属的完整模型包, 再把补丁叠加到那个包的候选上 (patch_root、overrides 与 format 都被改写成
 * 补丁形态)。
 * 位置: 由精确包根探测与 nearby 扫描调用; 返回值沿用发现器约定 —— 大于 0 命中, 0 表示"不是补丁,
 * 请继续试别的格式", 负值表示错误。
 * 易错点: overrides 指向补丁里的模式目录, 模型本体仍从基准包读取, 两者混用会让补丁覆盖掉本体。
 */
#include "model_import.h"
#include "model_import_mver.h"
#include "../model_import_path.h"
#include "runtime.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

typedef struct PatchSearch {
    char image_root[BONGO_CAT_PATH_CAP];
    int depth;
    int matches;
} PatchSearch;

static bool child_file(const char *root, const char *first, const char *second) {
    char directory[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(directory, sizeof(directory), root, first) &&
        bongo_cat_path_join(path, sizeof(path), directory, second) &&
        bongo_cat_path_is_file(path);
}

/* 判定目录是不是 Mver 的 img 根: standard 模式要有 hand/0.png, gamepad 模式要有左右手各一张图片,
   满足其一即可。 */
static bool patch_shape(const char *root) {
    char mode[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(mode, sizeof(mode), root, "standard") &&
        child_file(mode, "hand", "0.png")) return true;
    return bongo_cat_path_join(mode, sizeof(mode), root, "gamepad") &&
        child_file(mode, "lefthand", "0.png") &&
        child_file(mode, "righthand", "0.png");
}

/* 递归 enumerator: 找出目录树里所有形如 img 的补丁根, 只记住第一个路径, 其余仅用于计数 (多于
   一个时调用方报补丁含多个模型根)。最多向下 3 层, 跳过点号开头的隐藏目录; 出错时返回 FAILURE
   让枚举整体中止。 */
static BongoCatPathVisit find_image_root(void *userdata,
    const char *dirname, const char *name) {
    PatchSearch *search = userdata;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), dirname, name))
        return BONGO_CAT_PATH_FAILURE;
    if (!bongo_cat_path_is_dir(path) || name[0] == '.')
        return BONGO_CAT_PATH_CONTINUE;
    if (strcmp(name, "img") == 0 && patch_shape(path)) {
        search->matches++;
        if (search->matches == 1)
            snprintf(search->image_root, sizeof(search->image_root), "%s", path);
        return BONGO_CAT_PATH_CONTINUE;
    }
    if (search->depth >= 3) return BONGO_CAT_PATH_CONTINUE;
    search->depth++;
    bool ok = bongo_cat_path_enumerate(path, find_image_root, search);
    search->depth--;
    return ok ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
}

static bool full_package(const char *path) {
    char config[BONGO_CAT_PATH_CAP], image[BONGO_CAT_PATH_CAP];
    return bongo_cat_import_mver_config_path(path, config, sizeof(config)) &&
        bongo_cat_path_join(image, sizeof(image), path, "img") &&
        bongo_cat_path_is_dir(image) &&
        patch_shape(image);
}

/* 从 source 起向上最多 6 层, 在每一层的同级目录里找名为 model_name 的完整模型包。返回命中个数
   (0 表示没找到), base 只写第一个命中; 调用方据此区分唯一基准与多个候选, 后者必须报错而不是
   随便挑一个。 */
static int find_base(const char *source, const char *model_name,
    char *base, size_t capacity) {
    char current[BONGO_CAT_PATH_CAP];
    int matches = 0;
    snprintf(current, sizeof(current), "%s", source);
    for (int depth = 0; depth < 6; ++depth) {
        char parent[BONGO_CAT_PATH_CAP], candidate[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_import_parent_path(current, parent, sizeof(parent)))
            break;
        if (bongo_cat_path_join(candidate, sizeof(candidate), parent, model_name) &&
            strcmp(candidate, source) != 0 && full_package(candidate)) {
            if (!matches) snprintf(base, capacity, "%s", candidate);
            matches++;
        }
        snprintf(current, sizeof(current), "%s", parent);
    }
    return matches;
}

/* 把补丁的图片目录叠加到唯一找到的基准包上: 先用基准包做常规 Mver 发现, 再逐个候选改写
   overrides、patch_root 与 format。返回发现器的值 (1 成功、0 未命中、-1 错误); discovery 由调用
   方提供并就地修改。 */
static int finish_patch(const char *source, const char *image_root,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    char model_root[BONGO_CAT_PATH_CAP], base[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_import_parent_path(image_root, model_root,
            sizeof(model_root))) return -1;
    int bases = find_base(source, bongo_cat_path_name(model_root), base, sizeof(base));
    if (bases != 1) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            bases ? "Mver image patch matches multiple full model packages: %s" :
            "Mver image patch requires a matching full model package: %s", source);
        return -1;
    }
    int result = bongo_cat_import_mver_discover(base, discovery, error);
    if (result <= 0) return result < 0 ? -1 : 0;
    const char *source_name = bongo_cat_path_name(source);
    if (source_name && source_name[0]) snprintf(discovery->source_name,
        sizeof(discovery->source_name), "%s", source_name);
    for (size_t i = 0; i < discovery->count; ++i) {
        BongoCatImportCandidate *candidate = &discovery->candidates[i];
        char mode[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(mode, sizeof(mode), image_root,
            bongo_cat_mode_name(candidate->mode)) && bongo_cat_path_is_dir(mode))
            snprintf(candidate->overrides, sizeof(candidate->overrides), "%s", mode);
        snprintf(candidate->patch_root, sizeof(candidate->patch_root), "%s",
            source);
        candidate->format = BONGO_CAT_IMPORT_MVER_PATCH;
    }
    return 1;
}

/* 只认两种情况: source 本身就是 img 补丁目录, 或 source 下的 img 是补丁目录。返回 0 表示不是
   补丁 (调用方可以继续探测其它格式), 不要求 source 存在。 */
int bongo_cat_import_mver_patch_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    char root[BONGO_CAT_PATH_CAP], image[BONGO_CAT_PATH_CAP];
    if (strcmp(bongo_cat_path_name(source), "img") == 0 && patch_shape(source)) {
        if (!bongo_cat_import_parent_path(source, root, sizeof(root)))
            return 0;
        return finish_patch(root, source, discovery, error);
    }
    if (!bongo_cat_path_join(image, sizeof(image), source, "img") ||
        !bongo_cat_path_is_dir(image) || !patch_shape(image)) return 0;
    return finish_patch(source, image, discovery, error);
}

/* 先做精确判定; 否则受限地扫描子树找唯一的 img 补丁根。枚举失败与没有补丁一样返回 0 (错误被
   吞掉, 好让上层继续尝试别的适配器), 找到多个补丁根则返回错误 -1。 */
int bongo_cat_import_mver_patch_discover(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    int exact = bongo_cat_import_mver_patch_discover_exact(source,
        discovery, error);
    if (exact) return exact;
    PatchSearch search = {0};
    if (!bongo_cat_path_enumerate(source, find_image_root, &search)) return 0;
    if (!search.matches) return 0;
    if (search.matches != 1) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Mver image patch contains multiple model roots: %s", source);
        return -1;
    }
    return finish_patch(source, search.image_root, discovery, error);
}
