/*
 * 设置窗口字形图集的范围表维护。
 *
 * 背景: 字体图集是启动时按固定范围建好的 (中文有几千字, 全建太贵), 所以模型名/
 * 行为名里出现的新字必须先"登记", 下次重建图集时才会被包含进去; 否则名字会画成
 * 缺字方块。这就是 pending_import_names 和这些函数存在的全部理由。
 *
 * 范围表格式: 升序的 [start, end] 对, 以 0 结尾 —— 与 Nuklear 的
 * nk_font_config.range 完全一致, 不要改格式。ASCII (<= 0x7e) 永远在表里, 所以
 * 一律跳过, 免得把表撑满。
 */
#include "preferences_state.h"
#include "preferences_model_glyphs.h"

#include <stdio.h>
#include <string.h>

static bool glyph_covered(const uint32_t *ranges, size_t used, uint32_t rune) {
    for (size_t i = 0; i + 1 < used; i += 2)
        if (rune >= ranges[i] && rune <= ranges[i + 1]) return true;
    return false;
}

/*
 * 把一段文本里所有"表里还没有的非 ASCII 字"追加进范围表 (每个字自己起一个区间)。
 * 解码失败时只前进一个字节而不是放弃: 名字里混进半个 UTF-8 序列时, 至少要保证后面
 * 能解码的字还是被收进去。
 * 容量判断留了两个位置给 [暂停] 和结尾的 0, 表满就直接返回 (宁可少几个字也不能越界)。
 */
static void add_text(uint32_t *ranges, size_t capacity,
    size_t *used, const char *text) {
    const char *cursor = text;
    size_t remaining = text ? strlen(text) : 0;
    while (cursor && *cursor) {
        nk_rune rune = 0;
        int decoded = nk_utf_decode(cursor, &rune, (int)remaining);
        if (decoded <= 0) {
            cursor++;
            if (remaining) remaining--;
            continue;
        }
        cursor += decoded; remaining -= (size_t)decoded;
        if (rune <= 0x7e || glyph_covered(ranges, *used, rune)) continue;
        if (*used + 2 >= capacity) return;
        ranges[(*used)++] = rune;
        ranges[(*used)++] = rune;
        ranges[*used] = 0;
    }
}

/*
 * 表里能不能覆盖整段文本 (决定要不要重建图集)。多字节解码失败按"覆盖不了"处理:
 * 宁可多重建一次图集, 也不要画出缺字。
 */
static bool text_covered(const uint32_t *ranges, size_t capacity,
    const char *text) {
    if (!ranges || !capacity || !text || !text[0]) return false;
    size_t used = 0;
    while (used < capacity && ranges[used]) used++;
    const char *cursor = text;
    size_t remaining = text ? strlen(text) : 0;
    while (cursor && *cursor) {
        nk_rune rune = 0;
        int decoded = nk_utf_decode(cursor, &rune, (int)remaining);
        if (decoded <= 0) return false;
        if (rune > 0x7e && !glyph_covered(ranges, used, rune)) return false;
        cursor += decoded;
        remaining -= (size_t)decoded;
    }
    return true;
}

/*
 * 登记一个"下次重建图集时要包含"的名字 (导入过程中的新包名)。
 * 去重 + 封顶: 同一个包在导入过程中会被回调很多次。
 */
void bongo_cat_preferences_model_glyphs_note(
    BongoCatPreferences *preferences, const char *name) {
    if (!preferences || !name || !name[0]) return;
    for (size_t i = 0; i < preferences->pending_import_name_count; ++i)
        if (!strcmp(preferences->pending_import_names[i], name)) return;
    if (preferences->pending_import_name_count >= BONGO_CAT_MODEL_CAP) return;
    snprintf(preferences->pending_import_names[
        preferences->pending_import_name_count++], BONGO_CAT_ID_CAP, "%s", name);
}

bool bongo_cat_preferences_model_glyphs_ready(
    const BongoCatPreferences *preferences, const char *name) {
    return preferences && text_covered(preferences->glyph_ranges,
        sizeof(preferences->glyph_ranges) /
            sizeof(preferences->glyph_ranges[0]), name);
}

/* 登记的名字在重建完图集后必须清掉, 否则每次重建都会把上一轮的名字再算一遍 */
void bongo_cat_preferences_model_glyphs_clear_pending(
    BongoCatPreferences *preferences) {
    if (preferences) preferences->pending_import_name_count = 0;
}

/*
 * 重建图集前把"界面上可能出现的所有文字"补进范围表: 模型名、行为名、快捷键标签,
 * 以及导入中登记的新名字。
 * used 必须是偶数 (区间对), 奇数说明表被外部写坏了, 这时直接放弃而不是硬填 ——
 * 填进去会让 Nuklear 把半个区间当数据解释。
 * 如果这一轮加进新字, 调用方 (reload_fonts) 会发现表变了并重建图集。
 */
void bongo_cat_preferences_model_glyphs(const BongoCatApp *app,
    uint32_t *ranges, size_t capacity) {
    if (!app || !ranges) return;
    size_t used = 0;
    while (used < capacity && ranges[used]) used++;
    if (used >= capacity || (used & 1)) return;
    for (size_t i = 0; i < app->models.count; ++i) {
        const BongoCatModelEntry *entry = &app->models.entries[i];
        add_text(ranges, capacity, &used,
            bongo_cat_model_name(&app->settings, entry));
    }
    for (size_t i = 0; i < app->behaviors.count; ++i)
        add_text(ranges, capacity, &used, app->behaviors.entries[i].label);
    for (size_t i = 0;
        i < app->settings.behavior_shortcut_count; ++i)
        add_text(ranges, capacity, &used,
            app->settings.behavior_shortcuts[i].label);
    if (app->preferences) {
        const BongoCatPreferences *preferences = app->preferences;
        for (size_t i = 0; i < preferences->pending_import_name_count; ++i)
            add_text(ranges, capacity, &used,
                preferences->pending_import_names[i]);
    }
}
