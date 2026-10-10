/* stb_vorbis must be declared before miniaudio to enable its Ogg backend. */
/*
 * miniaudio + stb_vorbis 的唯一"展开点" (思路同 stb_image_impl.c)。
 *
 * 顺序是硬要求: stb_vorbis 的声明必须出现在 miniaudio **之前**, 否则 miniaudio 编译它的 Ogg
 * 后端时看不到这些符号。先定义 STB_VORBIS_HEADER_ONLY 借第一次 include 只拿声明, 等
 * miniaudio 的实现展开完、再把该宏取消后 include 第二次来生成实现 —— 这是 stb 系列标准的
 * 单 TU 手法, 顺序或宏写反了都会变成链接错误。
 * STB_VORBIS_NO_INTEGER_CONVERSION 是因为消费端只要浮点样本, 去掉整数转换可以少一份代码。
 * BONGO_CAT_VORBIS_HEADER 可覆盖, 方便换成 vendored / 固定版本的副本。
 */
#ifndef BONGO_CAT_VORBIS_HEADER
#define BONGO_CAT_VORBIS_HEADER <stb_vorbis.c>
#endif
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_HEADER_ONLY
#include BONGO_CAT_VORBIS_HEADER
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#undef STB_VORBIS_HEADER_ONLY
#include BONGO_CAT_VORBIS_HEADER
