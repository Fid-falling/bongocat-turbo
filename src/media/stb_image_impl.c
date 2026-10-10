/*
 * stb 系列在本工程里唯一的"展开点"。
 *
 * 这些 *_IMPLEMENTATION 宏全工程只能展开一次 (展开即产生定义, 展开两次就是重复符号), 所以
 * 所有 stb 实现都集中到这一个翻译单元, 其它文件只 include 头文件。
 * 裁剪是刻意的: 只留 PNG/JPEG (实际用到的全部格式), 其它格式在 stbi_info/stbi_load 处干净
 * 失败; STBI_NO_LINEAR 是因为调用方一律要 8 位 RGBA, 用不到线性浮点路径。
 * stb_image_write 的实现在这里一起展开, 它只用于导出封面/缩略图 PNG。
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
