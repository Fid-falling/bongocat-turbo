/* Tauri 源格式适配器的对外入口: 精确发现、递归发现、转换成 Mver 规范包。
 * 转换产物必须能被 Mver 适配器直接接管 (img/<mode>/ + config.json); 发现函数的
 * 返回值是 1 = 命中、0 = 未命中、负值 = 出错, 调用方不能把 0 当失败处理。
 */

#ifndef BONGO_CAT_MODEL_IMPORT_TAURI_H
#define BONGO_CAT_MODEL_IMPORT_TAURI_H

#include "../model_import.h"

/* Source-format adapter. Conversion output must be a canonical Mver package. */
int bongo_cat_import_tauri_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
int bongo_cat_import_tauri_discover_recursive(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
bool bongo_cat_import_tauri_convert_to_mver(
    const BongoCatImportCandidate *candidate, const char *target,
    BongoCatImportCandidate *installed, BongoCatError *error);

#endif
