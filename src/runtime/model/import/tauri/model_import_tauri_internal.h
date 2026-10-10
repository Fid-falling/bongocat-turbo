/*
 * Tauri 适配器的内部契约: 候选构造、源包资源查找、以及 Mver 归一化各步骤的签名。
 * 只有 tauri/ 目录下的 .c 会 include 它, 外部一律走 model_import_tauri.h。
 * 这些声明共同维护一条不变量: 转换产物在磁盘上的形态必须与 Mver 规范包一致,
 * 因为安装流程会把整个结果目录再交给 bongo_cat_import_discover 重新识别一遍。
 */
#ifndef BONGO_CAT_MODEL_IMPORT_TAURI_INTERNAL_H
#define BONGO_CAT_MODEL_IMPORT_TAURI_INTERNAL_H

#include "model_import_tauri.h"
#include "../model_import_path.h"
#include "bongo_cat/path.h"

/* 每组按键图的数量上限, 同时是 config.json 矩阵的行数上限 (见 model_import_tauri_input.c);
 * BONGO_CAT_TAURI_SOURCE_FILE 是源包自己的标定清单, 只读, 不写进产物。 */
#define TAURI_KEY_CAP 256
#define BONGO_CAT_TAURI_SOURCE_FILE "bongocat.mver.json"

/* 写进 config.json decoration 段的投影标定。l2d_correct 是投影缩放 (Mver 默认 1.1),
 * l2d_offset_* 直接进 Cubism 投影矩阵的 translate, window_* 同时是参考分辨率。 */
typedef struct TauriMverCalibration {
    double l2d_correct;
    double l2d_offset_x;
    double l2d_offset_y;
    int window_width;
    int window_height;
    bool mirror;
    bool auto_frame;
} TauriMverCalibration;

typedef struct TauriKeyFile {
    char name[BONGO_CAT_ID_CAP];
    char path[BONGO_CAT_PATH_CAP];
    int code;
} TauriKeyFile;

/* 顺序有语义: 排好序之后的下标就是 <group>/<index>.png 的编号, 也是矩阵的行号。 */

typedef struct TauriKeyFiles {
    TauriKeyFile values[TAURI_KEY_CAP];
    size_t count;
} TauriKeyFiles;

/* Candidate construction is private to Tauri discovery. */
bool bongo_cat_import_tauri_add_candidate(BongoCatImportDiscovery *discovery,
    const char *directory, const char *setting);

/* Source package lookup. */
bool bongo_cat_tauri_find_resource_file(
    const BongoCatImportCandidate *candidate, const char *name,
    char *path, size_t capacity);
bool bongo_cat_tauri_find_resource_directory(
    const BongoCatImportCandidate *candidate, const char *name,
    char *path, size_t capacity);
bool bongo_cat_tauri_find_package_file(
    const BongoCatImportCandidate *candidate, const char *name,
    char *path, size_t capacity);

/* Mver normalization steps. */
int bongo_cat_tauri_key_code(const char *filename);
void bongo_cat_tauri_calibration_defaults(TauriMverCalibration *calibration);
void bongo_cat_tauri_legacy_calibration(
    const BongoCatImportCandidate *candidate, TauriMverCalibration *calibration);
bool bongo_cat_tauri_copy_image_or_placeholder(const char *source,
    const char *target, BongoCatError *error);
bool bongo_cat_tauri_copy_preview(const BongoCatImportCandidate *candidate,
    const char *mode_root, BongoCatError *error);
bool bongo_cat_tauri_copy_runtime_images(
    const BongoCatImportCandidate *candidate, const char *mode_root,
    BongoCatError *error);
bool bongo_cat_tauri_copy_input_images(
    const BongoCatImportCandidate *candidate,
    const char *mode_root, TauriKeyFiles *left, TauriKeyFiles *right,
    BongoCatError *error);
bool bongo_cat_tauri_write_config(const char *path, BongoCatModelMode mode,
    const TauriKeyFiles *left, const TauriKeyFiles *right,
    const TauriMverCalibration *calibration, BongoCatError *error);
bool bongo_cat_tauri_copy_model_tree(const char *source, const char *target,
    unsigned depth, BongoCatError *error);
bool bongo_cat_tauri_read_calibration(
    const BongoCatImportCandidate *candidate,
    TauriMverCalibration *calibration);

#endif
