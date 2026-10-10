/*
 * Mver 包发现器：用 bongocat.skin.json 或 config.json 加 img/<mode> 布局识别 Mver 包，并把各模式枚举成导入候选。
 * 位置在导入流程最前置的一步：精确探测与广域识别都先问它，返回 0 才轮到贴图补丁与 Tauri 适配器，因此
 * 这里的判定宽严决定哪个格式拥有解释权。
 * 契约：大于 0 命中（discovery->count 已增加），0 表示不是 Mver 包，负值表示确实是 Mver 包但内容不可用
 * （通常已写 error）；调用前必须清空 discovery，因为"有没有可用模式"的判定看的是总数而不是本次新增数。
 * 易错点：l2d 为 false 的模式只是随包附带的静态贴图模板，不是候选；模式的登记顺序还会决定导入后默认选中的模型（receipt.ids[0]）。
 */
#include "model_import.h"
#include "model_import_mver_manifest.h"
#include "model_import_mver.h"
#include "model_import_path.h"
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

/* 拼 root/first/second：中间结果借栈上的缓冲区，只有最终路径受 capacity 约束；失败时 output 内容未定义。 */
static bool child_path(char *output, size_t capacity, const char *root,
    const char *first, const char *second) {
    char intermediate[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(intermediate, sizeof(intermediate), root, first) &&
        bongo_cat_path_join(output, capacity, intermediate, second);
}

static bool numbered_asset(const char *directory, const char *group) {
    char path[BONGO_CAT_PATH_CAP];
    return child_path(path, sizeof(path), directory, group, "0.png") &&
        bongo_cat_path_is_file(path);
}

/* 目录形状判定：三个模式目录里有一个符合 Mver 布局就算包根 —— 贴图包看 hand 或者 lefthand 与
   righthand，Live2D 包看模式目录或 cat_model 下的 .model3.json。这里用宽松的 find_suffix 只求
   "存在"，唯一性与清单合法性留给 model_at 严格判定。 */
static bool mver_shape(const char *image_root) {
    char mode[BONGO_CAT_PATH_CAP], model[BONGO_CAT_PATH_CAP];
    char setting[BONGO_CAT_PATH_CAP];
    const char *names[] = {"standard", "keyboard", "gamepad"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!bongo_cat_path_join(mode, sizeof(mode), image_root, names[i]))
            continue;
        if (i == 0 ? numbered_asset(mode, "hand") :
            numbered_asset(mode, "lefthand") && numbered_asset(mode, "righthand"))
            return true;
        /* Live2D packages may omit all optional hand sprites. */
        if (bongo_cat_path_find_suffix(mode, ".model3.json", setting,
                sizeof(setting)) ||
            (bongo_cat_path_join(model, sizeof(model), mode, "cat_model") &&
                bongo_cat_path_find_suffix(model, ".model3.json", setting,
                    sizeof(setting)))) return true;
    }
    return false;
}

/* 定位配置文件：bongocat.skin.json 优先于 config.json，两者都是合法的导入入口。 */
bool bongo_cat_import_mver_config_path(const char *root,
    char *path, size_t capacity) {
    static const char *names[] = {BONGO_CAT_SKIN_CONFIG_FILE, "config.json"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (bongo_cat_path_join(path, capacity, root, names[i]) &&
            bongo_cat_path_is_file(path)) return true;
    return false;
}

static bool package_at(const char *source, char *config, char *image_root) {
    return bongo_cat_import_mver_config_path(source, config,
            BONGO_CAT_PATH_CAP) &&
        bongo_cat_path_join(image_root, BONGO_CAT_PATH_CAP, source, "img") &&
        bongo_cat_path_is_dir(image_root) && mver_shape(image_root);
}

/* 从 source 起向上最多找 4 层包根，好让用户选中包内的子目录也能导入；失败时 package、config 与
   image_root 的内容没有意义，调用方只能看返回值。 */
static bool find_package(const char *source, char *package, size_t capacity,
    char *config, char *image_root) {
    snprintf(package, capacity, "%s", source);
    for (int depth = 0; depth < 4; ++depth) {
        if (package_at(package, config, image_root)) return true;
        char parent[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_import_parent_path(package, parent, sizeof(parent)))
            break;
        snprintf(package, capacity, "%s", parent);
    }
    return false;
}

/* 输入矩阵可以省略或写 null（该模式不响应这类输入），只有写成非数组才算配置损坏。 */
static bool optional_matrix(yyjson_val *mode, const char *name) {
    yyjson_val *value = yyjson_obj_get(mode, name);
    return !value || yyjson_is_null(value) || yyjson_is_arr(value);
}

/* standard 只认 hand，其余模式要求 lefthand 与 righthand；通过校验不代表矩阵非空（空数组是合法配置）。 */
static bool mode_config_valid(yyjson_val *mode, BongoCatModelMode value) {
    if (!yyjson_is_obj(mode)) return false;
    if (value == BONGO_CAT_MODE_STANDARD)
        return optional_matrix(mode, "hand");
    return optional_matrix(mode, "lefthand") && optional_matrix(mode, "righthand");
}

/* l2d 缺省为 true：老版本的 config 里没有这个字段，那时只有 Live2D 渲染。 */
static bool mode_uses_live2d(yyjson_val *mode) {
    yyjson_val *enabled = yyjson_obj_get(mode, "l2d");
    return !yyjson_is_bool(enabled) || yyjson_get_bool(enabled);
}

/* 模式目录里必须"有且仅有一个" .model3.json（多于一个就无法确定要加载哪个变体），并且它要能过
   宽松的 Mver 清单校验（允许缺 Physics、Pose 等可选段落）。 */
static bool model_at(const char *directory, char *setting, size_t capacity) {
    return bongo_cat_path_find_unique_suffix(directory, ".model3.json",
        setting, capacity) == 1 &&
        bongo_cat_import_mver_manifest_valid(directory, setting);
}

/* 清单先找 <mode>/cat_model，再退回模式目录本身（Mver 的两种常见布局）；directory 无论成败都会
   被改写，只有返回 true 时 directory 与 setting 才构成有效入口。 */
static bool find_mode_model(const char *mode_root, char *directory,
    size_t directory_capacity, char *setting, size_t setting_capacity) {
    if (bongo_cat_path_join(directory, directory_capacity, mode_root, "cat_model") &&
        bongo_cat_path_is_dir(directory) && model_at(directory, setting, setting_capacity))
        return true;
    snprintf(directory, directory_capacity, "%s", mode_root);
    return model_at(directory, setting, setting_capacity);
}

/* 把一个模式登记成候选：目录或模式对象缺失、以及 l2d 为 false（静态贴图模板）都算"这个包没提供
   该模式"而静默跳过；输入映射损坏、没有合法清单或候选表已满才算失败（表满时不写 error）。成功时
   占用 discovery 尾部并 count++，失败不回滚已写入的候选 —— 由失败路径上的整体清零兜底。 */
static bool add_mode(BongoCatImportDiscovery *discovery, const char *source,
    const char *config, const char *image_root, yyjson_val *root,
    BongoCatModelMode mode, BongoCatError *error) {
    const char *name = bongo_cat_mode_name(mode);
    char mode_root[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(mode_root, sizeof(mode_root), image_root, name)) return false;
    yyjson_val *mode_config = yyjson_obj_get(root, name);
    if (!bongo_cat_path_is_dir(mode_root) || !yyjson_is_obj(mode_config)) return true;
    /* Mver distributions bundle fallback Live2D files for modes that are
       configured to use static sprites. They are runtime templates, not
       authored model variants, so they must not become import candidates. */
    if (!mode_uses_live2d(mode_config)) return true;
    if (!mode_config_valid(mode_config, mode)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Mver mode has invalid input mappings: %s", mode_root);
        return false;
    }
    if (discovery->count >= BONGO_CAT_IMPORT_CANDIDATE_CAP) return false;
    BongoCatImportCandidate *candidate = &discovery->candidates[discovery->count];
    if (!find_mode_model(mode_root, candidate->directory, sizeof(candidate->directory),
        candidate->setting, sizeof(candidate->setting))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Mver mode contains no valid Live2D model: %s", mode_root);
        return false;
    }
    snprintf(candidate->assets, sizeof(candidate->assets), "%s", mode_root);
    snprintf(candidate->package_root, sizeof(candidate->package_root), "%s", source);
    snprintf(candidate->config, sizeof(candidate->config), "%s", config);
    candidate->mode = mode;
    candidate->format = BONGO_CAT_IMPORT_MVER;
    /* 只有 input_mode 显式为整数 0 时才关掉手柄按钮语义；缺字段或非整数都按手柄解释键码。 */
    yyjson_val *input_mode = yyjson_obj_get(mode_config, "input_mode");
    candidate->gamepad_buttons = mode == BONGO_CAT_MODE_GAMEPAD &&
        ((!yyjson_is_int(input_mode) && !yyjson_is_uint(input_mode)) ||
        yyjson_get_int(input_mode) != 0);
    discovery->count++;
    return true;
}

/* 解析 config 并登记三个模式；解析失败或一个候选都没产出时报 -1（错误码统一是 FORMAT，因为对用户
   而言就是"这个包坏了"）。document 在所有返回路径上都会释放。 */
static int discover_package(const char *package, const char *config,
    const char *image_root, BongoCatImportDiscovery *discovery,
    BongoCatError *error) {
    FILE *file = bongo_cat_file_open(config, "rb");
    yyjson_doc *document = file ? yyjson_read_fp(file,
        YYJSON_READ_JSON5 | YYJSON_READ_ALLOW_INVALID_UNICODE, NULL, NULL) : NULL;
    if (file) fclose(file);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(document);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Cannot parse Mver configuration: %s", config);
        return -1;
    }
    /* 固定按 standard、keyboard、gamepad 的顺序登记：广域识别命中时不会再排序，候选下标原样变成
       receipt.ids 的顺序，而 ids[0] 就是导入完成后选中的模型。 */
    bool ok = add_mode(discovery, package, config, image_root, root,
        BONGO_CAT_MODE_STANDARD, error) &&
        add_mode(discovery, package, config, image_root, root,
            BONGO_CAT_MODE_KEYBOARD, error) &&
        add_mode(discovery, package, config, image_root, root,
            BONGO_CAT_MODE_GAMEPAD, error);
    yyjson_doc_free(document);
    if (!ok) return -1;
    if (!discovery->count) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Mver package contains no supported model modes: %s", package);
        return -1;
    }
    return 1;
}

/* 精确模式：只认 source 自己就是包根，不向上回溯（probe_exact 靠这条语义分发格式）；source_name
   只在调用方没填时才补，因为用户选中的目录名才是要展示的模型名。 */
int bongo_cat_import_mver_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    char config[BONGO_CAT_PATH_CAP], image_root[BONGO_CAT_PATH_CAP];
    if (!package_at(source, config, image_root)) return 0;
    if (!discovery->source_name[0]) snprintf(discovery->source_name,
        sizeof(discovery->source_name), "%s", bongo_cat_path_name(source));
    return discover_package(source, config, image_root, discovery, error);
}

/* 宽查找模式：允许 source 是包内任意子目录，先向上归一化到包根；package_root 记的是归一化后的根，
   后续复制、去重与布局归一化都按它分组，所以不能改成用户给的原始路径。 */
int bongo_cat_import_mver_discover(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    char package[BONGO_CAT_PATH_CAP], config[BONGO_CAT_PATH_CAP];
    char image_root[BONGO_CAT_PATH_CAP];
    if (!find_package(source, package, sizeof(package), config, image_root)) return 0;
    if (!discovery->source_name[0]) snprintf(discovery->source_name,
        sizeof(discovery->source_name), "%s", bongo_cat_path_name(package));
    return discover_package(package, config, image_root, discovery, error);
}
