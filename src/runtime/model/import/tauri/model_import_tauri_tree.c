/*
 * 规范包里的 Live2D 资源整树拷贝: 把源包目录 (model3.json、贴图、动作、物理等) 原样搬
 * 进 target/img/<mode>/cat_model, 只丢掉会被其它步骤重新生成的界面文件。
 * 过滤分两层: resources/ 的直接子项走 interface_resource 名单, 更深的层级按模型资源
 * 原样保留 (所以同名图片放在子目录里不会被误删)。
 * 深度上限 32 是为了挡住目录环 (符号链接/junction) 或异常深的树, 超限按失败处理。
 */
#include "model_import_tauri_internal.h"

#include <SDL3/SDL.h>

/* 拷贝深度上限: 正常 Tauri 包只有 3~4 层, 超过它基本就是目录环, 直接报错而不是死循环。 */
#define TAURI_MODEL_COPY_DEPTH_CAP 32

typedef struct TauriModelCopy {
    const char *target_root;
    BongoCatError *error;
    unsigned depth;
} TauriModelCopy;

static bool copy_resource_tree(const char *source, const char *target,
    unsigned depth, BongoCatError *error);

/* 会被 input/images 步骤重新生成的名字, 以及源包清单本身 (按键目录、界面图、
 * bongocat.mver.json): 整树拷贝阶段必须跳过, 否则产物里会同时存在新旧两份, 排查时
 * 分不清哪份生效。 */
static bool interface_resource(const char *name) {
    static const char *const files[] = {
        "cover.png", "background.png", "cat.png", "bg.png",
        "mousebg.png", "tabletbg.png", BONGO_CAT_TAURI_SOURCE_FILE
    };
    if (SDL_strcasecmp(name, "left-keys") == 0 ||
        SDL_strcasecmp(name, "right-keys") == 0) return true;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (SDL_strcasecmp(name, files[i]) == 0) return true;
    return false;
}

/* resources/ 顶层的枚举回调: 目录递归下去时改回模型资源语义 (不再做界面名单过滤),
 * 因为 resources/ 下除界面图外还可能有需要原样保留的真实资源。 */
static BongoCatPathVisit copy_resource_child(void *userdata,
    const char *dirname, const char *name) {
    TauriModelCopy *context = userdata;
    if (!name || name[0] == '.' || interface_resource(name))
        return BONGO_CAT_PATH_CONTINUE;
    char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(source, sizeof(source), dirname, name) ||
        !bongo_cat_path_join(target, sizeof(target), context->target_root,
            name)) return BONGO_CAT_PATH_FAILURE;
    if (bongo_cat_path_is_dir(source))
        return bongo_cat_tauri_copy_model_tree(source, target,
            context->depth + 1, context->error)
            ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
    if (!bongo_cat_path_is_file(source)) return BONGO_CAT_PATH_CONTINUE;
    if (!bongo_cat_path_create_directory(context->target_root) ||
        !bongo_cat_path_copy_file(source, target)) {
        bongo_cat_error_set(context->error, BONGO_CAT_ERROR_IO,
            "Cannot preserve Tauri Live2D resource: %s", source);
        return BONGO_CAT_PATH_FAILURE;
    }
    return BONGO_CAT_PATH_CONTINUE;
}

/* 模型目录的枚举回调: depth == 0 时跳过源包清单 (它是源侧元数据, 不该进规范包),
 * resources/ 交给上面那个过滤更严的拷贝, 其余目录与文件原样递归。
 * 注意包根下的 left-keys/right-keys 不在过滤名单里, 会被整目录带进来 —— 它们不参与
 * 归一化 (归一化后的图在 img/<mode>/<group>/), 只是占空间。 */
static BongoCatPathVisit copy_model_child(void *userdata,
    const char *dirname, const char *name) {
    TauriModelCopy *context = userdata;
    if (!name || name[0] == '.' || (context->depth == 0 &&
            SDL_strcasecmp(name, BONGO_CAT_TAURI_SOURCE_FILE) == 0))
        return BONGO_CAT_PATH_CONTINUE;
    char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(source, sizeof(source), dirname, name) ||
        !bongo_cat_path_join(target, sizeof(target), context->target_root,
            name)) return BONGO_CAT_PATH_FAILURE;
    if (context->depth == 0 && bongo_cat_path_is_dir(source) &&
        SDL_strcasecmp(name, "resources") == 0)
        return copy_resource_tree(source, target, context->depth + 1,
            context->error) ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
    if (bongo_cat_path_is_dir(source))
        return bongo_cat_tauri_copy_model_tree(source, target,
            context->depth + 1, context->error)
            ? BONGO_CAT_PATH_CONTINUE : BONGO_CAT_PATH_FAILURE;
    if (!bongo_cat_path_is_file(source) ||
        !bongo_cat_path_copy_file(source, target)) {
        bongo_cat_error_set(context->error, BONGO_CAT_ERROR_IO,
            "Cannot copy Tauri Live2D asset: %s", source);
        return BONGO_CAT_PATH_FAILURE;
    }
    return BONGO_CAT_PATH_CONTINUE;
}

/* 供内部递归调用的入口 (depth 由调用方维护, 转换流程从 0 起)。失败时只补一条通用 IO
 * 错误: 更具体的原因 (哪个文件出错) 已经在回调里写进 error, 这里不覆盖它。 */
bool bongo_cat_tauri_copy_model_tree(const char *source, const char *target,
    unsigned depth, BongoCatError *error) {
    if (depth > TAURI_MODEL_COPY_DEPTH_CAP ||
        !bongo_cat_path_create_directory(target) ||
        !bongo_cat_path_enumerate(source, copy_model_child,
            &(TauriModelCopy){target, error, depth})) {
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_IO, "Cannot copy Tauri Live2D directory: %s",
            source);
        return false;
    }
    return true;
}

static bool copy_resource_tree(const char *source, const char *target,
    unsigned depth, BongoCatError *error) {
    if (depth > TAURI_MODEL_COPY_DEPTH_CAP ||
        !bongo_cat_path_enumerate(source, copy_resource_child,
            &(TauriModelCopy){target, error, depth})) {
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_IO, "Cannot inspect Tauri resource directory: %s",
            source);
        return false;
    }
    return true;
}
