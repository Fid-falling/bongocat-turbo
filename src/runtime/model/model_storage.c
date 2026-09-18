#include "model_storage.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MODEL_REMOVE_DEPTH_CAP 32
#define MODEL_COPY_DEPTH_CAP 32

typedef struct RemoveContext { BongoCatError *error; unsigned depth; } RemoveContext;
static bool remove_tree(const char *path, unsigned depth, BongoCatError *error);

static BongoCatPathVisit remove_item(void *userdata,
    const char *dirname, const char *name) {
    RemoveContext *context = userdata;
    char path[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(path, sizeof(path), dirname, name) &&
        remove_tree(path, context->depth + 1, context->error)
        ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
}

static bool remove_tree(const char *path, unsigned depth, BongoCatError *error) {
    if (!path || !path[0]) return true;
    bool directory = bongo_cat_path_is_dir(path);
    if (!directory && !bongo_cat_path_is_file(path)) return true;
    if (depth > MODEL_REMOVE_DEPTH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Model directory nesting exceeds %u levels", MODEL_REMOVE_DEPTH_CAP);
        return false;
    }
    RemoveContext context = {error, depth};
    if (directory &&
        !bongo_cat_path_enumerate(path, remove_item, &context)) return false;
    if (bongo_cat_path_remove(path)) return true;
    if (error) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot remove %s: %s", path, SDL_GetError());
    return false;
}

bool bongo_cat_model_remove_tree(const char *path, BongoCatError *error) {
    return remove_tree(path, 0, error);
}

static bool temporary_name(const char *name) {
    static const char prefix[] = ".import-", suffix[] = ".tmp";
    if (!name || strncmp(name, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *cursor = name + sizeof(prefix) - 1;
    const char *dash = strchr(cursor, '-');
    if (!dash || dash == cursor) return false;
    for (const char *value = cursor; value < dash; ++value)
        if (!isxdigit((unsigned char)*value)) return false;
    cursor = dash + 1;
    const char *end = name + strlen(name) - (sizeof(suffix) - 1);
    if (end <= cursor || strcmp(end, suffix) != 0) return false;
    for (const char *value = cursor; value < end; ++value)
        if (!isdigit((unsigned char)*value)) return false;
    return true;
}

static BongoCatPathVisit cleanup_item(void *userdata,
    const char *dirname, const char *name) {
    BongoCatError *error = userdata;
    if (!temporary_name(name)) return BONGO_CAT_PATH_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), dirname, name) ||
        !bongo_cat_path_is_dir(path)) return BONGO_CAT_PATH_CONTINUE;
    return bongo_cat_model_remove_tree(path, error)
        ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
}

bool bongo_cat_model_cleanup_imports(const char *root, BongoCatError *error) {
    return root && bongo_cat_path_is_dir(root) &&
        bongo_cat_path_enumerate(root, cleanup_item, error);
}

typedef struct CopyContext {
    const char *source;
    const char *target;
    BongoCatError *error;
    unsigned depth;
} CopyContext;

static bool copy_tree(const char *source, const char *target, unsigned depth,
    BongoCatError *error);

static BongoCatPathVisit copy_item(void *userdata,
    const char *dirname, const char *name) {
    (void)dirname;
    CopyContext *context = userdata;
    char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(source, sizeof(source), context->source, name) ||
        !bongo_cat_path_join(target, sizeof(target), context->target, name)) {
        bongo_cat_error_set(context->error, BONGO_CAT_ERROR_IO,
            "Model path is too long");
        return BONGO_CAT_PATH_FAILURE;
    }
    bool directory = bongo_cat_path_is_dir(source);
    bool file = !directory && bongo_cat_path_is_file(source);
    bool ok = directory ? copy_tree(source, target, context->depth + 1,
        context->error) : file && bongo_cat_path_copy_file(source, target);
    if (!ok && context->error && !context->error->message[0])
        bongo_cat_error_set(context->error, BONGO_CAT_ERROR_IO,
            "Cannot copy %s: %s", source, SDL_GetError());
    return ok ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
}

static bool copy_tree(const char *source, const char *target, unsigned depth,
    BongoCatError *error) {
    if (depth > MODEL_COPY_DEPTH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Model directory nesting exceeds %u levels", MODEL_COPY_DEPTH_CAP);
        return false;
    }
    if (!bongo_cat_path_create_directory(target)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot create %s: %s", target, SDL_GetError());
        return false;
    }
    CopyContext context = {source, target, error, depth};
    return bongo_cat_path_enumerate(source, copy_item, &context);
}

BongoCatResult bongo_cat_model_copy_directory(const char *source,
    const char *target, BongoCatError *error) {
    if (!source || !target || !bongo_cat_path_is_dir(source))
        return BONGO_CAT_ERROR_ARGUMENT;
    return copy_tree(source, target, 0, error) ? BONGO_CAT_OK :
        error && error->code == BONGO_CAT_ERROR_FORMAT ?
        BONGO_CAT_ERROR_FORMAT : BONGO_CAT_ERROR_IO;
}
