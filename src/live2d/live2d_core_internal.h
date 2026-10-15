/*
 * Core 导入流程的内部约定(只有 live2d_core_import.c 与 live2d_core_archive.c 用)。
 *
 * 拆两个文件的原因: 行数策略(CheckLines 500 行)与职责边界刚好重合 ——
 *   * live2d_core_archive.c: 只跟 zip 打交道(白名单、路径安全、符号链接、上限、解压);
 *   * live2d_core_import.c: 只跟磁盘与元数据打交道(staging、校验、原子替换、core.json、回滚)。
 * 二者唯一的交界是下面这一个函数, 所以攻击面(第一条)可以单独审计与单独测试。
 */
#ifndef BONGO_CAT_LIVE2D_CORE_INTERNAL_H
#define BONGO_CAT_LIVE2D_CORE_INTERNAL_H

#include "bongo_cat/common.h"

/* 上限(任一超限即整体失败, 不做部分导入):
   * zip 本身 <= 512 MiB: 官方 SDK 压缩包约 100 MB 出头, 512 MiB 留足余量;
   * 单个 Core 条目 <= 64 MiB: Core 实际只有几 MB, 64 MiB 是"绝不放行异常大文件"的硬闸;
   * 条目数 <= 20000: 与模型导入同口径, 挡住"几十万条目"的畸形容器;
   * 条目路径 <= 512 字符(且 miniz 自己的文件名缓冲是 260, 超过即拒绝整包)。 */
#define BONGO_CAT_CORE_ZIP_LIMIT (UINT64_C(512) * 1024 * 1024)
#define BONGO_CAT_CORE_ENTRY_LIMIT (UINT64_C(64) * 1024 * 1024)
#define BONGO_CAT_CORE_ENTRY_COUNT_LIMIT 20000
#define BONGO_CAT_CORE_PATH_LIMIT 512

/* 从压缩包里取出**唯一一个**匹配本机平台/架构的 Core, 写到 staged 文件。
 * sdk_hint 收到压缩包内 SDK 根目录名(供支持排查), 失败时清空。
 * 任何不安全条目(链接/重解析点/特殊文件/加密/超长名)都会让整包失败 —— 不"跳过该条"。 */
BongoCatResult bongo_cat_live2d_core_extract_archive(const char *source,
    const char *staged, char *sdk_hint, size_t sdk_hint_capacity,
    BongoCatError *error);

#endif
