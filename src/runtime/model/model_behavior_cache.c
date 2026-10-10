/* 行为目录缓存: 用户在几个模型之间来回切换时复用已解析的 behavior catalog, 省掉重读
 * model3.json 并拍平动作/表情的代价 (缓存按值拷贝, 惰性分配)。只缓存内容不会自己变的模型:
 * 内置 preset 或带内容摘要的已安装包; nearby (managed) 模型归用户的源目录所有, 一律不缓存。
 */
#include "model_behavior_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 可缓存判据: preset 由应用自己发布, 因此只比 id; 其余必须是非 managed 且带 content_digest,
 * 这样目录内容变了能被 matches 认出来, 不会拿旧结果顶替。
 */
static bool cacheable(const BongoCatModelEntry *entry) {
    /* Installed Mver trees keep a stable content digest. Nearby models are
       managed by their source directory, so do not retain stale data. */
    return entry && (entry->preset || (!entry->managed &&
        entry->content_digest[0]));
}

bool bongo_cat_model_behavior_cache_matches(const BongoCatApp *app,
    const BongoCatModelEntry *entry) {
    return app && app->behavior_cache && app->behavior_cache_valid &&
        cacheable(entry) &&
        strcmp(app->behavior_cache_model_id, entry->id) == 0 &&
        (entry->preset || strcmp(app->behavior_cache_digest,
            entry->content_digest) == 0);
}

void bongo_cat_model_behavior_cache_store(BongoCatApp *app,
    const BongoCatModelEntry *entry) {
    if (!app || !cacheable(entry)) {
        if (app) app->behavior_cache_valid = false;
        return;
    }
    if (!app->behavior_cache)
        app->behavior_cache = malloc(sizeof(*app->behavior_cache));
    if (!app->behavior_cache) {
        app->behavior_cache_valid = false;
        return;
    }
    *app->behavior_cache = app->behaviors;
    snprintf(app->behavior_cache_model_id,
        sizeof(app->behavior_cache_model_id), "%s", entry->id);
    snprintf(app->behavior_cache_digest, sizeof(app->behavior_cache_digest),
        "%s", entry->content_digest);
    app->behavior_cache_valid = true;
}
