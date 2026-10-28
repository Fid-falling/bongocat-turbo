#ifndef BONGO_CAT_PREFERENCES_MODEL_GLYPHS_H
#define BONGO_CAT_PREFERENCES_MODEL_GLYPHS_H

#include <stddef.h>
#include <stdint.h>

typedef struct BongoCatApp BongoCatApp;
typedef struct BongoCatPreferences BongoCatPreferences;

/*
 * 模型/行为名字的字形范围维护 (实现在 preferences_model_glyphs.c)。
 * ranges 的格式是升序的 [start, end] 对、以 0 结尾, 与 Nuklear 的 nk_font_config.range
 * 一致, 长度必须是偶数。容量由调用方给 (preferences->glyph_ranges 的大小)。
 * 典型用法: glyphs_note 在导入过程中登记新名字 -> 重建图集前调 glyphs -> 重建成功后
 * clear_pending; glyphs_ready 用来判断"这个名字现在画得出来吗"。
 */
void bongo_cat_preferences_model_glyphs(const BongoCatApp *app,
    uint32_t *ranges, size_t capacity);
void bongo_cat_preferences_model_glyphs_note(BongoCatPreferences *preferences,
    const char *name);
bool bongo_cat_preferences_model_glyphs_ready(
    const BongoCatPreferences *preferences, const char *name);
void bongo_cat_preferences_model_glyphs_clear_pending(
    BongoCatPreferences *preferences);
/*
 * 登记一段运行时生成的文本 (自检报告) 里用到的字; 图集重建由调用方在帧末触发
 * (绘制中途不能重建, 见 preferences_fonts.c)。字体图集是按范围预烘焙的, 报告里的
 * 字大多不在界面文案里, 不登记就会画成缺字方块。
 */
void bongo_cat_preferences_glyphs_add_text(BongoCatPreferences *preferences,
    const char *text);
/* 把已登记的运行时文字并进范围表 (fonts_resolve 在重建图集前调用) */
void bongo_cat_preferences_glyphs_include_extra(
    const BongoCatPreferences *preferences, uint32_t *ranges, size_t capacity);

#endif
