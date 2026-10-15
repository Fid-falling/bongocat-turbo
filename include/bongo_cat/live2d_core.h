#ifndef BONGO_CAT_LIVE2D_CORE_H
#define BONGO_CAT_LIVE2D_CORE_H

#include "bongo_cat/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 用户自备的 Live2D Cubism Core: 解析位置、运行期加载、安全导入。
 *
 * 法律边界(见 LIVE2D-PLUGIN-PLAN.md §9.1): Cubism Core 是 Live2D Inc. 的专有运行库,
 * 我们不分发它, 也不代用户下载。程序只做三件事:
 *   1. 告诉用户去哪拿(引导页, 只打开浏览器);
 *   2. 把用户自己选中的官方 SDK 压缩包/动态库**安全地**取出一份放到存储根;
 *   3. 运行期从存储根加载它, 并把 csm* 符号转发给渲染组件。
 * 因此本模块里不存在任何网络代码 —— 这是刻意的, 也有 CI 策略兜底
 * (.github/scripts/check-publish-guards.ps1 的 cubism.live2d.com 判据)。
 *
 * 落点约定: <存储根>/cores/<平台>/<架构>/Live2DCubismCore.<ext>
 *   * 与随包分发的渲染组件(<exe>/cores/bongo_live2d.*)分开: Core 是用户的闭源资产,
 *     误删代价高、升级节奏与我们无关; 组件是程序自身的一部分, 版本必须与 exe 一起走。
 *   * 便携模式下 <存储根> 就是 BongoCatData/, 整体拷走仍然可用。
 *   * 目录名统一用 `cores` 而不是 plugins/live2d-core: 我们不兼容第三方插件 ABI,
 *     "插件"这个词会误导用户以为可以装别人的东西(见 LIVE2D-PLUGIN-PLAN.md §P0.8)。
 */

/* 平台/架构目录名("windows"/"linux"/"macos" 与 "x86_64"/"x86"/"arm64")。
   这三个函数是"目录约定"的唯一来源: 导入、查找、CI 与文档都从这里取。 */
const char *bongo_cat_live2d_core_platform_name(void);
const char *bongo_cat_live2d_core_arch_name(void);
/* 动态库文件名(Live2DCubismCore.dll / libLive2DCubismCore.so / libLive2DCubismCore.dylib)。 */
const char *bongo_cat_live2d_core_library_name(void);

/* <存储根>/cores。storage_root 为空时按 --storage-root → 便携模式 → 平台默认的顺序
   解析(与六棵树同一套逻辑, 见 storage_paths.c)。返回 false 表示无处可放。 */
bool bongo_cat_live2d_core_root_directory(char *output, size_t capacity);
/* <存储根>/cores/<平台>/<架构>: 导入的最终落点。 */
bool bongo_cat_live2d_core_install_directory(char *output, size_t capacity);
/* 导入过程的临时区 <存储根>/cores/.staging: 失败即整目录删除, 绝不留半成品。 */
bool bongo_cat_live2d_core_staging_directory(char *output, size_t capacity);

/* 导入结果: 供 UI/日志显示"从哪来、装到哪、是什么、哈希多少"。 */
typedef struct BongoCatLive2DCoreImport {
    char source[BONGO_CAT_PATH_CAP];   /* 用户选中的文件(压缩包或裸库) */
    char installed[BONGO_CAT_PATH_CAP];/* 落盘后的绝对路径 */
    char previous[BONGO_CAT_PATH_CAP]; /* 被替换掉的旧 Core(没有则为空), 可回滚 */
    char sha256[65];                   /* 落盘文件的 SHA-256: 给用户核对, 不当白名单用 */
    char sdk_hint[128];                /* 压缩包内的 SDK 根目录名, 便于支持排查 */
    uint32_t csm_version;              /* csmGetVersion() 的整数值(坑见 CHANGELOG: 不是字符串) */
    bool from_archive;
} BongoCatLive2DCoreImport;

/*
 * 从用户选中的文件导入 Core。source 可以是:
 *   * 官方 SDK 的 zip 压缩包 —— 只取出**一个**匹配本机平台/架构的 Core, 其余条目一个字节
 *     都不落盘(白名单判定, 见 live2d_core_import.c);
 *   * 裸的 Core 动态库 —— 直接校验后落盘。
 * 成功后旧 Core 仍在(改名为 <名称>.<旧sha前8位>.bak), report->previous 记录它, 便于回滚。
 * 任何一步失败: 清掉 .staging/、保留旧 Core 与旧 core.json 完全不变。
 */
BongoCatResult bongo_cat_live2d_core_import(const char *source,
    BongoCatLive2DCoreImport *report, BongoCatError *error);

/* 回滚到最近一次被替换的 Core(report->previous)。没有可回滚的文件返回 FORMAT。 */
BongoCatResult bongo_cat_live2d_core_rollback(BongoCatError *error);

/* 读取 <存储根>/cores/core.json 里记录的元数据(上一次成功导入的结果)。 */
bool bongo_cat_live2d_core_read_manifest(BongoCatLive2DCoreImport *report);

/* 启动期装载: 按 环境变量 → <存储根>/cores/<平台>/<架构>/ → <存储根>/cores/
   → <exe 目录>/cores/<平台>/<架构>/ → <exe 目录>/cores/ 的顺序找第一个能通过三符号探测的
   Core 并加载。找不到不是错误(程序照常启动, Live2D 走诊断兜底), 返回 false 并写日志。 */
bool bongo_cat_live2d_core_initialize(void);

/* 当前是否已有可用的 Core(能力位协商要用它, 必须在组件选择之前就能回答)。 */
bool bongo_cat_live2d_core_available(void);
/* 已加载 Core 的 csm* 符号解析(宿主服务表 core_symbol 的实现)。 */
void *bongo_cat_live2d_core_symbol(const char *name);
/* csmGetVersion() 的整数值; 未加载时为 0。 */
uint32_t bongo_cat_live2d_core_version(void);
/* 当前已加载 Core 的文件路径(未加载时为空串)。 */
const char *bongo_cat_live2d_core_loaded_path(void);
/* 卸载: 按"先销毁实例与 GPU 资源, 再 dlclose/LoadLibrary 反注册"的顺序由调用方负责,
   本函数只负责释放句柄本身。 */
void bongo_cat_live2d_core_shutdown(void);

/* 平台层: 这三个是 windows_live2d_core.c / posix_live2d_core.c 提供的薄封装,
   放在公开头里是为了让导入流程能在"落盘后立刻试加载一次"从而给出确切失败原因。 */
void *bongo_cat_platform_live2d_core_open(const char *path, BongoCatError *error);
void bongo_cat_platform_live2d_core_close(void *handle);
void *bongo_cat_platform_live2d_core_symbol(void *handle, const char *name);
/* 三符号探测: csmGetVersion + csmReviveMocInPlace + csmInitializeModelInPlace。
   只查一个符号会把"别的同名库"误判成 Core。 */
bool bongo_cat_platform_live2d_core_probe(void *handle);
/* 校验文件的机器架构与本进程一致(PE Machine / ELF e_machine / Mach-O cputype)。 */
bool bongo_cat_platform_live2d_core_arch_matches(const char *path);

#ifdef __cplusplus
}
#endif

#endif
