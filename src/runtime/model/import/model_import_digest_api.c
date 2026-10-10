/* 内容指纹的无缓存封装：两个入口都转发到 inspect_cached，区别只在要不要 placeholder 结果。
 * 最近模型（nearby）扫描走这里 —— 它的结果缓存未命中时就会重算整棵资源树；批量导入
 * （session、package index）请自己持有 BongoCatImportDigestCache 再调 inspect_cached。 */
#include "model_import.h"

bool bongo_cat_import_candidate_inspect(const BongoCatImportCandidate *candidate,
    char output[65], bool *placeholder, BongoCatError *error) {
    return bongo_cat_import_candidate_inspect_cached(candidate, output,
        placeholder, NULL, error);
}

bool bongo_cat_import_candidate_digest(const BongoCatImportCandidate *candidate,
    char output[65], BongoCatError *error) {
    return bongo_cat_import_candidate_inspect(candidate, output, NULL, error);
}
