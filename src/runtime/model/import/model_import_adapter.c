/*
 * 把候选包装配成"运行时适配目录"(nearby 缓存与正式导入共用同一条流程):
 * 预览图 -> Mver 素材与贴图 -> 适配元数据(.bongo-cat-adapter.json) -> 导入报告 ->
 * 模式标记(.bongo-cat-mode)。步骤顺序有依赖: 元数据与报告会引用前面步骤写出的文件,
 * 因此下面的步骤表不能重排。函数本身不保证原子性, 由调用方给的临时目录兜底。
 */
#include "model_import.h"
#include "mver/model_import_mver.h"
#include "model_storage.h"
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>

static bool copy_optional(const char *source_dir, const char *source_name,
    const char *target_dir, const char *target_name) {
    char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(source, sizeof(source), source_dir, source_name) &&
        bongo_cat_path_join(target, sizeof(target), target_dir, target_name) &&
        bongo_cat_path_copy_file(source, target);
}

/* 等价候选名列表的复制助手: 一个都不存在时返回 true(不算错), 存在但复制失败才算错。
   copy_file 对缺失的源文件同样报失败, 所以"源文件被删掉"不会被静默忽略。 */
static bool copy_first(const char *source_dir, const char *const *names,
    size_t count, const char *target_dir, const char *target_name) {
    for (size_t i = 0; i < count; ++i) {
        char source[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(source, sizeof(source), source_dir, names[i]) ||
            !bongo_cat_path_is_file(source)) continue;
        return copy_optional(source_dir, names[i], target_dir, target_name);
    }
    return true;
}

static bool copy_preview_file(const char *source_resources,
    const char *source_root, const char *const *names, size_t count,
    const char *target_resources, const char *target_name) {
    for (size_t i = 0; i < count; ++i) {
        char source[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(source, sizeof(source), source_resources,
                names[i]) && bongo_cat_path_is_file(source))
            return copy_optional(source_resources, names[i], target_resources,
                target_name);
    }
    return copy_first(source_root, names, count, target_resources, target_name);
}

static bool preview_file_exists(const char *directory, const char *name) {
    char path[BONGO_CAT_PATH_CAP];
    return bongo_cat_path_join(path, sizeof(path), directory, name) &&
        bongo_cat_path_is_file(path);
}

/* 预览图源优先 resources/ 子目录, 找不到再退回包根; 目标 resources 目录要么整体复制源
   目录, 要么单独创建。cover 与 background 只在目标侧缺失时才补, 因此重复调用不会覆盖
   已经生成好的预览图。 */
static bool copy_preview(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error) {
    char source_resources[BONGO_CAT_PATH_CAP], target_resources[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(source_resources, sizeof(source_resources),
        candidate->assets, "resources") ||
        !bongo_cat_path_join(target_resources, sizeof(target_resources), target,
            "resources")) return false;
    bool target_exists = bongo_cat_path_is_dir(target_resources);
    if (bongo_cat_path_is_dir(source_resources) && !target_exists) {
        if (bongo_cat_model_copy_directory(source_resources, target_resources,
                error) != BONGO_CAT_OK) return false;
        target_exists = true;
    }
    if (!target_exists && !bongo_cat_path_create_directory(target_resources))
        return false;
    const char *covers[] = {
        "cover.png", "cat.png", "bg.png", "mousebg.png", "tabletbg.png"};
    const char *backgrounds[] = {
        "background.png", "bg.png", "mousebg.png", "tabletbg.png"};
    bool ok = (preview_file_exists(target_resources, "cover.png") ||
        copy_preview_file(source_resources, candidate->assets, covers, 5,
            target_resources, "cover.png")) &&
        (preview_file_exists(target_resources, "background.png") ||
        copy_preview_file(source_resources, candidate->assets, backgrounds, 4,
            target_resources, "background.png"));
    if (!ok) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot copy model preview assets: %s", SDL_GetError());
    return ok;
}

/* 写 .bongo-cat-mode(内容只能是 keyboard/gamepad/standard): 它是目录扫描推断模型模式
   时优先级最高的一级来源, 写错会让模型套用错误的按键布局。 */
static bool write_mode(const char *target, BongoCatModelMode mode,
    BongoCatError *error) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), target, ".bongo-cat-mode"))
        return false;
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return false;
    const char *name = mode == BONGO_CAT_MODE_KEYBOARD ? "keyboard" :
        mode == BONGO_CAT_MODE_GAMEPAD ? "gamepad" : "standard";
    bool ok = fputs(name, file) >= 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot write imported model metadata");
    return ok;
}

/* 前提: candidate 与 target 非空, target 不存在时会被创建。步骤表按数组顺序执行且
   后一步依赖前一步产物; 已写入的文件不在这里回滚, 交由调用方的临时目录语义处理。 */
bool bongo_cat_import_prepare_adapter(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error) {
    if (!candidate || !target || (!bongo_cat_path_is_dir(target) &&
        !bongo_cat_path_create_directory(target))) return false;
    /* 每一步单独判断, 失败时把步骤名写进日志 —— 否则只会看到一句
       "import failed", 无法定位是预览图、素材转换还是元数据出错。 */
    struct { const char *name; bool (*run)(const BongoCatImportCandidate *,
        const char *, BongoCatError *); } steps[] = {
        {"preview", copy_preview},
        {"mver-assets", bongo_cat_import_mver_assets},
        {"metadata", bongo_cat_import_adapter_metadata},
        {"report", bongo_cat_import_write_report}
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (steps[i].run(candidate, target, error)) continue;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Model import adapter step '%s' failed: %s (source=%s)",
            steps[i].name, error && error->message[0] ? error->message : "unknown",
            candidate->assets);
        return false;
    }
    /* 模式标记单独一步: 放最后写, 避免素材步骤失败时留下一个已经被标记模式的半成品目录。 */
    if (write_mode(target, candidate->mode, error)) return true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Model import adapter step 'mode' failed: %s",
        error && error->message[0] ? error->message : "unknown");
    return false;
}
