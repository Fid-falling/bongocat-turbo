/*
 * 单行文本编辑的纯逻辑层 (不含任何绘制与 SDL 依赖)。
 *
 * 这个文件里所有的"位置"都是**字节偏移**, 但对外表现的移动/删除一律按 UTF-8 字素
 * 边界走 —— 这就是 boundary/previous/next 存在的理由。少了它们, 光标会停在多字节
 * 字符中间, 退格会删出半个汉字 (后续渲染出一串乱码方块)。
 * 所有函数都对 text/cursor/select_all 做空指针检查, 因为它们也被只有部分状态可用的
 * 路径 (设置页的普通输入框) 调用。
 */
#include "preferences_text_edit.h"

#include <string.h>

static bool continuation(unsigned char value) {
    return (value & 0xc0) == 0x80;
}

/* 字节偏移吸附到字符边界: 只向左吸附, 保证不会跨过当前字符 */
static size_t boundary(const char *text, size_t cursor) {
    size_t length = text ? strlen(text) : 0;
    if (cursor > length) cursor = length;
    while (cursor && cursor < length && continuation((unsigned char)text[cursor]))
        cursor--;
    return cursor;
}

static size_t previous(const char *text, size_t cursor) {
    cursor = boundary(text, cursor);
    if (!cursor) return 0;
    do cursor--; while (cursor && continuation((unsigned char)text[cursor]));
    return cursor;
}

static size_t next(const char *text, size_t cursor) {
    size_t length = text ? strlen(text) : 0;
    cursor = boundary(text, cursor);
    if (cursor >= length) return length;
    do cursor++; while (cursor < length &&
        continuation((unsigned char)text[cursor]));
    return cursor;
}

/*
 * 从 UTF-8 首字节推出这个字符有几个字节, 并做最基本的合法性校验 (续字节、以及几个
 * 明确规定非法的区间: 代理区 / 过长编码 / 超出 U+10FFFF)。
 * 非法序列返回 0, 调用方会跳过这一个字节继续 —— 输入法粘贴进来的数据不一定干净,
 * 不能因为一个坏字节就整段丢弃。
 */
static size_t rune_bytes(const unsigned char *value, size_t remaining) {
    if (!remaining || !value[0]) return 0;
    if (value[0] < 0x80) return 1;
    size_t count = value[0] >= 0xc2 && value[0] <= 0xdf ? 2 :
        value[0] >= 0xe0 && value[0] <= 0xef ? 3 :
        value[0] >= 0xf0 && value[0] <= 0xf4 ? 4 : 0;
    if (!count || count > remaining) return 0;
    for (size_t i = 1; i < count; ++i)
        if (!continuation(value[i])) return 0;
    if ((count == 3 && value[0] == 0xe0 && value[1] < 0xa0) ||
        (count == 3 && value[0] == 0xed && value[1] >= 0xa0) ||
        (count == 4 && value[0] == 0xf0 && value[1] < 0x90) ||
        (count == 4 && value[0] == 0xf4 && value[1] >= 0x90)) return 0;
    return count;
}

void bongo_cat_text_edit_begin(const char *text, size_t *cursor,
    bool *select_all) {
    if (cursor) *cursor = text ? strlen(text) : 0;
    if (select_all) *select_all = false;
}

/*
 * 插入一段文本 (来自输入法或剪贴板)。
 * 全程只做一次 memmove: 先把尾巴整体右移"这次要插入的总字节数"是做不到的 (要逐个
 * 字符算), 所以这里对每个字符各 memmove 一次 —— 单行文本很短, 这点开销可以接受,
 * 换来的是逻辑简单且不会算错长度。
 * 容量判断用的是 >= capacity, 即永远给结尾的 '\0' 留一个字节。
 * select_all 状态下先清空: 输入法直接替换选区的行为要和用户预期一致。
 */
bool bongo_cat_text_edit_insert(char *text, size_t capacity, size_t *cursor,
    bool *select_all, const char *inserted) {
    if (!text || !capacity || !cursor || !select_all || !inserted) return false;
    bool changed = false;
    if (*select_all) {
        changed = text[0] != '\0';
        text[0] = '\0'; *cursor = 0; *select_all = false;
    }
    size_t length = strlen(text);
    *cursor = boundary(text, *cursor);
    size_t remaining = strlen(inserted);
    for (const unsigned char *source = (const unsigned char *)inserted;
        remaining && *source;) {
        size_t bytes = rune_bytes(source, remaining);
        if (!bytes) { source++; remaining--; continue; }
        if (bytes == 1 && source[0] < 32) {
            source++; remaining--; continue;
        }
        if (length + bytes >= capacity) break;
        memmove(text + *cursor + bytes, text + *cursor,
            length - *cursor + 1);
        memcpy(text + *cursor, source, bytes);
        *cursor += bytes; length += bytes;
        source += bytes; remaining -= bytes; changed = true;
    }
    return changed;
}

/* forward=true 是 Delete (删光标右边一个字符), false 是 Backspace (删左边一个);
 * 有选区时无论方向都是"删掉选区" */
bool bongo_cat_text_edit_erase(char *text, size_t *cursor,
    bool *select_all, bool forward) {
    if (!text || !cursor || !select_all) return false;
    size_t length = strlen(text);
    if (*select_all) {
        bool changed = length != 0;
        text[0] = '\0'; *cursor = 0; *select_all = false;
        return changed;
    }
    *cursor = boundary(text, *cursor);
    size_t start = forward ? *cursor : previous(text, *cursor);
    size_t end = forward ? next(text, *cursor) : *cursor;
    if (start == end) return false;
    memmove(text + start, text + end, length - end + 1);
    *cursor = start;
    return true;
}

/*
 * 光标移动。"有选区时按左右"被特意定义成"跳到行首/行尾": 这时的左右键承担的是
 * "取消选区并落到一端"的语义, 而不是移动一个字符。
 * 返回值是"需要重绘吗" —— 移动本身也算需要重绘 (光标位置变了), 所以有选区时即使
 * 位置没动也返回真。
 */
bool bongo_cat_text_edit_move(const char *text, size_t *cursor,
    bool *select_all, BongoCatTextEditMove move) {
    if (!text || !cursor || !select_all) return false;
    size_t length = strlen(text), old = *cursor;
    bool selected = *select_all;
    *cursor = boundary(text, *cursor);
    if (move == BONGO_CAT_TEXT_EDIT_HOME || (selected &&
        move == BONGO_CAT_TEXT_EDIT_LEFT)) *cursor = 0;
    else if (move == BONGO_CAT_TEXT_EDIT_END || (selected &&
        move == BONGO_CAT_TEXT_EDIT_RIGHT)) *cursor = length;
    else if (move == BONGO_CAT_TEXT_EDIT_LEFT) *cursor = previous(text, *cursor);
    else if (move == BONGO_CAT_TEXT_EDIT_RIGHT) *cursor = next(text, *cursor);
    *select_all = false;
    return selected || old != *cursor;
}

void bongo_cat_text_edit_select_all(const char *text, size_t *cursor,
    bool *select_all) {
    if (cursor) *cursor = text ? strlen(text) : 0;
    if (select_all) *select_all = true;
}

void bongo_cat_text_edit_clear(char *text, size_t *cursor, bool *select_all) {
    if (text) text[0] = '\0';
    if (cursor) *cursor = 0;
    if (select_all) *select_all = false;
}

/* 提交前修剪首尾空白: 用户手输的路径/名字很容易带一个尾随空格, 存进配置后
 * 会导致找不到文件这类莫名其妙的问题 */
void bongo_cat_text_edit_trim(char *text) {
    if (!text) return;
    size_t start = 0, end = strlen(text);
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        end--;
    if (start) memmove(text, text + start, end - start);
    text[end - start] = '\0';
}

/*
 * 把点击的像素横坐标换算成最接近的字符插入点 (点哪儿光标落哪儿)。
 * 用"前一个字符的右边界到当前字符右边界的中点"作为分界, 所以点在字符左半边时
 * 光标落在它前面, 右半边落在它后面 —— 和常见文本框的手感一致。
 * measure 由调用方注入 (字体测量), 这样本文件不必依赖 Nuklear。
 */
size_t bongo_cat_text_edit_nearest(const char *text, float target,
    float (*measure)(const void *userdata, const char *text, size_t length),
    const void *userdata) {
    if (!text || !measure || target <= 0.0f) return 0;
    size_t length = strlen(text), cursor = 0;
    float previous_width = 0.0f;
    while (cursor < length) {
        size_t after = next(text, cursor);
        float width = measure(userdata, text, after);
        if (target < previous_width + (width - previous_width) * 0.5f)
            return cursor;
        previous_width = width; cursor = after;
    }
    return length;
}
