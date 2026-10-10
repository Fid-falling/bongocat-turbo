#include "bongo_cat/file.h"
#include "bongo_cat/model.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "windows_utf8.h"
#include <stdlib.h>
#include <windows.h>
#else
#include <dirent.h>
#endif

/*
 * 模型目录的扫描与查找。
 *
 * 扫描规则: 逐个看 root 下的子目录, 只有"用户自己放进去的模型"(带内置标记文件)
 * 或 preset(随程序分发的内置模型)才算数; 目录里找不到 .model3.json 的直接跳过
 * —— 那不是模型, 可能只是用户随手丢进去的文件夹。
 * entries 是定长数组, 顺序就是磁盘返回的顺序(平台相关), count 之后没有有效数据。
 */
void bongo_cat_models_init(BongoCatModelCatalog *catalog) {
    if (catalog) memset(catalog, 0, sizeof(*catalog));
}

bool bongo_cat_model_adapter_metadata_path(const char *directory,
    char *path, size_t capacity) {
    return directory && path && capacity &&
        bongo_cat_path_join(path, capacity, directory,
            BONGO_CAT_MODEL_ADAPTER_FILE);
}

/* 判断模型属于哪个模式(标准 / 键盘 / 手柄), 按可信度从高到低三级回退: 先看模型
   自己写的 .bongo-cat-mode, 再看目录名, 最后看贴图结构(resources/right-keys/
   East.png 存在 = 手柄; 只有 right-keys 目录 = 键盘)。三级都判断不出来就当标准
   模式 —— 猜错只影响默认按键布局, 不会让模型加载失败。 */
static BongoCatModelMode infer_mode(const char *directory) {
    char path[BONGO_CAT_PATH_CAP], stored[16] = {0};
    bongo_cat_path_join(path, sizeof(path), directory, ".bongo-cat-mode");
    FILE *file = bongo_cat_file_open(path, "rb");
    if (file) {
        size_t length = fread(stored, 1, sizeof(stored) - 1, file);
        fclose(file);
        stored[length] = '\0';
        if (strcmp(stored, "gamepad") == 0) return BONGO_CAT_MODE_GAMEPAD;
        if (strcmp(stored, "keyboard") == 0) return BONGO_CAT_MODE_KEYBOARD;
        if (strcmp(stored, "standard") == 0) return BONGO_CAT_MODE_STANDARD;
    }
    const char *name = bongo_cat_path_name(directory);
    if (strcmp(name, "gamepad") == 0) return BONGO_CAT_MODE_GAMEPAD;
    if (strcmp(name, "keyboard") == 0) return BONGO_CAT_MODE_KEYBOARD;
    if (strcmp(name, "standard") == 0) return BONGO_CAT_MODE_STANDARD;
    bongo_cat_path_join(path, sizeof(path), directory, "resources/right-keys/East.png");
    if (bongo_cat_path_is_file(path)) return BONGO_CAT_MODE_GAMEPAD;
    bongo_cat_path_join(path, sizeof(path), directory, "resources/right-keys");
    return bongo_cat_path_is_dir(path) ? BONGO_CAT_MODE_KEYBOARD : BONGO_CAT_MODE_STANDARD;
}

/* 内置标记文件用来区分"程序自带的模型"和"用户导入的模型": 前者由安装包管理,
   只有后者才允许删除或改名。 */
static bool builtin_marker(const char *directory) {
    char marker[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(marker, sizeof(marker), directory,
        BONGO_CAT_MODEL_BUILTIN_MARKER) && bongo_cat_path_is_file(marker);
}

/* 返回 false 只表示"模型太多"或"路径太长"这类硬错误; 目录里没有模型文件属于
   正常情况, 返回 true 但不添加任何条目。id 直接用目录名 —— 设置里的自定义名称、
   已删除列表都以它为键, 所以重命名目录等于换了一个模型。 */
static bool add_model(BongoCatModelCatalog *catalog, const char *directory, bool preset) {
    if (!preset && !builtin_marker(directory)) return true;
    if (catalog->count >= BONGO_CAT_MODEL_CAP) return false;
    char setting[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_find_suffix(directory, ".model3.json", setting, sizeof(setting))) return true;
    BongoCatModelEntry *entry = &catalog->entries[catalog->count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, sizeof(entry->id), "%s", bongo_cat_path_name(directory));
    snprintf(entry->package_id, sizeof(entry->package_id), "%s", entry->id);
    snprintf(entry->directory, sizeof(entry->directory), "%s", directory);
    snprintf(entry->adapter_directory, sizeof(entry->adapter_directory), "%s", directory);
    snprintf(entry->storage_directory, sizeof(entry->storage_directory), "%s", directory);
    snprintf(entry->setting_file, sizeof(entry->setting_file), "%s", setting);
    entry->mode = infer_mode(directory);
    entry->source_format = BONGO_CAT_MODEL_SOURCE_BUILTIN;
    entry->capabilities = BONGO_CAT_MODEL_CAPABILITY_LIVE2D |
        BONGO_CAT_MODEL_CAPABILITY_PREVIEW;
    entry->preset = preset || builtin_marker(directory);
    return true;
}

#ifdef _WIN32
static BongoCatResult scan_windows(BongoCatModelCatalog *catalog, const char *root, bool preset,
    BongoCatError *error) {
    char pattern[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(pattern, sizeof(pattern), root, "*");
    wchar_t *wide = bongo_cat_windows_wide(pattern);
    WIN32_FIND_DATAW data = {0};
    HANDLE find = wide ? FindFirstFileW(wide, &data) : INVALID_HANDLE_VALUE;
    free(wide);
    if (find == INVALID_HANDLE_VALUE) return BONGO_CAT_OK;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        char filename[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_windows_utf8(data.cFileName, filename, sizeof(filename))) continue;
        char directory[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(directory, sizeof(directory), root, filename) ||
            !add_model(catalog, directory, preset)) {
            FindClose(find);
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT, "Too many models or a path is too long");
            return BONGO_CAT_ERROR_FORMAT;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return BONGO_CAT_OK;
}
#else

static BongoCatResult scan_posix(BongoCatModelCatalog *catalog, const char *root, bool preset,
    BongoCatError *error) {
    DIR *handle = opendir(root);
    if (!handle) return BONGO_CAT_OK;
    struct dirent *item;
    while ((item = readdir(handle))) {
        if (item->d_name[0] == '.') continue;
        char directory[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(directory, sizeof(directory), root, item->d_name)) continue;
        if (!bongo_cat_path_is_dir(directory)) continue;
        if (!add_model(catalog, directory, preset)) {
            closedir(handle);
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT, "Too many models");
            return BONGO_CAT_ERROR_FORMAT;
        }
    }
    closedir(handle);
    return BONGO_CAT_OK;
}
#endif

/* 目录不存在不算错误(首次运行时模型目录还没建), 扫描结果为空即可。 */
BongoCatResult bongo_cat_models_scan(BongoCatModelCatalog *catalog, const char *root, bool preset,
    BongoCatError *error) {
    if (!catalog || !root) return BONGO_CAT_ERROR_ARGUMENT;
#ifdef _WIN32
    return scan_windows(catalog, root, preset, error);
#else
    return scan_posix(catalog, root, preset, error);
#endif
}

const BongoCatModelEntry *bongo_cat_models_find(const BongoCatModelCatalog *catalog, const char *id) {
    if (!catalog || !id) return NULL;
    for (size_t i = 0; i < catalog->count; ++i) {
        if (strcmp(catalog->entries[i].id, id) == 0) return &catalog->entries[i];
    }
    return NULL;
}

/* 显示名的回退链: 模型自带的 display_name -> 三个内置模型的固定英文名 -> 目录
   名。返回值可能指向 entry 内部, 调用方不要释放。 */
const char *bongo_cat_model_default_name(const BongoCatModelEntry *entry) {
    if (!entry) return "model";
    if (entry->display_name[0]) return entry->display_name;
    if (!strcmp(entry->id, "standard")) return "Standard";
    if (!strcmp(entry->id, "keyboard")) return "Keyboard";
    if (!strcmp(entry->id, "gamepad")) return "Gamepad";
    return entry->id;
}

/* 用户设置的名字优先于模型自带的名字(见 config_model_state.c 里的 modelOverrides)。
   settings 允许为 NULL, 那表示配置还没加载, 直接退回默认名。 */
const char *bongo_cat_model_name(const BongoCatSettings *settings,
    const BongoCatModelEntry *entry) {
    if (!entry) return "model";
    const char *custom = bongo_cat_settings_model_label(settings, entry->id);
    return custom && custom[0] ? custom : bongo_cat_model_default_name(entry);
}
