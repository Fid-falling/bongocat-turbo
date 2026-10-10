#include "bongo_cat/common.h"

#include <stdarg.h>
#include <stdio.h>

/*
 * 全项目统一的错误记录入口。
 *
 * 约定: error 可以为 NULL(调用方不关心细节), format 也可以为 NULL(只记错误码);
 * message 一定是 NUL 结尾的字符串, 但超出缓冲区的部分会被 vsnprintf 截断,
 * 所以它只适合展示和写日志, 不要拿 message 的内容做逻辑判断。
 */
void bongo_cat_error_set(BongoCatError *error, BongoCatResult code, const char *format, ...) {
    if (!error) return;
    error->code = code;
    if (!format) {
        error->message[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}
