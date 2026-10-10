/* Mver 包的落盘复制入口 (实现在 model_import_mver_copy.c): 把来源包搬进 target 并把 installed 的路径字段
 * 改写成安装位置。只接受 MVER 与 MVER_PATCH 两种格式, 三个参数都必须非空; 返回 false 时 target 下是半成品,
 * 由调用方的临时目录语义丢弃。 */
#ifndef BONGO_CAT_MODEL_IMPORT_MVER_COPY_H
#define BONGO_CAT_MODEL_IMPORT_MVER_COPY_H

#include "model_import_mver.h"

bool bongo_cat_import_mver_copy_package(
    const BongoCatImportCandidate *candidate, const char *target,
    BongoCatImportCandidate *installed, BongoCatError *error);

#endif
