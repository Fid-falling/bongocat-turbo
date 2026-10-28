#ifndef BONGO_CAT_SELFCHECK_INTERNAL_H
#define BONGO_CAT_SELFCHECK_INTERNAL_H

#include "bongo_cat/selfcheck.h"

/*
 * 自检的分组实现之间的私有接口 (只在 src/runtime/diagnostics/selfcheck*.c 之间用)。
 *
 * 拆成多个文件有两个原因: 单文件 500 行的策略 (cmake/CheckLines.cmake), 以及这几组
 * 检查依赖的子系统完全不同 (渲染组碰 Vulkan/OpenGL, 文件组碰目录与磁盘, Live2D 组
 * 碰组件/Core)。放进同一个文件会让"改渲染探测"与"改权限探测"互相牵连。
 *
 * 唯一的共享状态是 builder: 它带着当前分组名, add 时把分组名一起写进条目 ——
 * 面板按分组画小标题, 报告文本按分组分段, 两处读的是同一个字段。
 */
typedef struct BongoCatSelfCheckBuilder {
    BongoCatSelfCheckReport *report;
    char section[BONGO_CAT_SELFCHECK_SECTION_CAP];
} BongoCatSelfCheckBuilder;

/* 开始一个新分组 (下面 add 的条目都归到它) */
void bongo_cat_selfcheck_section(BongoCatSelfCheckBuilder *builder,
    const char *title);
/* 追加一条结论。detail 是 printf 风格的一句话, 必须能独立读懂。 */
void bongo_cat_selfcheck_add(BongoCatSelfCheckBuilder *builder,
    BongoCatSelfCheckStatus status, const char *title, const char *format, ...);

/* 第 1 组: 版本与系统 (版本/构建能力/OS/DPI/包标识/管理员) */
void bongo_cat_selfcheck_env(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);
/* 第 2 组: 渲染后端 (当前后端/Vulkan 设备/呈现模式与合成/首帧探测/OpenGL) */
void bongo_cat_selfcheck_render(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);
/* 第 3 组: Live2D (组件/Core/渲染器创建/着色器/当前模型) */
void bongo_cat_selfcheck_live2d(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);
/* 第 4 组: 文件与权限 (存储根/exe 目录/缓存/模型/日志/磁盘剩余) */
void bongo_cat_selfcheck_files(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);
/* 第 5 组: 配置 (能否解析/非法值/关键项当前值) */
void bongo_cat_selfcheck_config(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);
/* 第 6 组: 音频 (miniaudio 设备能否初始化) */
void bongo_cat_selfcheck_audio(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);
/* 第 7 组: 输入 (接收器是否在跑; 只读状态, 不做深度测试) */
void bongo_cat_selfcheck_input(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app);

/* --- 各文件共用的只读小工具 --- */
/* 目录能否写: 在里面建一个临时文件, 写一个字节后删掉。失败时 reason 写明原因。
   这是自检里唯一的写操作 (不碰配置/模型), 见 selfcheck.h 的硬约束 1。 */
bool bongo_cat_selfcheck_dir_writable(const char *directory, char *reason,
    size_t capacity);
/* 目录是否可读: 尝试枚举一次, 能读到至少一个条目或空目录都算可读。 */
bool bongo_cat_selfcheck_dir_readable(const char *directory, char *reason,
    size_t capacity);
/* 取 path 的父目录 (就地写到 output); 没有分隔符时输出空串 */
void bongo_cat_selfcheck_parent_dir(const char *path, char *output,
    size_t capacity);
/* 渲染设置的一句话摘要 (renderScale / textureQuality / 帧率 / 垂直同步) */
void bongo_cat_selfcheck_render_settings(const struct BongoCatApp *app,
    char *output, size_t capacity);

#endif
