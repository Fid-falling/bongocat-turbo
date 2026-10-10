/*
 * Nuklear 的唯一实现单元。NK_IMPLEMENTATION 必须且只能在一个 .c 里定义 (否则
 * 符号重复定义), 所以这个文件故意只有两行 —— 别把别的代码加进来, 也别在别处
 * 再 define 一次。
 * 所有用到 Nuklear 的地方都必须经 nuklear_config.h 引入, 保证宏开关一致: 同一份
 * 头文件在不同宏组合下算出的结构体布局不同, 混用会静默越界。
 */
#define NK_IMPLEMENTATION
#include "nuklear_config.h"
