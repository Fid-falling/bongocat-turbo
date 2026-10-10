/*
 * Mver 包的落盘复制: 把来源包按原相对目录结构搬进 target, 并把 candidate 里指向来源的路径
 * (directory、assets、config, 补丁另加 patch_root 与 overrides) 改写成 target 下的位置, 结果写进 installed
 * 供后续适配器步骤使用。时序: 由 bongo_cat_import_prepare_package 在生成适配器之前调用, 失败时不要尝试回滚 ——
 * 调用方会丢弃整个临时目录。返回 false 时 installed 可能只改写了一半, 不可使用 (本函数不保证原子性)。
 */
#include "model_import_mver_copy.h"
#include "model_import_path.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

typedef struct MverDirectoryCopy {
    const char *target_root;
    BongoCatError *error;
    unsigned depth;
} MverDirectoryCopy;

/* 把来源侧路径映射到安装侧: 求 source 相对 source_root 的部分再拼到 installed_root 下; 两者相等时结果就是
 * installed_root 本身 (包根必须能映射到自己)。不在 source_root 之下时返回 false 且不写 target。 */
static bool rebase(const char *source_root, const char *installed_root,
    const char *source, char *target, size_t capacity) {
    char relative[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_import_relative_path(source_root, source, relative,
            sizeof(relative)))
        return false;
    if (!relative[0]) {
        int written = snprintf(target, capacity, "%s", installed_root);
        return written >= 0 && (size_t)written < capacity;
    }
    return bongo_cat_path_join(target, capacity, installed_root, relative);
}

/* 复制单个文件并保留相对路径: 目标已有同名文件时直接算成功 (补丁与本体重叠的文件不重复搬), 真正复制失败
 * 才补一条带源路径的 IO 错误; source 就是包根 (相对路径为空) 时返回 false。 */
static bool copy_relative_file(const char *source_root, const char *source,
    const char *target_root, BongoCatError *error) {
    char relative[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_import_relative_path(source_root, source, relative,
            sizeof(relative)) ||
        !relative[0] || !bongo_cat_path_join(target, sizeof(target),
            target_root, relative)) return false;
    if (bongo_cat_path_is_file(target)) return true;
    if (bongo_cat_path_copy_file(source, target)) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot copy imported model file: %s", source);
    return false;
}

/* 名字属于"发行物而不是模型素材"就在复制时跳过: 点号开头的隐藏项、_bak 备份后缀, 以及可执行文件、文档、
 * 压缩包等扩展名。比较一律大小写不敏感 (Windows 上 .EXE 与 .exe 是同一个东西)。 */
static bool distribution_file(const char *name) {
    if (!name || name[0] == '.') return true;
    size_t length = strlen(name);
    if (length >= 4 && SDL_strcasecmp(name + length - 4, "_bak") == 0)
        return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *const extensions[] = {
        ".exe", ".dll", ".pdf", ".bak", ".lock", ".pdb", ".zip"
    };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (SDL_strcasecmp(dot, extensions[i]) == 0) return true;
    return false;
}

/* 认出 Mver 自带的运行时资源目录 (名字必须是 Resources, 且含 cat.ttf 与 l2dlogo.png): 那是原版程序的皮肤
 * 资源, 与模型无关, 搬进来只会让模型目录凭空多出几十兆, 所以整棵子树跳过。 */
static bool runtime_resources(const char *directory, const char *name) {
    if (!name || SDL_strcasecmp(name, "Resources") != 0 ||
        !bongo_cat_path_is_dir(directory)) return false;
    char font[BONGO_CAT_PATH_CAP], logo[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(font, sizeof(font), directory, "cat.ttf") &&
        bongo_cat_path_join(logo, sizeof(logo), directory, "l2dlogo.png") &&
        bongo_cat_path_is_file(font) && bongo_cat_path_is_file(logo);
}

static bool copy_tree(const char *source, const char *target,
    unsigned depth, BongoCatError *error);

/* 枚举回调: 逐项决定复制、跳过还是中止 (返回 FAILURE 会让整棵枚举失败并向上冒泡)。包根层 (depth == 0) 的
 * 散文件一律跳过 —— 包根的 config.json 由 copy_package 按相对路径单独复制, 其余多是 README 之类; 子目录
 * 递归时 depth 加一, 从那一层起文件才真的被搬走。同名目标会被 copy_file 覆盖, 这里不做去重。 */
static BongoCatPathVisit copy_child(void *userdata, const char *dirname,
    const char *name) {
    MverDirectoryCopy *context = userdata;
    char source[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(source, sizeof(source), dirname, name))
        return BONGO_CAT_PATH_FAILURE;
    if (runtime_resources(source, name) || distribution_file(name))
        return BONGO_CAT_PATH_CONTINUE;
    char target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(target, sizeof(target), context->target_root,
            name)) return BONGO_CAT_PATH_FAILURE;
    if (!bongo_cat_path_is_dir(source)) {
        if (context->depth == 0) return BONGO_CAT_PATH_CONTINUE;
        return bongo_cat_path_copy_file(source, target)
            ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
    }
    return copy_tree(source, target, context->depth + 1, context->error)
        ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
}

/* 递归入口: 先建目标目录再枚举。depth > 32 直接判失败 —— 正常模型远达不到这个深度, 只有符号链接成环才会
 * 一直往下; 这个判断与建目录失败共用同一句错误文案, 看到 "Cannot create imported model directory" 时也要
 * 怀疑是深度超限。 */
static bool copy_tree(const char *source, const char *target,
    unsigned depth, BongoCatError *error) {
    if (depth > 32 || !bongo_cat_path_create_directory(target)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot create imported model directory: %s", target);
        return false;
    }
    MverDirectoryCopy context = {target, error, depth};
    return bongo_cat_path_enumerate(source, copy_child, &context);
}

/* 补丁的 overrides 可能落在包根之外, 这时整棵子树要按相对补丁根的结构单独复制一次; copy_tree 从 depth 1
 * 起步, 所以该目录下的散文件 (就是覆盖用的贴图) 也会被复制。 */
static bool copy_relative_directory(const char *source_root,
    const char *source, const char *target_root, BongoCatError *error) {
    char relative[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_import_relative_path(source_root, source, relative,
            sizeof(relative)) ||
        !relative[0] || !bongo_cat_path_join(target, sizeof(target),
            target_root, relative)) return false;
    return copy_tree(source, target, 1, error);
}

/* 复制整包: 建 target、按相对结构搬目录树, 再单独补 config.json —— 包根层的散文件被 copy_tree 跳过, 而
 * 标准布局的 config 正好在包根; config 缺失会在这里报错, 因为后续所有转换都依赖它。 */
static bool copy_package(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error) {
    if (!bongo_cat_path_create_directory(target) ||
        !copy_tree(candidate->package_root, target, 0, error)) return false;
    return copy_relative_file(candidate->package_root, candidate->config,
        target, error);
}

/* 入口: installed 接收改写后的候选, 其路径字段全部指向 target 之下, 存储由调用方负责。
 * 补丁分两种情况: patch_root 在包内时它随整包一起被复制, 在包外时另把 overrides 子树搬进来; 两种情况的相对
 * 基准都是 patch_source_root —— 换成 package_root 会让 overrides 的映射错位。
 * 只改写路径字段与 format, 其余字段 (mode、setting、gamepad_buttons) 原样保留, 不能在这里丢。 */
bool bongo_cat_import_mver_copy_package(
    const BongoCatImportCandidate *candidate, const char *target,
    BongoCatImportCandidate *installed, BongoCatError *error) {
    if (!candidate || !target || !installed) return false;
    char base[BONGO_CAT_PATH_CAP];
    bool patch = candidate->format == BONGO_CAT_IMPORT_MVER_PATCH;
    if (patch) {
        char patch_relative[BONGO_CAT_PATH_CAP];
        bool patch_inside_package = bongo_cat_import_relative_path(
            candidate->package_root, candidate->patch_root, patch_relative,
            sizeof(patch_relative));
        const char *patch_source_root = patch_inside_package
            ? candidate->package_root : candidate->patch_root;
        snprintf(base, sizeof(base), "%s", target);
        if (!copy_package(candidate, base, error) ||
            (!patch_inside_package && candidate->overrides[0] &&
                !copy_relative_directory(patch_source_root,
                    candidate->overrides, target, error)) ||
            (candidate->overrides[0] && !rebase(patch_source_root, target,
                candidate->overrides, installed->overrides,
                sizeof(installed->overrides))) ||
            !rebase(patch_source_root, target, candidate->patch_root,
                installed->patch_root, sizeof(installed->patch_root)))
            return false;
    } else if (candidate->format == BONGO_CAT_IMPORT_MVER) {
        snprintf(base, sizeof(base), "%s", target);
        if (!copy_package(candidate, base, error)) return false;
    } else return false;
    if (!rebase(candidate->package_root, base, candidate->directory,
            installed->directory, sizeof(installed->directory)) ||
        !rebase(candidate->package_root, base, candidate->assets,
            installed->assets, sizeof(installed->assets)) ||
        !rebase(candidate->package_root, base, candidate->config,
            installed->config, sizeof(installed->config))) return false;
    installed->format = candidate->format;
    return true;
}
