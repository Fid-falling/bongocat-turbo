/*
 * 更新检查失败文案的本地化: 把网络层写出的英文原文翻成界面能直接显示的人话。
 *
 * 为什么要单独一层: update_http.c / update_http_unix.c 拿到非 200 时只有状态码这一个
 * 事实, 它们把那句英文原文交给上层, 顺便留给日志排查用。界面不能直接显示它 ——
 * 用户看到 "GitHub returned HTTP status 403" 既不知道发生了什么, 也不知道能做什么。
 * 其中 403/429 是 GitHub 匿名接口限流(每 IP 每小时 60 次), 必须单独给一句
 * "稍后重试 + 也可以直接打开 release 页面" 的出路, 而不是笼统的"检查失败"。
 *
 * 原始状态码不进界面, 但也不会丢: 调用方(update_service.c 的 complete)记录的是原文,
 * 日志里始终能看到 "GitHub returned HTTP status 403"。
 */
#include "bongo_cat/update.h"

#include <stdlib.h>
#include <string.h>

/* 与 update_http.c / update_http_unix.c 写出的文案严格一致; 改那两处就要改这里,
   否则前缀对不上, 界面会退回英文原文。 */
#define UPDATE_HTTP_STATUS_PREFIX "GitHub returned HTTP status "

/* 只认"前缀 + 纯数字"这一种形态: 后面还有别的内容就当作识别不出来, 退回原文,
   免得把半截数字当成状态码显示成另一句不相干的文案。0 表示"没识别到"。 */
static unsigned long http_status(const char *raw) {
    if (!raw || strncmp(raw, UPDATE_HTTP_STATUS_PREFIX,
            sizeof(UPDATE_HTTP_STATUS_PREFIX) - 1) != 0) return 0;
    char *end = NULL;
    unsigned long status = strtoul(raw + sizeof(UPDATE_HTTP_STATUS_PREFIX) - 1,
        &end, 10);
    return end && *end == '\0' ? status : 0;
}

/* 把 raw 翻成可直接显示的本地化文案, 返回 i18n 内部的借用指针或 raw 本身, 不要 free。
 * 403 与 429 对用户是同一件事(接口被限流), 共用一句文案; 其余状态码走通用文案。
 * 两个键都不带占位符: 状态码只留在日志里, 界面只显示人话。
 * i18n 为空(极早期失败)时 bongo_cat_i18n_get 会回退英文兜底; raw 也为空则提示
 * "无法检查更新", 调用方不必再判空。 */
const char *bongo_cat_update_error_text(const BongoCatI18n *i18n,
    const char *raw) {
    unsigned long status = http_status(raw);
    if (!status)
        return raw && raw[0] ? raw : bongo_cat_i18n_get(i18n,
            "native.support.updateFailed", "Unable to check for updates");
    if (status == 403 || status == 429)
        return bongo_cat_i18n_get(i18n, "native.support.updateRateLimited",
            "GitHub is temporarily limiting update checks. Try again later, "
            "or open the releases page.");
    return bongo_cat_i18n_get(i18n, "native.support.updateHttpFailed",
        "GitHub did not respond normally. Try again later, or open the "
        "releases page.");
}
