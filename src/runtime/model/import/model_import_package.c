/*
 * 把"来源包"落成"存储包": Tauri 源转成 Mver(重写清单与贴图), Mver 源原样复制并归一化。
 * 三种目标布局: 单包根直接落到 target; 全是 Tauri 且多候选时按模式分子目录; 其余情况
 * (多个包根)保留相对目录结构, 避免把无关的同级目录一起搬走。
 * 只负责建目录与写文件, 不保证原子性 —— 调用方用临时目录加改名兜底。
 */
#include "model_import.h"
#include "model_import_path.h"
#include "model_import_mver_copy.h"
#include "tauri/model_import_tauri.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

/* 按来源格式分派到具体复制实现; installed 由被调方按目标布局改写。 */
static bool copy_package_files(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatImportCandidate *installed,
    BongoCatError *error) {
    if (candidate->format == BONGO_CAT_IMPORT_TAURI)
        return bongo_cat_import_tauri_convert_to_mver(candidate, target,
            installed, error);
    return bongo_cat_import_mver_copy_package(candidate, target, installed,
        error);
}

/* 前提: candidate、target、installed 非空, target 会被创建。先把候选整体复制给
   *installed, 再由适配器改写它指向目标里的真实位置; 返回 false 时 *installed 的内容
   没有意义(可能只改写了一半)。错误消息为空时补一句通用 IO 文案。 */
bool bongo_cat_import_prepare_package(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatImportCandidate *installed,
    BongoCatError *error) {
    if (!candidate || !target || !installed ||
        !bongo_cat_path_create_directory(target)) return false;
    *installed = *candidate;
    if (copy_package_files(candidate, target, installed, error)) return true;
    if (error && !error->message[0]) bongo_cat_error_set(error,
        BONGO_CAT_ERROR_IO, "Cannot preserve imported model files");
    return false;
}

/* 同一模式有多个 Tauri 变体时用 "gamepad-2" 这类后缀区分, 第一个不加后缀。
   名字必须稳定: 模型 id 与适配目录名都从这里派生, 改名等于换模型。 */
static bool tauri_variant_directory(char *output, size_t capacity,
    const char *target, const BongoCatImportDiscovery *discovery,
    size_t index) {
    const char *mode = bongo_cat_mode_name(discovery->candidates[index].mode);
    size_t occurrence = 1;
    for (size_t i = 0; i < index; ++i)
        if (discovery->candidates[i].mode == discovery->candidates[index].mode)
            occurrence++;
    char name[48];
    int written = occurrence == 1
        ? snprintf(name, sizeof(name), "%s", mode)
        : snprintf(name, sizeof(name), "%s-%zu", mode, occurrence);
    return written >= 0 && (size_t)written < sizeof(name) &&
        bongo_cat_path_join(output, capacity, target, name);
}

/* 求所有包根的公共父目录: 从第一个包根的父目录出发, 逐级上溯直到能相对表示每一个包根。
   它是容器布局的相对基准, 求不出来就放弃容器布局(返回 false)。 */
static bool common_package_root(const BongoCatImportDiscovery *discovery,
    char root[BONGO_CAT_PATH_CAP]) {
    if (!bongo_cat_import_parent_path(discovery->candidates[0].package_root,
            root, BONGO_CAT_PATH_CAP)) return false;
    for (size_t i = 1; i < discovery->count; ++i) {
        char relative[BONGO_CAT_PATH_CAP];
        while (!bongo_cat_import_relative_path(root,
                discovery->candidates[i].package_root, relative,
                sizeof(relative))) {
            char parent[BONGO_CAT_PATH_CAP];
            if (!bongo_cat_import_parent_path(root, parent, sizeof(parent)))
                return false;
            snprintf(root, BONGO_CAT_PATH_CAP, "%s", parent);
        }
    }
    return true;
}

/* 同一个 package_root 的多个候选只复制一次(按 package_root 去重), 共享同一份包内容;
   相对路径取相对公共根的部分, 因此 target 下的结构与源目录一一对应。 */
static bool prepare_container_storage(
    const BongoCatImportDiscovery *discovery, const char *target,
    BongoCatError *error) {
    char root[BONGO_CAT_PATH_CAP];
    if (!common_package_root(discovery, root)) return false;
    for (size_t i = 0; i < discovery->count; ++i) {
        bool already_copied = false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(discovery->candidates[j].package_root,
                    discovery->candidates[i].package_root) == 0) {
                already_copied = true;
                break;
            }
        if (already_copied) continue;
        char relative[BONGO_CAT_PATH_CAP], package[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_import_relative_path(root,
                discovery->candidates[i].package_root, relative,
                sizeof(relative)) || !relative[0] ||
            !bongo_cat_path_join(package, sizeof(package), target, relative))
            return false;
        BongoCatImportCandidate installed;
        if (!bongo_cat_import_prepare_package(&discovery->candidates[i],
                package, &installed, error)) return false;
    }
    return true;
}

static bool single_package_root(const BongoCatImportDiscovery *discovery) {
    for (size_t i = 1; i < discovery->count; ++i)
        if (strcmp(discovery->candidates[0].package_root,
                discovery->candidates[i].package_root) != 0) return false;
    return true;
}

/* 布局总决策, 依据是候选的 format 与 package_root 是否相同:
   全是 Tauri 且多候选 -> 按模式分子目录; 单包根 -> 直接落 target; 否则容器布局。 */
bool bongo_cat_import_prepare_storage(
    const BongoCatImportDiscovery *discovery, const char *target,
    BongoCatError *error) {
    if (!discovery || !discovery->count || !target) return false;
    bool tauri = true;
    for (size_t i = 0; i < discovery->count; ++i)
        tauri = tauri && discovery->candidates[i].format ==
            BONGO_CAT_IMPORT_TAURI;
    if (!tauri && single_package_root(discovery)) {
        BongoCatImportCandidate installed;
        return bongo_cat_import_prepare_package(&discovery->candidates[0],
            target, &installed, error);
    }
    if (!tauri) return prepare_container_storage(discovery, target, error);
    for (size_t i = 0; i < discovery->count; ++i) {
        char package[BONGO_CAT_PATH_CAP];
        if (discovery->count == 1)
            snprintf(package, sizeof(package), "%s", target);
        else if (!tauri_variant_directory(package, sizeof(package), target,
                discovery, i)) return false;
        BongoCatImportCandidate installed;
        if (!bongo_cat_import_prepare_package(&discovery->candidates[i],
                package, &installed, error)) return false;
    }
    return true;
}

/* 判断目录是不是"用户自己写的源包": 只要出现运行时占用的标记(见下面的 internal 列表,
   例如 .bongo-cat-adapter 与 .bongo-cat-builtin)就返回 false。调用方用它跳过已安装与
   内置模型; 名字读作"未被运行时接管的目录"更贴切。 */
bool bongo_cat_import_authored_package(const char *directory) {
    char path[BONGO_CAT_PATH_CAP];
    if (!directory || !bongo_cat_path_is_dir(directory)) return false;
    static const char *const internal[] = {
        ".bongo-cat-package.json", ".bongo-cat-adapter",
        BONGO_CAT_MODEL_BUILTIN_MARKER
    };
    for (size_t i = 0; i < sizeof(internal) / sizeof(internal[0]); ++i)
        if (bongo_cat_path_join(path, sizeof(path), directory, internal[i]) &&
            (bongo_cat_path_is_file(path) || bongo_cat_path_is_dir(path)))
            return false;
    return true;
}
