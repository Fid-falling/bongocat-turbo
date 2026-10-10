/* Mver 适配器内部的共享声明: 键名翻译、和弦解析、行为/音效/特效生成与标签读取, 只在 mver/ 内部使用。
 * 参数里的 void * 是 yyjson 文档与值指针的类型擦除 (本头文件刻意不引 yyjson), 实参类型传错编译器不会拦 ——
 * 例如 add_audio 的 config 与 rows 都是 yyjson_val *。生成函数只往调用方的 items 追加, 不接管文档所有权。 */
#ifndef BONGO_CAT_MODEL_IMPORT_MVER_INTERNAL_H
#define BONGO_CAT_MODEL_IMPORT_MVER_INTERNAL_H

#include "model_import_mver.h"

/* 一个键码最多两个别名 (左右修饰键各一个): items 的元素可能为 NULL, 那种情况下名字在 generated 里
 * (数字键码的兜底名), 取用时必须写成 items[i] ? items[i] : generated, 见 emit_pair 与 shortcut.c;
 * count 为 0 表示键码不受支持, 调用方据此报 "unsupported key code"。 */
typedef struct BongoCatMverKeyNames {
    const char *items[2];
    char generated[16];
    size_t count;
} BongoCatMverKeyNames;

typedef struct BongoCatMverLabelEntry {
    char field[32];
    char label[BONGO_CAT_ID_CAP];
    size_t index;
} BongoCatMverLabelEntry;

typedef struct BongoCatMverLabels {
    BongoCatMverLabelEntry entries[BONGO_CAT_BEHAVIOR_CAP];
    size_t count;
} BongoCatMverLabels;

bool bongo_cat_mver_emit_pair(const char *hand, const char *keyboard,
    const char *directory, BongoCatMverKeyNames names, BongoCatError *error);
BongoCatMverKeyNames bongo_cat_mver_device_names(int code,
    size_t occurrence, size_t total);
BongoCatMverKeyNames bongo_cat_mver_gamepad_names(int code);
int bongo_cat_mver_modifier_index(int code);
bool bongo_cat_mver_chord(const BongoCatImportCandidate *candidate,
    void *row, char *output, size_t capacity);
bool bongo_cat_mver_sound_chord(void *row, char *output, size_t capacity);
bool bongo_cat_mver_add_audio(void *output, void *items, void *config, void *rows,
    const BongoCatImportCandidate *candidate, const BongoCatMverLabels *labels,
    const char *target);
bool bongo_cat_mver_effects(void *output, void *items, void *root, void *mode,
    const BongoCatImportCandidate *candidate, const char *target);
bool bongo_cat_mver_add_behaviors(void *output, void *items, void *mode,
    const BongoCatImportCandidate *candidate, const BongoCatMverLabels *labels,
    BongoCatError *error);
bool bongo_cat_mver_labels_load(const char *path, const char *mode,
    BongoCatMverLabels *labels);
const char *bongo_cat_mver_label(const BongoCatMverLabels *labels,
    const char *field, size_t index);
#endif
