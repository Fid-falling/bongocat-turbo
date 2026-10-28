/*
 * "自动回退 OpenGL" 的持久化记忆 (实现见 renderer_fallback_state.c)。
 *
 * 只给 runtime 内部用: 设置页读 app->renderer_fallback 画提示条, 看门狗/初始化失败写记录,
 * 渲染后端切换清记录。状态文件的位置与理由见 .c 文件头。
 */
#ifndef BONGO_CAT_RENDERER_FALLBACK_STATE_H
#define BONGO_CAT_RENDERER_FALLBACK_STATE_H

#include <stdbool.h>

struct BongoCatApp;

/*
 * 连续自动回退达到这个次数就"暂停尝试 Vulkan"。
 *
 * 为什么不是 1: 第一次卡住可能是偶发 (显卡被别的程序占满、着色器缓存冷、驱动瞬时超时),
 * 直接永久钉死 OpenGL 对一台其实能跑 Vulkan 的机器是误伤; 连续两次才当作确定性故障。
 * 为什么不是 3+: 每次失败都要用户对着不出现的猫等 8 秒, 三次已经明显是在反复踩坑。
 */
#define BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD 2

/*
 * 原因枚举 (写进状态文件与日志, 不要翻译): 界面上再按这两个值查本地化文案。
 *   first-frame-timeout: 首帧看门狗 8 秒没等到呈现 (first_frame_watchdog.c)
 *   init-failed:         Vulkan 窗口/设备/交换链初始化失败 (shell/window.c)
 */
#define BONGO_CAT_RENDERER_FALLBACK_REASON_TIMEOUT "first-frame-timeout"
#define BONGO_CAT_RENDERER_FALLBACK_REASON_INIT "init-failed"

/* 读 <config_root>/renderer-fallback.json。文件不存在/损坏都只清空内存状态并继续启动,
   绝不让状态文件成为启动失败的原因。 */
void bongo_cat_renderer_fallback_load(struct BongoCatApp *app);

/*
 * 记录一次自动回退: 计数 +1、写原因/时间/显卡、达到阈值置 auto_disabled, 并落盘。
 * 返回 false = 状态文件没写成功; 此时本函数已经退回"把 settings.model.renderer 写死
 * OpenGL"的硬保险 (旧行为), 保证下次启动不会再次卡在同一个后端上。
 */
bool bongo_cat_renderer_fallback_note_failure(struct BongoCatApp *app,
    const char *reason, const char *detail, const char *gpu);

/* 用户手动把渲染后端设为 Vulkan (设置页/托盘/提示条上的"再试一次"):
   清空计数与暂停标志并落盘, 然后由调用方重启。source 只用于日志 (入口名)。 */
void bongo_cat_renderer_fallback_note_manual_retry(struct BongoCatApp *app,
    const char *source);

/* Vulkan 首帧真的呈现了: 计数归零、解除暂停并落盘 (只有存在记录时才写文件)。 */
void bongo_cat_renderer_fallback_note_success(struct BongoCatApp *app);

/* 本次启动是否应跳过 Vulkan。只对"自动尝试"(配置为 AUTO/VULKAN 且没有命令行覆盖) 生效,
   见 shell/window.c 的调用点。 */
bool bongo_cat_renderer_fallback_paused(const struct BongoCatApp *app);

/* 是否存在回退记录 (提示条与自检条目只看它)。 */
bool bongo_cat_renderer_fallback_recorded(const struct BongoCatApp *app);

/* 原因枚举 -> 中文摘要 (日志与自检报告用; 设置页用 i18n 自行翻译)。 */
const char *bongo_cat_renderer_fallback_reason_label(const char *reason);

#endif
