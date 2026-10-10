/*
 * Mver 配置 (JSON5) 的标签读取器: 按模式对象 (standard / keyboard / gamepad) 从 config 里取出
 * l2d_expression、l2d_motion、l2d_motion_lockhand、sounds 四个数组, 并把每行旁边的双斜杠注释当作
 * 行为的显示名。适配器生成阶段 (bongo_cat_import_adapter_metadata) 调用, 早于 add_behaviors 与
 * add_audio; 只读文本, 不写文件; 行序号 index 是数组下标 (含被跳过的行), 下游靠它和 Live2D
 * manifest 逐行对齐, 超出 BONGO_CAT_BEHAVIOR_CAP 的标签静默丢弃。
 * 易错点: Mver 的注释语法不是合法 JSON, 这里刻意不引 JSON 库; 改扫描器时必须保证各分支消费到同一位置。
 */
#include "model_import_mver_internal.h"
#include "bongo_cat/file.h"
#include "bongo_cat/utf8.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TextSpan { const char *begin, *end; } TextSpan;

/* cursor 必须指向引号本身; 返回闭合引号之后的位置, 未闭合则返回 end。反斜杠转义只按吃掉两个
   字节处理, 不做解码 —— 上层要的是原文边界。 */
static const char *skip_string(const char *cursor, const char *end) {
    char quote = *cursor++;
    while (cursor < end) {
        if (*cursor == '\\' && cursor + 1 < end) cursor += 2;
        else if (*cursor++ == quote) break;
    }
    return cursor;
}

/* 跳过空白与两种注释, 返回第一个有意义的字符位置。本文件所有扫描都必须经由它, 否则注释文本
   会被当成键或值。 */
static const char *skip_space(const char *cursor, const char *end) {
    for (;;) {
        while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
        if (cursor + 1 >= end || cursor[0] != '/') return cursor;
        if (cursor[1] == '/') {
            cursor += 2;
            while (cursor < end && *cursor != '\n' && *cursor != '\r') cursor++;
        } else if (cursor[1] == '*') {
            cursor += 2;
            while (cursor + 1 < end && !(cursor[0] == '*' && cursor[1] == '/'))
                cursor++;
            if (cursor + 1 < end) cursor += 2;
        } else return cursor;
    }
}

/* 从 cursor 处跳过一个完整的值: 标量走到逗号、方括号或花括号为止, 字符串交给 skip_string,
   容器用括号栈配对。嵌套超过栈容量时直接返回 end —— 宁可把剩余文本整体当成一个值, 也不越界
   写栈。 */
static const char *skip_value(const char *cursor, const char *end) {
    cursor = skip_space(cursor, end);
    if (cursor >= end) return cursor;
    if (*cursor == '"' || *cursor == '\'') return skip_string(cursor, end);
    if (*cursor != '{' && *cursor != '[') {
        while (cursor < end && *cursor != ',' && *cursor != '}' && *cursor != ']')
            cursor++;
        return cursor;
    }
    char stack[64]; size_t depth = 0;
    stack[depth++] = *cursor++ == '{' ? '}' : ']';
    while (cursor < end && depth) {
        if (*cursor == '"' || *cursor == '\'') cursor = skip_string(cursor, end);
        else if (cursor + 1 < end && cursor[0] == '/' && cursor[1] == '/') {
            cursor += 2;
            while (cursor < end && *cursor != '\n' && *cursor != '\r') cursor++;
        } else if (cursor + 1 < end && cursor[0] == '/' && cursor[1] == '*') {
            cursor += 2;
            while (cursor + 1 < end && !(cursor[0] == '*' && cursor[1] == '/'))
                cursor++;
            if (cursor + 1 < end) cursor += 2;
        } else if (*cursor == '{' || *cursor == '[') {
            if (depth >= sizeof(stack)) return end;
            stack[depth++] = *cursor++ == '{' ? '}' : ']';
        } else if (*cursor == stack[depth - 1]) { depth--; cursor++; }
        else cursor++;
    }
    return cursor;
}

static bool key_token(const char **cursor, const char *end, TextSpan *key) {
    const char *start = skip_space(*cursor, end);
    if (start >= end) return false;
    if (*start == '"' || *start == '\'') {
        const char *after = skip_string(start, end);
        if (after <= start + 1 || after[-1] != *start) return false;
        key->begin = start + 1; key->end = after - 1; *cursor = after; return true;
    }
    const char *after = start;
    while (after < end && (isalnum((unsigned char)*after) || *after == '_' ||
        *after == '-')) after++;
    if (after == start) return false;
    key->begin = start; key->end = after; *cursor = after; return true;
}

static bool key_equals(TextSpan key, const char *name) {
    size_t length = (size_t)(key.end - key.begin);
    return strlen(name) == length && memcmp(key.begin, name, length) == 0;
}

/* 在已定位的对象区间里按名字取成员的值区间 (借用 object 内部指针, 生命周期同源文本)。只扫描
   该对象的第一层, 不递归进嵌套对象; 返回的区间不含键名与冒号。 */
static bool member(TextSpan object, const char *name, TextSpan *value) {
    const char *cursor = skip_space(object.begin, object.end);
    if (cursor >= object.end || *cursor++ != '{') return false;
    while ((cursor = skip_space(cursor, object.end)) < object.end && *cursor != '}') {
        TextSpan key;
        if (!key_token(&cursor, object.end, &key)) return false;
        cursor = skip_space(cursor, object.end);
        if (cursor >= object.end || *cursor++ != ':') return false;
        const char *begin = skip_space(cursor, object.end);
        const char *after = skip_value(begin, object.end);
        if (key_equals(key, name)) {
            value->begin = begin; value->end = after; return true;
        }
        cursor = skip_space(after, object.end);
        if (cursor < object.end && *cursor == ',') cursor++;
    }
    return false;
}

/* 在一行绑定里找显示名: 约定是行尾的双斜杠注释, 扫描时先跳过字符串与块注释。命中时把去掉首尾
   空白后的文本写进 output 并返回 true; 没有注释则返回 false 且不写 output。 */
static bool line_label(TextSpan row, char *output, size_t capacity) {
    const char *cursor = row.begin;
    while (cursor + 1 < row.end) {
        if (*cursor == '"' || *cursor == '\'') cursor = skip_string(cursor, row.end);
        else if (cursor[0] == '/' && cursor[1] == '*') {
            cursor += 2;
            while (cursor + 1 < row.end && !(cursor[0] == '*' && cursor[1] == '/'))
                cursor++;
            if (cursor + 1 < row.end) cursor += 2;
        } else if (cursor[0] == '/' && cursor[1] == '/') {
            const char *begin = cursor + 2, *end = begin;
            while (end < row.end && *end != '\n' && *end != '\r') end++;
            while (begin < end && isspace((unsigned char)*begin)) begin++;
            while (end > begin && isspace((unsigned char)end[-1])) end--;
            size_t length = (size_t)(end - begin);
            if (!length) return false;
            /* 容量不足时从尾部回退到整字符边界: 截断点若落在续字节 (高两位为 10) 上, 必须连它的
               首字节一起丢掉, 否则下游的 UTF-8 校验会失败。 */
            if (length >= capacity) {
                length = capacity - 1;
                while (length &&
                    ((unsigned char)begin[length] & 0xc0) == 0x80) length--;
            }
            memcpy(output, begin, length); output[length] = '\0'; return true;
        } else cursor++;
    }
    return false;
}

/* 收集 mode 对象里 field 数组每一行的标签。index 用数组下标而不是成功计数, 这样被跳过 (无注释、
   编码非法或超容量) 的行不会让下游的 manifest 行号错位; 达到 BONGO_CAT_BEHAVIOR_CAP 之后的行
   静默丢弃。 */
static void collect_field(BongoCatMverLabels *labels, TextSpan mode,
    const char *field) {
    TextSpan rows;
    if (!member(mode, field, &rows)) return;
    const char *cursor = skip_space(rows.begin, rows.end);
    if (cursor >= rows.end || *cursor++ != '[') return;
    size_t index = 0;
    while ((cursor = skip_space(cursor, rows.end)) < rows.end && *cursor != ']') {
        const char *after = skip_value(cursor, rows.end);
        TextSpan row = {cursor, after};
        char label[BONGO_CAT_ID_CAP], normalized[BONGO_CAT_ID_CAP];
        if (line_label(row, label, sizeof(label)) &&
            bongo_cat_utf8_normalize_mver(label, normalized,
                sizeof(normalized)) &&
            labels->count < BONGO_CAT_BEHAVIOR_CAP) {
            BongoCatMverLabelEntry *entry = &labels->entries[labels->count++];
            snprintf(entry->field, sizeof(entry->field), "%s", field);
            snprintf(entry->label, sizeof(entry->label), "%s", normalized);
            entry->index = index;
        }
        index++; cursor = skip_space(after, rows.end);
        if (cursor < rows.end && *cursor == ',') cursor++;
    }
}

/* 读 path (Mver 的 config 文件, 上限 16 MiB, 允许 UTF-8 BOM) 中名为 mode 的对象并填充 labels。
   返回 false 表示读文件失败或没有这个模式对象, 此时 labels 已清零, 可当"无标签"安全使用;
   文件句柄与文本缓冲区都由本函数自行释放。 */
bool bongo_cat_mver_labels_load(const char *path, const char *mode,
    BongoCatMverLabels *labels) {
    if (!path || !mode || !labels) return false;
    memset(labels, 0, sizeof(*labels));
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) { if (file) fclose(file); return false; }
    long length = ftell(file);
    if (length < 0 || length > 16 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    char *text = malloc((size_t)length + 1);
    bool read = text && fread(text, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!read) { free(text); return false; }
    text[length] = '\0';
    const char *begin = text;
    if (length >= 3 && (unsigned char)text[0] == 0xef &&
        (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf)
        begin += 3;
    TextSpan root = {begin, text + length}, selected;
    bool found = member(root, mode, &selected);
    if (found) {
        static const char *fields[] = {
            "l2d_expression", "l2d_motion", "l2d_motion_lockhand", "sounds"
        };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
            collect_field(labels, selected, fields[i]);
    }
    free(text);
    return found;
}

/* 按字段名与下标查标签, 返回 labels 内部字符串的借用指针 (调用方不得 free, 生命周期同 labels);
   没有匹配返回 NULL, 调用方据此省略 label 字段。labels 可以为 NULL。 */
const char *bongo_cat_mver_label(const BongoCatMverLabels *labels,
    const char *field, size_t index) {
    if (!labels || !field) return NULL;
    for (size_t i = 0; i < labels->count; ++i)
        if (labels->entries[i].index == index &&
            strcmp(labels->entries[i].field, field) == 0)
            return labels->entries[i].label;
    return NULL;
}
