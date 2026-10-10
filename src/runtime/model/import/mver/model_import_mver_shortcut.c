/*
 * Mver 键位矩阵到 BongoCat 快捷方式串的翻译: 输入是配置里的一行 (Windows 虚拟键码数组), 输出形如
 * Control+Shift+KeyA 或 Gamepad:South, 由适配器生成阶段写进 bindings 的 shortcut 字段。
 * 契约: 最终由 bongo_cat_shortcut_matches 解析 —— 比较大小写不敏感、修饰键顺序无关, 但修饰键必须与
 * 按下状态完全一致 (多按一个 Shift 就不命中); output 由调用方提供, 成功时以 0 结尾, 失败时未定义。
 * 易错点: 键名风格必须和 shortcut.c 的 canonical_key 对齐 (Key 与 Num 前缀、Left 与 Right 后缀、
 * UpArrow 改名 ArrowUp 等), 改这里要同步改解析侧。
 */
#include "model_import_mver_internal.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

static bool append(char *output, size_t capacity, const char *value) {
    size_t used = strlen(output), length = strlen(value);
    if (used + length >= capacity) return false;
    memcpy(output + used, value, length + 1);
    return true;
}

/* 取单个键码的规范名: 优先用共享键名表, 否则写进调用方给的 generated 缓冲区 (16 字节, 返回值可能
   指向它, 只在本次调用内有效)。修饰键返回 NULL, 交给调用方当修饰位处理。 */
static const char *shortcut_key(int code, char generated[16]) {
    BongoCatMverKeyNames names = bongo_cat_mver_device_names(code, 0, 1);
    const char *name = names.items[0];
    if (!name && names.generated[0]) {
        snprintf(generated, 16, "%s", names.generated);
        name = generated;
    }
    if (!name || !*name || bongo_cat_mver_modifier_index(code) >= 0) return NULL;
    if (strncmp(name, "Key", 3) == 0 || strncmp(name, "Num", 3) == 0) {
        generated[0] = name[3]; generated[1] = '\0'; return generated;
    }
    if (strcmp(name, "UpArrow") == 0) return "ArrowUp";
    if (strcmp(name, "DownArrow") == 0) return "ArrowDown";
    if (strcmp(name, "LeftArrow") == 0) return "ArrowLeft";
    if (strcmp(name, "RightArrow") == 0) return "ArrowRight";
    if (strcmp(name, "Return") == 0) return "Enter";
    return name;
}

/* 键盘组合键: 一行里最多一个非修饰键, 修饰键按 Shift、Control、Alt 三类去重 (重复出现不算错),
   输出顺序固定为 Control、Shift、Alt、主键, 与配置里的出现顺序无关, 这样同一个组合只有一种写法。
   出现两个主键、键码不是整数或没有主键都返回 false。 */
static bool keyboard_chord(yyjson_val *row, char *output, size_t capacity) {
    output[0] = '\0';
    if (!yyjson_is_arr(row)) return false;
    bool modifiers[3] = {false}, primary = false;
    size_t index, count; yyjson_val *key;
    yyjson_arr_foreach(row, index, count, key) {
        if (!yyjson_is_int(key) && !yyjson_is_uint(key)) return false;
        int code = (int)yyjson_get_int(key);
        int modifier = bongo_cat_mver_modifier_index(code);
        if (modifier >= 0) { modifiers[modifier] = true; continue; }
        char generated[16]; const char *name = shortcut_key(code, generated);
        if (!name || primary) return false;
        primary = append(output, capacity, name);
    }
    if (!primary) return false;
    char key_name[BONGO_CAT_SHORTCUT_CAP];
    snprintf(key_name, sizeof(key_name), "%s", output); output[0] = '\0';
    return (!modifiers[1] || append(output, capacity, "Control+")) &&
        (!modifiers[0] || append(output, capacity, "Shift+")) &&
        (!modifiers[2] || append(output, capacity, "Alt+")) &&
        append(output, capacity, key_name);
}

/* 手柄组合键: Mver 用单元素数组表示一个按钮 (0 到 15 映射为 South 到 Select), 因此数组长度不等于
   1 一律失败; 输出带 Gamepad: 前缀, 与键盘绑定区分。 */
static bool gamepad_chord(yyjson_val *row, char *output, size_t capacity) {
    if (!yyjson_is_arr(row) || yyjson_arr_size(row) != 1) return false;
    yyjson_val *key = yyjson_arr_get_first(row);
    if (!yyjson_is_int(key) && !yyjson_is_uint(key)) return false;
    BongoCatMverKeyNames names = bongo_cat_mver_gamepad_names(
        (int)yyjson_get_int(key));
    return names.count && names.items[0] &&
        snprintf(output, capacity, "Gamepad:%s", names.items[0]) > 0;
}

/* 统一入口: 候选声明了手柄按钮且键码在 0 到 15 之间时按手柄解释, 否则一律按键盘解释 —— 同一个键码
   在两种模式下含义不同, 判断依据是候选的 gamepad_buttons 而不是模式名。raw 是 yyjson 值的类型
   擦除指针 (内部头文件不暴露 yyjson)。 */
bool bongo_cat_mver_chord(const BongoCatImportCandidate *candidate,
    void *raw, char *output, size_t capacity) {
    yyjson_val *row = raw;
    yyjson_val *only = yyjson_is_arr(row) && yyjson_arr_size(row) == 1
        ? yyjson_arr_get_first(row) : NULL;
    int code = (yyjson_is_int(only) || yyjson_is_uint(only))
        ? (int)yyjson_get_int(only) : -1;
    return candidate && candidate->gamepad_buttons && code >= 0 && code <= 15
        ? gamepad_chord(row, output, capacity) : keyboard_chord(row, output, capacity);
}

/* 音效绑定专用: 与视觉绑定不同, 不重排修饰键, 而是按行内顺序用加号连接任意组合, 并允许纯修饰键与
   鼠标键; 键名可能借自 shortcut_key 的 generated 缓冲区, 返回值只在本次调用内有效。 */
/* Mver's audio uses Windows keys even when its visual mode uses a gamepad.
   Preserve arbitrary chords, modifier-only keys, and mouse buttons. */
bool bongo_cat_mver_sound_chord(void *raw, char *output, size_t capacity) {
    yyjson_val *row = raw;
    if (!output || !capacity || !yyjson_is_arr(row) || !yyjson_arr_size(row)) return false;
    output[0] = '\0';
    size_t index, count; yyjson_val *key;
    yyjson_arr_foreach(row, index, count, key) {
        if (!yyjson_is_int(key) && !yyjson_is_uint(key)) return false;
        int64_t raw_code = yyjson_get_sint(key);
        if (raw_code <= 0 || raw_code > 255) return false;
        int code = (int)raw_code;
        const char *name = NULL;
        if (code == 1) name = "Left";
        else if (code == 2) name = "Right";
        else if (code == 4) name = "Middle";
        else if (code == 5) name = "Back";
        else if (code == 6) name = "Forward";
        else if (code == 16) name = "Shift";
        else if (code == 17) name = "Control";
        else if (code == 18) name = "Alt";
        else if (code == 160) name = "ShiftLeft";
        else if (code == 161) name = "ShiftRight";
        else if (code == 162) name = "ControlLeft";
        else if (code == 163) name = "ControlRight";
        else if (code == 164) name = "Alt";
        else if (code == 165) name = "AltGr";
        char generated[16];
        if (!name) name = shortcut_key(code, generated);
        if (!name || (index && !append(output, capacity, "+")) ||
            !append(output, capacity, name)) return false;
    }
    return true;
}
