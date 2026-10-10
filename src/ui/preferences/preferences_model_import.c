/*
 * 导入结束后的用户可见结果 (由 complete_job 在工作线程结束后于主线程调用)。
 *
 * 这个函数是"导入"这条链路的唯一出口: 提示文字、失败清单、冒烟退出码、重绘都在这里
 * 定稿, 上面的 worker 只管干活不碰 UI。
 * 两条特殊约定:
 * - 失败清单只列前 N 个, 剩下的用 "and N more" 概括 —— 一次拖进 200 个包时
 * 全列出来会把通知撑爆;
 * - app->smoke 下不弹通知, 而是把失败转成 exit_code=1, 让 CI 能判定。
 * 末尾主动 render 一次: 导入是在事件流之外结束的, 不主动画这一帧, 结果提示要等到
 * 用户下一次操作才出现。
 */
#include "preferences_state.h"
#include "preferences_notice.h"
#include "model_import.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <string.h>

static const char *tr(BongoCatApp *app, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/*
 * 把结果码翻成人话。CUBISM 那一档单独列出, 因为它是"装上了但显示不出来" ——
 * 和"没装上"是完全不同的两件事, 提示必须说清楚。
 */
static const char *import_failure_message(BongoCatApp *app,
    BongoCatResult result) {
    switch (result) {
    case BONGO_CAT_ERROR_ARGUMENT:
        return tr(app, "pages.preference.model.hints.importInvalidSource",
            "The selected source no longer exists or cannot be used");
    case BONGO_CAT_ERROR_FORMAT:
        return tr(app, "pages.preference.model.hints.importInvalidFormat",
            "No supported model was found, or the model package is incomplete");
    case BONGO_CAT_ERROR_IO:
        return tr(app, "pages.preference.model.hints.importFileAccess",
            "The model files could not be read or saved");
    case BONGO_CAT_ERROR_MEMORY:
        return tr(app, "pages.preference.model.hints.importOutOfMemory",
            "There is not enough memory to import this model");
    case BONGO_CAT_ERROR_CUBISM:
        return tr(app, "pages.preference.model.hints.importLoadFailed",
            "The model was imported, but it could not be displayed");
    default:
        return tr(app, "pages.preference.model.hints.importFailed",
            "Model import failed. Check the selected files and try again");
    }
}

void bongo_cat_preferences_import_complete(BongoCatApp *app,
    BongoCatResult result, const BongoCatError *error, size_t resolved_count,
    size_t installed_count, size_t succeeded_count, size_t failed_count,
    const char (*failed_names)[BONGO_CAT_ID_CAP], size_t failed_name_count) {
    if (!app || !app->preferences) return;
    bool failed = failed_count > 0 || result != BONGO_CAT_OK;
    char batch_message[384];
    char failure_list[220] = "";
    char more_failures[64] = "";
    size_t shown_names = failed_names && failed_name_count >
        BONGO_CAT_IMPORT_FAILURE_NAME_CAP ? BONGO_CAT_IMPORT_FAILURE_NAME_CAP
        : failed_names ? failed_name_count : 0;
    for (size_t i = 0; i < shown_names; ++i) {
        size_t used = strlen(failure_list);
        snprintf(failure_list + used, sizeof(failure_list) - used,
            "%s%s", i ? ", " : "", failed_names[i]);
    }
    /*
     * failed_count 是"失败总数", failed_names 是"记得住名字的那部分", 两者可能不等。
     * listed_count 记录真正列出来的条数, 只用来算 "and N more" 里的 N;
     * 一条名字都没有但确实有失败时, 补一个 "unknown source" 占位, 否则提示会变成
     * "失败 3 个, 失败的模型: " 这种半截话。
     */
    size_t listed_count = shown_names;
    if (!failure_list[0]) {
        snprintf(failure_list, sizeof(failure_list), "%s",
            tr(app, "pages.preference.model.hints.importUnknownSource",
                "unknown source"));
        listed_count = failed_count ? 1 : 0;
    }
    if (failed_count > listed_count)
        snprintf(more_failures, sizeof(more_failures), tr(app,
            "pages.preference.model.hints.importMoreFailures",
            " and %zu more"), failed_count - listed_count);
    /*
     * 三种措辞分开: 装上了新包 / 只有同名已存在的包 / 部分失败。
     * error 的 code 优先于结果码, 因为它带着更具体的失败原因。
     */
    const char *message = installed_count ? tr(app,
            "pages.preference.model.hints.importSuccess", "Model imported")
        : tr(app, "pages.preference.model.hints.importExists",
            "Model already exists");
    if (failed) {
        BongoCatResult failure = error && error->code != BONGO_CAT_OK
            ? error->code : result;
        snprintf(batch_message, sizeof(batch_message), tr(app,
            "pages.preference.model.hints.importBatchResult",
            "Import succeeded: %zu; failed: %zu. Failed models: %s%s. First failure: %s"),
            succeeded_count, failed_count, failure_list, more_failures,
            import_failure_message(app, failure));
        message = batch_message;
    }
    if (failed && error && error->message[0])
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Model import failed: %s", error->message);
    if (app->smoke) {
        if (failed) app->exit_code = 1;
    } else bongo_cat_preferences_notice_show(app, message, failed);
    if (result == BONGO_CAT_OK)
        SDL_Log("Resolved %zu model package(s); installed %zu new package(s)",
            resolved_count, installed_count);
    bongo_cat_preferences_invalidate(app->preferences);
    bongo_cat_preferences_render(app->preferences);
}
