/*
 * 候选构造: 把一个已发现的 Tauri 目录变成 BongoCatImportCandidate。
 * 这里确定后续转换依赖的字段: directory (含 model3.json 的目录)、setting (清单文件名)、
 * assets (预览图与按键图的资源根, 可能上溯到父目录)、package_root (包根, 先按模型目录
 * 填, 精确发现时会再改写成用户选中的目录)。
 * 模式判定分两级: 先看 resources/right-keys 里出现的键名判手柄/键盘, 判不出来再退回
 * 目录名关键字 (含中文“键盘/手柄/标准”); 两级都失败时按 standard 处理。
 * 最容易改错的是 assets 的上溯规则, 它会改变后面所有找图的搜索根。
 */
#include "model_import_tauri_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 目录名匹配用“裸字节子串查找”, 不经过 locale 也不做正规化: 导出器会给目录加前后缀,
 * 所以只能按子串判; 中文关键字直接以 UTF-8 字节序列比对 (见下面的 utf8_contains),
 * 因为 Tauri 中文包的目录名就是“键盘 / 手柄 / 标准”这三个词。 */
static bool ascii_contains_ci(const char *value, const char *needle) {
    if (!value || !needle || !needle[0]) return false;
    size_t length = strlen(needle);
    for (const char *cursor = value; *cursor; ++cursor)
        if (SDL_strncasecmp(cursor, needle, length) == 0) return true;
    return false;
}

static bool utf8_contains(const char *value, const unsigned char *needle,
    size_t length) {
    if (!value || !needle || !length) return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor;
        ++cursor) {
        size_t remaining = strlen((const char *)cursor);
        if (remaining >= length && memcmp(cursor, needle, length) == 0)
            return true;
    }
    return false;
}

/* 目录名的模式启发式。按路径段逐层扫描, 越深的段越优先 (后写覆盖先写), 但 basename
 * 一旦命中关键字就直接返回 —— basename 比任何中间目录都权威, 因为用户看到的是它。 */
static BongoCatModelMode import_mode(const char *path) {
    const char *cursor = path;
    BongoCatModelMode mode = BONGO_CAT_MODE_STANDARD;
    while (cursor && *cursor) {
        while (*cursor == '/' || *cursor == '\\') cursor++;
        const char *end = strpbrk(cursor, "/\\");
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length == 8 && SDL_strncasecmp(cursor, "keyboard", length) == 0)
            mode = BONGO_CAT_MODE_KEYBOARD;
        else if (length == 7 && SDL_strncasecmp(cursor, "gamepad", length) == 0)
            mode = BONGO_CAT_MODE_GAMEPAD;
        else if (length == 8 && SDL_strncasecmp(cursor, "standard", length) == 0)
            mode = BONGO_CAT_MODE_STANDARD;
        if (!end) break;
        cursor = end + 1;
    }
    const char *name = bongo_cat_path_name(path);
    static const unsigned char keyboard_cn[] = {0xe9,0x94,0xae,0xe7,0x9b,0x98};
    static const unsigned char gamepad_cn[] = {0xe6,0x89,0x8b,0xe6,0x9f,0x84};
    static const unsigned char standard_cn[] = {0xe6,0xa0,0x87,0xe5,0x87,0x86};
    if (name && (ascii_contains_ci(name, "keyboard") ||
            utf8_contains(name, keyboard_cn, sizeof(keyboard_cn))))
        return BONGO_CAT_MODE_KEYBOARD;
    if (name && (ascii_contains_ci(name, "gamepad") ||
            utf8_contains(name, gamepad_cn, sizeof(gamepad_cn))))
        return BONGO_CAT_MODE_GAMEPAD;
    if (name && (ascii_contains_ci(name, "standard") ||
            utf8_contains(name, standard_cn, sizeof(standard_cn))))
        return BONGO_CAT_MODE_STANDARD;
    return mode;
}

/* 下面三个探测函数回答“这一层有没有能当预览/背景用的资源”: 任意一个候选名命中就算有。
 * has_preview_assets 连 resources/ 目录也算命中, 另外两个只认文件。 */
static bool has_preview_assets(const char *directory) {
    const char *names[] = {"resources", "cover.png", "cat.png", "bg.png",
        "mousebg.png", "tabletbg.png"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(path, sizeof(path), directory, names[i]))
            continue;
        if (bongo_cat_path_is_file(path) || bongo_cat_path_is_dir(path))
            return true;
    }
    return false;
}

static bool has_cover_asset(const char *directory) {
    const char *names[] = {
        "resources/cover.png", "cover.png", "cat.png", "bg.png"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(path, sizeof(path), directory, names[i]) &&
            bongo_cat_path_is_file(path)) return true;
    }
    return false;
}

static bool has_background_asset(const char *directory) {
    const char *names[] = {"resources/background.png", "background.png",
        "bg.png", "mousebg.png", "tabletbg.png"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(path, sizeof(path), directory, names[i]) &&
            bongo_cat_path_is_file(path)) return true;
    }
    return false;
}

typedef struct TauriRightKeys {
    bool gamepad;
} TauriRightKeys;

/* 设计上假定只有手柄包会在 right-keys 里出现这 16 个键名 (与 model_import_tauri_keys.c 的手柄表、
 * Mver 侧的 bongo_cat_mver_gamepad_names 同名同序)。名字必须恰好等于表项加 .png,
 * 因为键盘包的图都叫 KeyA.png 这类名字, 不会误撞。 */
static bool gamepad_key_name(const char *name) {
    static const char *const names[] = {
        "South", "East", "West", "North", "LeftTrigger", "RightTrigger",
        "LeftTrigger2", "RightTrigger2", "LeftThumb", "RightThumb",
        "DPadLeft", "DPadRight", "DPadUp", "DPadDown", "Start", "Select"
    };
    size_t length = name ? strlen(name) : 0;
    if (length <= 4 || SDL_strcasecmp(name + length - 4, ".png") != 0)
        return false;
    length -= 4;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strlen(names[i]) == length &&
            SDL_strncasecmp(name, names[i], length) == 0) return true;
    return false;
}

static BongoCatPathVisit inspect_right_key(void *userdata,
    const char *dirname, const char *name) {
    (void)dirname;
    TauriRightKeys *keys = userdata;
    if (gamepad_key_name(name)) keys->gamepad = true;
    return BONGO_CAT_PATH_CONTINUE;
}

/* 用 right-keys 目录判定模式: 目录里的键名决定这包是手柄还是键盘。目录不存在或枚举
 * 失败都返回 false (调用方一律退回目录名启发式, 不需要区分这两种情况); 目录存在但
 * 为空时按键盘处理。 */
static bool tauri_resource_mode(const char *directory, const char *assets,
    BongoCatModelMode *mode, bool *gamepad_buttons) {
    const char *roots[] = {directory, assets};
    bool right_keys = false;
    TauriRightKeys keys = {0};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (!roots[i] || !roots[i][0] || (i && strcmp(roots[0], roots[i]) == 0))
            continue;
        const char *prefixes[] = {"resources", NULL};
        for (size_t j = 0; j < sizeof(prefixes) / sizeof(prefixes[0]); ++j) {
            char parent[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
            const char *root = roots[i];
            if (prefixes[j]) {
                if (!bongo_cat_path_join(parent, sizeof(parent), root,
                        prefixes[j])) continue;
                root = parent;
            }
            if (!bongo_cat_path_join(path, sizeof(path), root, "right-keys") ||
                !bongo_cat_path_is_dir(path)) continue;
            right_keys = true;
            if (!bongo_cat_path_enumerate(path, inspect_right_key, &keys))
                return false;
        }
    }
    if (!right_keys) return false;
    *mode = keys.gamepad ? BONGO_CAT_MODE_GAMEPAD : BONGO_CAT_MODE_KEYBOARD;
    *gamepad_buttons = keys.gamepad;
    return true;
}

/* 把一个 Tauri 目录登记成候选。返回值 true 只表示“这个目录不用再报了”, 不代表新增:
 * 同一目录 + 同一清单 = 重复 (静默忽略); 同一目录 + 不同清单 = 歧义, 置
 * discovery->ambiguous 并返回 false; 容量用尽时也返回 false, 但会走另一条错误文案。
 * assets 默认等于 directory, 只有本层缺封面/背景、父目录有预览资源时才上溯父目录 ——
 * Tauri 导出器常把预览图放在包根、模型放在子目录。 */

bool bongo_cat_import_tauri_add_candidate(BongoCatImportDiscovery *discovery,
    const char *directory, const char *setting) {
    if (discovery->count >= BONGO_CAT_IMPORT_CANDIDATE_CAP) return false;
    for (size_t i = 0; i < discovery->count; ++i)
        if (strcmp(discovery->candidates[i].directory, directory) == 0) {
            if (strcmp(discovery->candidates[i].setting, setting) == 0)
                return true;
            discovery->ambiguous = true;
            return false;
        }
    BongoCatImportCandidate *candidate =
        &discovery->candidates[discovery->count++];
    snprintf(candidate->directory, sizeof(candidate->directory), "%s",
        directory);
    snprintf(candidate->setting, sizeof(candidate->setting), "%s", setting);
    snprintf(candidate->assets, sizeof(candidate->assets), "%s", directory);
    snprintf(candidate->package_root, sizeof(candidate->package_root), "%s",
        directory);
    candidate->format = BONGO_CAT_IMPORT_TAURI;
    char parent[BONGO_CAT_PATH_CAP];
    if ((!has_cover_asset(directory) || !has_background_asset(directory)) &&
        bongo_cat_import_parent_path(directory, parent, sizeof(parent)) &&
        has_preview_assets(parent))
        snprintf(candidate->assets, sizeof(candidate->assets), "%s", parent);
    if (!tauri_resource_mode(candidate->directory, candidate->assets,
            &candidate->mode, &candidate->gamepad_buttons)) {
        candidate->mode = import_mode(candidate->directory);
        candidate->gamepad_buttons = candidate->mode == BONGO_CAT_MODE_GAMEPAD;
    }
    return true;
}
