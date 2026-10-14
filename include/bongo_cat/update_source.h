#ifndef BONGO_CAT_UPDATE_SOURCE_H
#define BONGO_CAT_UPDATE_SOURCE_H

#include "bongo_cat/common.h"

/* 一次更新最多尝试的源数量。8 个足够覆盖"官方 + 若干加速前缀 + 自建镜像",
   再多只会让全部失败时的等待时间变长。 */
#define BONGO_CAT_UPDATE_SOURCE_CAP 8
/* 单个源条目的最大长度: 加速前缀通常 30 字符上下, 自建镜像也可能带一长串路径。 */
#define BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP 320
/* 用户镜像列表文件的最大读取长度; 超过就当作文件坏了, 退回内置默认列表。 */
#define BONGO_CAT_UPDATE_SOURCE_TEXT_CAP 4096
/* 显示用的源名 (域名) 缓冲。 */
#define BONGO_CAT_UPDATE_SOURCE_LABEL_CAP 128

/* "direct" = 不加前缀, 直接用 release 元数据里的官方 URL。 */
#define BONGO_CAT_UPDATE_SOURCE_DIRECT "direct"

/*
 * 按顺序尝试的下载源列表。条目有两种写法:
 *   - "direct": 官方直链 (始终保留在列表首位, 除非用户显式改列表);
 *   - "https://前缀/": 前缀 + 官方 URL;
 *   - "https://自建镜像/{url}": 用 {url} 占位, 便于把官方 URL 塞进查询参数。
 * 顺序即优先级, 列表本身由 core/update_source.c 解析, 与运行时/网络无关, 因此可单测。
 */
typedef struct BongoCatUpdateSourceList {
    char entries[BONGO_CAT_UPDATE_SOURCE_CAP][BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP];
    size_t count;
} BongoCatUpdateSourceList;

#ifdef __cplusplus
extern "C" {
#endif

/* 内置默认列表。加速镜像都是公共免费服务, **随时可能失效或改域名**, 维护者改这里即可;
   失效的源只会让本次更新多花一个连接超时, 不影响正确性。 */
void bongo_cat_update_source_list_default(BongoCatUpdateSourceList *list);
/* 解析用户镜像列表文本 (每行一条, # 开头为注释)。返回解析出的条数, 0 表示文本里没有
   任何可用条目(调用方应当保留默认列表)。非法条目整行忽略, 不做部分解析。 */
size_t bongo_cat_update_source_list_parse(const char *text,
    BongoCatUpdateSourceList *list);
/* 条目本身是否合法: direct, 或 http(s) 前缀且不含控制字符/反斜杠。 */
bool bongo_cat_update_source_valid(const char *entry);
/* 用条目把官方 URL 拼成最终下载地址。返回 false 表示放不下或结果不合法。 */
bool bongo_cat_update_source_url(const char *entry, const char *origin,
    char *output, size_t capacity);
/* 把第 index 个源提到最前 (上次成功的源下次优先用)。越界或已在首位时空操作。 */
void bongo_cat_update_source_list_promote(BongoCatUpdateSourceList *list,
    size_t index);
/* 界面上显示的源名: 去掉协议与路径的域名; direct 显示成 "GitHub"。 */
void bongo_cat_update_source_label(const char *entry, char *output,
    size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
