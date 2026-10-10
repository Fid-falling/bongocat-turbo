/* 模型目录的字节级操作：递归删除、导入残留清理、递归复制。三者都由导入锁保护，被
 * model_catalog_* 与导入流程调用。递归深度都有上限，因为模型包里的 junction 或符号链接
 * 会形成环。删除是后序（先子后父），复制是前序（先建目录再枚举）；失败时可能留下半套目标
 * 目录，由调用方在下一轮扫描或清理时收拾。 */
#include "model_storage.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 递归深度上限（删除与复制各一份）：正常模型包只有几层，超过 32 层基本可以断定是目录环路
 * 或恶意构造，直接报 FORMAT 而不是继续递归。 */
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

/* 后序删除：是目录就先枚举子项（visitor 负责递归），再删目录本身 —— Windows 的
 * RemoveDirectory 拒绝非空目录。路径不存在视为已删除（幂等），便于失败重试。深度超限或任
 * 一子项失败立即中止并保留 error，已经删掉的部分不会回滚。 */
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

/* 外部入口，深度从 0 起算；调用方需持有导入锁，删除期间不能让 worker 同时枚举目录。 */
bool bongo_cat_model_remove_tree(const char *path, BongoCatError *error) {
    return remove_tree(path, 0, error);
}

/* 只认导入中途的暂存目录名 .import-<十六进制摘要>-<序号>.tmp（由导入流程生成）。这个判定
 * 直接决定 cleanup_imports 删什么，必须匹配得足够严，放宽一点就会删到用户目录。前缀已保
 * 证名字长度至少 8，所以下面从尾部回退取后缀不会越界。 */
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

/* 清理 root 下的导入残留：只处理匹配 temporary_name 的目录，其它条目静默跳过。root 不存在
 * 或不是目录时返回 false（而不是无事可做），由调用方决定要不要告警。 */
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

/* 逐个复制：目录递归，文件走 bongo_cat_path_copy_file（会覆盖同名目标）。通用错误信息只在
 * 还没有更具体原因时才补，保留最内层的诊断。 */
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

/* 前序遍历复制：先建目标目录（bongo_cat_path_enumerate 在 Windows 上把空目录当成成功，空
 * 目录也能正确复制），再枚举源目录。目标已存在时创建目录是幂等的，所以复制可以覆盖重跑。 */
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

/* 公开入口：源不是目录返回 ARGUMENT；失败时把嵌套太深单独映射成 FORMAT，其余归 IO，让调用
 * 方能区分路径问题与磁盘问题。 */
BongoCatResult bongo_cat_model_copy_directory(const char *source,
    const char *target, BongoCatError *error) {
    if (!source || !target || !bongo_cat_path_is_dir(source))
        return BONGO_CAT_ERROR_ARGUMENT;
    return copy_tree(source, target, 0, error) ? BONGO_CAT_OK :
        error && error->code == BONGO_CAT_ERROR_FORMAT ?
        BONGO_CAT_ERROR_FORMAT : BONGO_CAT_ERROR_IO;
}
