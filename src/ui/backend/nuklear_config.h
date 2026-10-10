/*
 * Nuklear 的编译开关。这里的每一个宏都会改变 nuklear.h 里的结构体布局, 因此
 * 全工程必须只通过这个头文件引入 Nuklear —— 谁直接 #include <nuklear.h> 而没有
 * 带上同一组宏, 就会按另一套布局解释 nk_context, 症状是极隐蔽的内存越界。
 * 需要新开关时加在这里, 并确认 nuklear_impl.c 那个唯一的实现单元也看得到。
 */
#ifndef BONGO_CAT_NUKLEAR_CONFIG_H
#define BONGO_CAT_NUKLEAR_CONFIG_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include <nuklear.h>

#endif
