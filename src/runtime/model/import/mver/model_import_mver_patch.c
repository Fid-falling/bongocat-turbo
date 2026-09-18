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

static bool patch_shape(const char *root) {
    char mode[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(mode, sizeof(mode), root, "standard") &&
        child_file(mode, "hand", "0.png")) return true;
    return bongo_cat_path_join(mode, sizeof(mode), root, "gamepad") &&
        child_file(mode, "lefthand", "0.png") &&
        child_file(mode, "righthand", "0.png");
}

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
