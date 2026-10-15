#ifndef BONGO_CAT_RENDERER_MODULE_H
#define BONGO_CAT_RENDERER_MODULE_H

#include "bongo_cat/app.h"
#include "bongo_cat/common.h"
#include "bongo_cat/renderer_module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Live2D 渲染模块(我们自己的 `bongo_live2d` 组件)的发现与目录约定。
 *
 * 命名约定(与 LIVE2D-PLUGIN-PLAN.md 的追加修正一致): 全部叫 `cores`, 不叫 plugins ——
 * 我们不兼容任何第三方插件 ABI, 也没有插件生态, 因此"插件"这个词会误导用户以为可以装别人
 * 的东西。两个 `cores` 目录的归属必须分清:
 *
 *   <exe 目录>/cores/bongo_live2d.dll       ← **我们编译的组件**, 随包分发。
 *                                             它内含 Cubism Framework(源码编出来的)与
 *                                             我们的桥接, 但**不含** Core。
 *   <存储根>/cores/<平台>/<架构>/*.dll      ← **用户自备的 Cubism Core**(专有库),
 *                                             由用户在设置里导入, 不进发行包。
 *
 * 为什么组件放 exe 同级而不是存储根: 它是程序自身的一部分(和资源、字体同级), 版本必须与
 * exe 一起走 —— 放在用户数据目录会出现"换了 exe 但组件还是旧版"的错配, 而 ABI 校验会因此
 * 拒绝加载, 用户看到的就是"Live2D 突然不能用了"。Core 则相反: 它是用户的闭源资产、升级
 * 节奏与我们无关, 所以放存储根, 与配置/模型一起被用户掌控(便携模式下整体拷走仍可用)。
 *
 * 组件目录查找顺序(高 → 低):
 *   1. $BONGO_CAT_CORES_DIRECTORY  —— 排障/CI/把组件放别处;
 *   2. <exe 目录>/cores            —— 发行包保证存在的位置;
 *   3. <存储根>/cores              —— 绿色版/便携安装时用户手动放置的回退位置。
 *
 * 停用标记: 与组件同名的空文件 <组件名>.disabled。停用时不删文件, "重新启用"只是删掉这个
 * 空文件 —— 用户不需要再去找一份 DLL。
 */

/* 组件必须导出这个查询符号。发现阶段只认这一个符号名, 不解析结构体 —— 完整的
   版本化描述符/ops/宿主服务表在同一目录下的 renderer_module_abi.h(阶段 2 落地)。 */
#define BONGO_CAT_RENDERER_MODULE_SYMBOL "bongo_cat_renderer_module_query"
/* Live2D 渲染组件的基名(不含平台扩展名与路径)。 */
#define BONGO_CAT_RENDERER_MODULE_LIVE2D "bongo_live2d"

/* 组件状态。顺序即"严重程度", UI 按它选文案。 */
typedef enum BongoCatRendererModuleState {
    BONGO_CAT_RENDERER_MODULE_READY = 0, /* 文件在、没被停用、ABI 是我们要的 */
    BONGO_CAT_RENDERER_MODULE_DISABLED,  /* 有 .disabled 标记 */
    BONGO_CAT_RENDERER_MODULE_UNKNOWN,   /* 不是我们的组件(缺导出符号) */
    BONGO_CAT_RENDERER_MODULE_REJECTED   /* 加载/ABI/引擎校验失败, 原因见 reason */
} BongoCatRendererModuleState;

#define BONGO_CAT_RENDERER_MODULE_REASON_CAP 192

typedef struct BongoCatRendererModuleInfo {
    char name[64];       /* 文件名(不含路径), 用于日志与 UI */
    char path[BONGO_CAT_PATH_CAP];
    BongoCatRendererModuleState state;
    char reason[BONGO_CAT_RENDERER_MODULE_REASON_CAP];
} BongoCatRendererModuleInfo;

/* 解析组件目录(按上面的顺序), 返回第一个存在的目录; 都不存在时返回"将要使用的主路径"
   并置 exists=false —— 调用方可以据此把它显示成投放点, 或在打包检查里断言它必须存在。 */
bool bongo_cat_renderer_module_directory(char *output, size_t capacity,
    bool *exists);

/* 扫描组件目录并写日志(发现/停用/拒绝原因各一条)。返回写入条目的数量。
   注: 阶段 1 只做发现与校验; 真正的实例化在组件 ABI(阶段 2)落地后接上。 */
size_t bongo_cat_renderer_module_scan(BongoCatRendererModuleInfo *entries,
    size_t capacity);

/* 最近一次扫描的结果(供 UI 复用, 不必重复扫盘)。只在扫描后有效。 */
size_t bongo_cat_renderer_module_last(BongoCatRendererModuleInfo **entries);
/* Live2D 渲染组件是否可用(找到、未停用、ABI 通过)。降级路径与提示文案按它分流:
   "缺组件"与"缺 Core"是两件不同的事, 提示也必须分开。 */
bool bongo_cat_renderer_module_live2d_available(void);

/*
 * 阶段 2: 校验 / 装载 / 卸载。
 *
 * validate 是**纯函数**(不碰文件系统、不加载库), 所以"坏描述符矩阵"可以逐条断言;
 * open 负责加载、取导出符号、校验、建实例, 任何一步失败都按相反顺序回滚。
 */
bool bongo_cat_renderer_module_validate(const BongoCatRendererModule *module,
    const BongoCatRendererModuleHost *host, BongoCatResult *result,
    char *reason, size_t capacity);
bool bongo_cat_renderer_module_validate_host(
    const BongoCatRendererModuleHost *host);
bool bongo_cat_renderer_module_open(const char *path, const char *asset_root,
    BongoCatError *error);
void bongo_cat_renderer_module_close(void);
bool bongo_cat_renderer_module_active(void);
const char *bongo_cat_renderer_module_active_path(void);
/* 宿主服务表(静态, 生命周期覆盖组件存活期)。 */
const BongoCatRendererModuleHost *bongo_cat_renderer_module_host(void);
/* 目录服务要回答"六棵树在哪", 而那只有 app 知道 —— 启动期绑定一次。
   传 NULL 表示解绑(退出时调用, 免得服务表里留下悬空指针)。 */
void bongo_cat_renderer_module_bind_app(const BongoCatApp *app);

#ifdef __cplusplus
}
#endif

#endif
