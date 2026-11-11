/*
 * 给"没有封面"的模型找一张能看的预览图。
 *
 * 背景: cover.png 是运行时渲染当前模型再回读生成的, 只对"被加载过"的模型存在。用户批量导入
 * 十几个模型后, 没逐个切换过的那批就没有封面 —— 轮盘和设置卡片只能画一个通用图标, 看起来
 * 就是"我的模型没显示全"。
 *
 * 兜底顺序:
 *   1) <dir>/resources/cover.png        —— 正式封面
 *   2) 各个 model3.json 的 FileReferences.Textures[0] —— 模型自己的贴图 (导入包里通常有
 *      好几份 model3.json, 有的指向已经不存在的旧图集, 所以要把候选都试一遍)
 *   3) 任意 texture_*.png
 *   4) 任意 png (跳过 resources/ 里的 UI 资源)
 */
#include "model_preview.h"

#include "model_cover_paths.h"

#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <yyjson.h>

#include <stdio.h>
#include <string.h>

#define PREVIEW_MAX_DEPTH 4
#define PREVIEW_MAX_DIRS 96
#define PREVIEW_MAX_MANIFESTS 6
#define PREVIEW_MAX_PNGS 6

typedef struct PreviewSearch {
    char directories[PREVIEW_MAX_DIRS][BONGO_CAT_PATH_CAP];
    int depths[PREVIEW_MAX_DIRS];
    int count;
    int cursor;
    int current_depth;
    char manifests[PREVIEW_MAX_MANIFESTS][BONGO_CAT_PATH_CAP];
    int manifest_count;
    char textures[PREVIEW_MAX_PNGS][BONGO_CAT_PATH_CAP];
    int texture_count;
    char pngs[PREVIEW_MAX_PNGS][BONGO_CAT_PATH_CAP];
    int png_count;
} PreviewSearch;

static bool ends_with_ci(const char *value, const char *suffix) {
    const size_t length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    if (length < suffix_length) return false;
    return SDL_strcasecmp(value + length - suffix_length, suffix) == 0;
}

static bool starts_with_texture(const char *name) {
    return SDL_strncasecmp(name, "texture", 7) == 0;
}

static SDL_EnumerationResult SDLCALL on_entry(void *userdata,
    const char *directory, const char *name) {
    PreviewSearch *search = userdata;
    if (!directory || !name || !name[0] || name[0] == '.') return SDL_ENUM_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), directory, name))
        return SDL_ENUM_CONTINUE;
    if (bongo_cat_path_is_dir(path)) {
        if (search->current_depth + 1 <= PREVIEW_MAX_DEPTH &&
                search->count < PREVIEW_MAX_DIRS) {
            const int index = search->count++;
            snprintf(search->directories[index], sizeof(search->directories[index]),
                "%s", path);
            search->depths[index] = search->current_depth + 1;
        }
        return SDL_ENUM_CONTINUE;
    }
    if (ends_with_ci(name, ".model3.json")) {
        if (search->manifest_count < PREVIEW_MAX_MANIFESTS)
            snprintf(search->manifests[search->manifest_count++],
                sizeof(search->manifests[0]), "%s", path);
        return SDL_ENUM_CONTINUE;
    }
    if (!ends_with_ci(name, ".png")) return SDL_ENUM_CONTINUE;
    if (strstr(path, "resources") != NULL) return SDL_ENUM_CONTINUE;
    if (starts_with_texture(name) && search->texture_count < PREVIEW_MAX_PNGS) {
        snprintf(search->textures[search->texture_count++],
            sizeof(search->textures[0]), "%s", path);
        return SDL_ENUM_CONTINUE;
    }
    if (search->png_count < PREVIEW_MAX_PNGS)
        snprintf(search->pngs[search->png_count++], sizeof(search->pngs[0]), "%s",
            path);
    return SDL_ENUM_CONTINUE;
}

/* 从一份 model3.json 里取第一张贴图; 路径相对该 json 所在目录。 */
static bool texture_from_manifest(const char *manifest, char *output,
    size_t capacity) {
    yyjson_doc *doc = yyjson_read_file(manifest, 0, NULL, NULL);
    if (!doc) return false;
    bool found = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *files = yyjson_obj_get(root, "FileReferences");
    yyjson_val *textures = files ? yyjson_obj_get(files, "Textures") : NULL;
    yyjson_val *first = textures ? yyjson_arr_get_first(textures) : NULL;
    const char *relative =
        first && yyjson_is_str(first) ? yyjson_get_str(first) : NULL;
    if (relative) {
        char directory[BONGO_CAT_PATH_CAP];
        char relative_windows[BONGO_CAT_PATH_CAP];
        /* 清单里的相对路径用 '/', 统一成 '\\' 再和 json 所在目录拼 */
        snprintf(relative_windows, sizeof(relative_windows), "%s", relative);
        for (char *cursor = relative_windows; *cursor; ++cursor)
            if (*cursor == '/') *cursor = '\\';
        snprintf(directory, sizeof(directory), "%s", manifest);
        char *separator = strrchr(directory, '\\');
        if (!separator) separator = strrchr(directory, '/');
        if (separator) *separator = '\0';
        if (separator &&
            bongo_cat_path_join(output, capacity, directory, relative_windows) &&
            bongo_cat_path_is_file(output))
            found = true;
    }
    yyjson_doc_free(doc);
    return found;
}

bool bongo_cat_model_preview_path(const char *model_directory, char *output,
    size_t capacity) {
    if (!model_directory || !output || capacity == 0) return false;
    output[0] = '\0';
    char cover[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(cover, sizeof(cover), model_directory,
            BONGO_CAT_MODEL_COVER_FILE) && bongo_cat_path_is_file(cover)) {
        snprintf(output, capacity, "%s", cover);
        return true;
    }

    PreviewSearch search;
    memset(&search, 0, sizeof(search));
    snprintf(search.directories[0], sizeof(search.directories[0]), "%s",
        model_directory);
    search.count = 1;
    while (search.cursor < search.count) {
        const int index = search.cursor++;
        search.current_depth = search.depths[index];
        SDL_EnumerateDirectory(search.directories[index], on_entry, &search);
    }

    for (int i = 0; i < search.manifest_count; ++i)
        if (texture_from_manifest(search.manifests[i], output, capacity))
            return true;
    if (search.texture_count > 0) {
        snprintf(output, capacity, "%s", search.textures[0]);
        return true;
    }
    if (search.png_count > 0) {
        snprintf(output, capacity, "%s", search.pngs[0]);
        return true;
    }
    return false;
}
