#ifndef BONGO_CAT_SELFCHECK_H
#define BONGO_CAT_SELFCHECK_H

#include "bongo_cat/common.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BongoCatApp;

/*
 * 设置 → 应用 → [运行自检]: 一次只读的运行环境诊断。
 *
 * 这份设施存在的理由只有一个 —— 用户报告的那类故障 (Vulkan 首帧附近卡死/闪退) 在
 * 日志里几乎没有线索。自检把"这台机器现在到底能不能跑起来"拆成每项一句明确结论
 * (通过 / 警告 / 失败), 让用户能直接复制一份报告发给支持, 而不是回传一段停在
 * create_renderer 的日志。
 *
 * 硬约束 (改这里之前先看):
 *  1. **只读**: 不改配置、不改模型、不联网。唯一的写操作是"试写一个临时文件再删掉"
 *     来证明目录可写, 以及用户主动点"保存为文件"时写报告本身;
 *  2. **不干扰正在运行的宠物窗口**: Vulkan/OpenGL 探测都在自己的隐藏窗口里做;
 *  3. 每项必须有结论: 不允许"未知" —— 拿不到就按失败/警告写清原因;
 *  4. 整体 < 5 秒 (探测各用独立上下文, 单次设备/交换链创建是百毫秒级)。
 */

typedef enum BongoCatSelfCheckStatus {
    BONGO_CAT_SELFCHECK_PASS = 0,
    BONGO_CAT_SELFCHECK_WARN,
    BONGO_CAT_SELFCHECK_FAIL
} BongoCatSelfCheckStatus;

#define BONGO_CAT_SELFCHECK_ITEM_CAP 40
#define BONGO_CAT_SELFCHECK_SECTION_CAP 32
#define BONGO_CAT_SELFCHECK_TITLE_CAP 64
#define BONGO_CAT_SELFCHECK_DETAIL_CAP 320
#define BONGO_CAT_SELFCHECK_TEXT_CAP 8192

typedef struct BongoCatSelfCheckItem {
    char section[BONGO_CAT_SELFCHECK_SECTION_CAP];
    char title[BONGO_CAT_SELFCHECK_TITLE_CAP];
    BongoCatSelfCheckStatus status;
    /* 一句话结论: 必须能独立读懂 (带路径/版本/原因), 不能只写"失败"两个字 */
    char detail[BONGO_CAT_SELFCHECK_DETAIL_CAP];
} BongoCatSelfCheckItem;

typedef struct BongoCatSelfCheckReport {
    BongoCatSelfCheckItem items[BONGO_CAT_SELFCHECK_ITEM_CAP];
    size_t count;
    unsigned pass, warn, fail;
    unsigned elapsed_ms;
    /*
     * 是否建议切到 OpenGL。置位的条件只有一条: Vulkan 首帧探测失败 (或者当前就是
     * Vulkan 但 Live2D 的渲染器建不出来)。面板据此显示 [切换并重启]。
     */
    bool recommend_opengl;
    /* 总结那一句 (面板底部与报告末尾共用同一份文案) */
    char summary[BONGO_CAT_SELFCHECK_DETAIL_CAP];
    /* 可复制/保存的完整报告文本 (面板的"复制报告"直接用它) */
    char text[BONGO_CAT_SELFCHECK_TEXT_CAP];
} BongoCatSelfCheckReport;

/* 跑一次完整自检。report 由调用方提供 (不要放在栈上: 它有二十多 KB)。 */
void bongo_cat_selfcheck_run(struct BongoCatApp *app,
    BongoCatSelfCheckReport *report);

/* 默认落盘位置: <存储根>/logs/selfcheck-<时间>.txt。失败返回 false。 */
bool bongo_cat_selfcheck_default_path(char *output, size_t capacity);

/* 把 report->text 写到 path (覆盖)。失败时 error 里是可直接展示的原因。 */
bool bongo_cat_selfcheck_save(const BongoCatSelfCheckReport *report,
    const char *path, BongoCatError *error);

#ifdef __cplusplus
}
#endif

#endif
