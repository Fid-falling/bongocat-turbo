#ifndef BONGO_CAT_RENDERER_MODULE_ABI_H
#define BONGO_CAT_RENDERER_MODULE_ABI_H

#include "bongo_cat/common.h"
#include "bongo_cat/image.h"
#include "bongo_cat/json.h"
#include "bongo_cat/model.h"
#include "bongo_cat/vk_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Live2D 渲染组件(`cores/bongo_live2d.*`)与宿主之间的 ABI v1。
 *
 * 设计纪律(每一条都对应一次真实的踩坑):
 *   1. **版本化描述符 + struct_size**: 组件与宿主是同架构下的同编译器 ABI, 但两侧的
 *      "同一个结构体"随时可能各自演化。所以描述符**首两个字段**固定是 abi_version 与
 *      struct_size, 宿主逐字段校验, 不匹配就拒绝加载 —— 而不是按偏移乱读后崩在用户面前。
 *   2. **双向校验**: 组件也要校验宿主(它的 abi_version/struct_size), 老组件遇到新宿主时
 *      必须能干净地返回 NULL, 而不是读越界。query 的返回值即"能不能配合"的答案。
 *   3. **必备子集 + 其余判空**: 只有 7 个回调是必须实现的; 其余能力(动作/表情/参数/镜像…)
 *      一律判空调用并回落到诊断实现 —— "没有这个能力"必须是一种正常状态, 不是错误。
 *   4. **能力位协商**: flags(是否需要用户自备的 Core)与 graphics_backends(GL/VK)由描述符
 *      声明, 宿主先校验"后端支持"再校验"Core 在不在", 顺序不能反 —— 否则用户拿到的是
 *      "缺 Core"这种误导性结论, 而真正的问题是这次构建不支持他的后端。
 *   5. **不跨界的所有权**: 组件不链接宿主的 SDL/yyjson, 也不自己 dlopen Core; 一切外部资源
 *      都走下面的服务表。宿主分配的内存由宿主释放, 组件分配的内存由组件释放。
 *
 * 与 BongoCat-X 的 `bongo_cat_model_plugin_query` 那套**互不兼容**: 两者的服务表内容、
 * ops 集合与图形契约都不同, 同名 DLL 放进我们的 cores 目录只会被判为"缺导出符号"。
 */

/* ABI 版本: 任何破坏兼容性的改动(字段顺序/语义)都要 +1, 并让老组件被明确拒绝。 */
#define BONGO_CAT_RENDERER_MODULE_ABI 1u

/* 引擎: 目前只有 Live2D(Cubism)。预留 Inox2D 一类不需要 Core 的实现。 */
#define BONGO_CAT_RENDERER_ENGINE_LIVE2D 1u

/* flags: 组件需要宿主先加载好用户自备的 Cubism Core 才能工作。 */
#define BONGO_CAT_RENDERER_FLAG_REQUIRES_CORE 1u

/* graphics_backends: 组件支持的图形后端(与 bongo_cat_renderer_* 的选择彼此独立)。 */
#define BONGO_CAT_RENDERER_BACKEND_OPENGL 1u
#define BONGO_CAT_RENDERER_BACKEND_VULKAN 2u

/* 宿主服务表: 组件能用的**全部**外部能力。
 *
 * 规则: 表由宿主拥有且生命周期覆盖组件的整个存活期; 组件不得缓存表以外的宿主指针。
 * 表头两个字段是版本与尺寸, 组件必须校验它们(见纪律 2)。 */
typedef struct BongoCatRendererModuleHost {
    uint32_t abi_version;
    uint32_t struct_size;
    /* 解析用户自备 Core 的 csm* 符号(Core 由宿主持有, 组件从不自己加载它)。
       找不到返回 NULL —— 组件必须把它当成"这个能力不可用",而不是崩。 */
    void *(*core_symbol)(const char *name);
    /* 日志: 组件的每一条诊断都从这里出去, 宿主负责加前缀与落盘。 */
    void (*log)(int priority, const char *message);
    /* 日志(带格式): 避免组件为了拼字符串而自己引入 CRT 边界问题。 */
    void (*log_format)(int priority, const char *format, const char *argument);
    /* 内存: 谁分配谁释放, 但**跨边界**的缓冲必须成对使用这两个函数 —— 组件与宿主可能是
       不同的 CRT 实例(Windows 上尤其明显), 混用会直接堆损坏。 */
    void *(*allocate)(size_t size);
    void (*release)(void *memory);
    /* 目录服务: 组件不该自己拼路径(便携模式/--storage-root 的规则只有宿主知道)。
       kind 取 BongoCatRendererHostDirectory。返回 false 表示该目录不可用。 */
    bool (*directory)(int kind, char *output, size_t capacity);

    /* ---- 以下按"组件实际用到什么"逐项补齐(清单见 LIVE2D-PLUGIN-PLAN.md §P0.9) ---- */

    /* 时间: 与宿主同一时钟(帧节拍/动画都用它), 避免组件自己引入第二套计时实现。 */
    uint64_t (*ticks_ns)(void);
    /* 当前生效的图形后端(BONGO_CAT_RENDERER_BACKEND_*): 组件据此选 GL/VK 渲染路径。 */
    uint32_t (*active_backend)(void);
    /* Cubism 渲染器是否走 Vulkan(宿主配置层决定, 组件在建渲染器之前问它) */
    bool (*cubism_renderer_uses_vulkan)(void);
    /* 组件回报"我这次用的是哪个渲染器": 宿主据此记日志并决定 VK 合成路径。 */
    void (*cubism_renderer_set_vulkan)(bool enabled);
    /* GL 能力: 组件不链接 SDL/GLEW, 上下文就绪状态与错误清理由宿主回答。
       契约: 宿主在调用 ops.draw 之前保证 GL 上下文已 current。
       window/context 对组件是**不透明句柄**(宿主自己的 SDL 对象), 组件只能比较相等与回传,
       不得解引用 —— 封面的离屏渲染需要在两个上下文之间来回切。 */
    bool (*gl_context_ready)(void);
    bool (*gl_clear_errors)(void);
    void *(*gl_current_window)(void);
    void *(*gl_current_context)(void);
    bool (*gl_make_current)(void *window, void *context);
    /* 宿主窗口的像素尺寸: window 句柄来自 gl_current_window(), 组件原样回传即可。 */
    bool (*window_size_pixels)(void *window, int *width, int *height);
    /* 图像管线: 宿主独占(它带纹理缓存与 alpha mask 语义), 组件只用不实现。
       约定: 任何产出 BongoCatImage 的调用都必须用 image_free 归还。 */
    BongoCatResult (*image_fit)(const char *path, int max_width, int max_height,
        BongoCatImage *out, BongoCatError *error);
    void (*image_free)(BongoCatImage *image);
    void (*image_make_alpha_mask)(const BongoCatImage *image,
        BongoCatImageAlphaMask *mask);
    unsigned int (*image_texture_model)(const char *path, bool direct_decode,
        int *width, int *height, BongoCatImageAlphaMask *alpha,
        BongoCatImageProgress progress, void *userdata, BongoCatError *error);
    /* 模型 JSON 的兼容性归一化只有一份实现(宿主), 组件不要自己复制一套。 */
    yyjson_doc *(*model_json_parse)(const char *data, size_t size,
        bool *normalized);
    /* 读整个文件(着色器源码/模型资源): 返回宿主分配的缓冲, 用 release 归还。 */
    void *(*read_file)(const char *path, size_t *size);
    /* Vulkan 能力面: 设备/队列/命令池等只在宿主手里, 组件拿到的是一份**只读快照**
       (BongoCatVkNativeInfo 是纯 POD, 不含宿主私有指针)。宿主私有 BongoCatVk* 绝不跨界。 */
    bool (*vulkan_native_info)(BongoCatVkNativeInfo *info);
} BongoCatRendererModuleHost;

typedef enum BongoCatRendererHostDirectory {
    BONGO_CAT_RENDERER_DIR_CONFIG = 0,
    BONGO_CAT_RENDERER_DIR_DATA,
    BONGO_CAT_RENDERER_DIR_MODELS,
    BONGO_CAT_RENDERER_DIR_CACHE,
    BONGO_CAT_RENDERER_DIR_STATE,
    BONGO_CAT_RENDERER_DIR_LOGS
} BongoCatRendererHostDirectory;

/* 组件回调。按"必备 / 可选"两段排列: 前面 7 个必须非空(宿主会拒绝缺任何一项的描述符),
   后面的全部可以留空, 宿主逐项判空并回落到诊断实现。 */
typedef struct BongoCatRendererModuleOps {
    /* —— 必备 —— */
    /* 建实例。asset_root 是宿主解出来的资源根(着色器等), 组件不得假设它可写。 */
    void *(*create)(const BongoCatRendererModuleHost *host, const char *asset_root,
        BongoCatError *error);
    /* 销毁实例: 必须在卸载组件之前调用(见纪律 5 与 model.h 的生命周期约束)。 */
    void (*destroy)(void *instance);
    BongoCatResult (*load)(void *instance, const char *model_dir,
        const char *setting_file, bool preset,
        const BongoCatLive2DRenderOptions *render_options,
        BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error);
    bool (*ready)(const void *instance);
    bool (*update)(void *instance, float delta_seconds);
    /* 画一帧。backend 是宿主当前生效的后端(OPENGL/VULKAN), 组件据此选渲染路径。 */
    void (*draw)(void *instance);
    void (*resize)(void *instance, int width, int height);

    /* —— 可选(判空调用) —— */
    void (*reshape)(void *instance, int width, int height);
    bool (*canvas_size)(const void *instance, int *width, int *height);
    bool (*frame)(const void *instance, BongoCatLive2DFrame *frame);
    bool (*viewport)(const void *instance, int *x, int *y, int *width, int *height);
    void (*set_mirror)(void *instance, bool mirror);
    void (*set_vertical_flip)(void *instance, bool flipped);
    void (*set_render_options)(void *instance,
        const BongoCatLive2DRenderOptions *options);
    void (*set_dragging)(void *instance, float x, float y);
    void (*set_centered_dragging)(void *instance, float x, float y);
    bool (*set_parameter)(void *instance, const char *id, float value);
    bool (*parameter)(void *instance, const char *id, BongoCatParameterRange *range);
    bool (*start_motion)(void *instance, const char *group, int index);
    bool (*restore_motion_state)(void *instance, const char *group, int index);
    bool (*preview_motion)(void *instance, const char *group, int index);
    bool (*restore_motion_preview)(void *instance);
    bool (*commit_motion_preview)(void *instance, const char *group, int index);
    bool (*motion_selected)(const void *instance, const char *group, int index);
    bool (*motion_persistent)(const void *instance, const char *group, int index);
    bool (*motion_visible)(const void *instance, const char *group, int index);
    bool (*motion_same_toggle)(const void *instance, const char *group,
        int left_index, const char *right_group, int right_index);
    bool (*set_expression)(void *instance, int index);
    int (*expression)(const void *instance);
    bool (*visual_state)(const void *instance, BongoCatLive2DVisualState *state);
    bool (*prepare_cover_capture)(void *instance);
    /* 诊断: 让模型按"查看器"的固定输入步进(审计场景用)。 */
    void (*prepare_viewer_audit)(void *instance);
    /* 分步加载会话: 换模型是渐进过程(解码/上传/建纹理), 宿主要能在其间继续出帧。
       会话句柄对宿主是不透明指针; 下面三个返回的 int 与宿主 BongoCatLive2DStepResult 的取值
       一一对应(0=working 1=done 2=failed), 这样两边不必共享枚举定义。 */
    void *(*load_session_begin)(void *instance, const char *model_dir,
        const char *setting_file, bool preset,
        const BongoCatLive2DRenderOptions *render_options,
        BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error);
    int (*load_session_activate)(void *session, void *window, void *context,
        BongoCatError *error);
    int (*load_session_step)(void *session, const char *model_dir,
        const char *setting_file, BongoCatError *error);
    void (*load_session_end)(void *session);
    void (*load_session_abort)(void *session);
} BongoCatRendererModuleOps;

/* 描述符: 组件唯一的导出符号返回它。字段顺序是 ABI 的一部分, 不得重排。 */
typedef struct BongoCatRendererModule {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t engine;
    uint32_t flags;
    uint32_t graphics_backends;
    const char *name;   /* 给人看的名字(日志/UI), 必须是静态字符串 */
    BongoCatRendererModuleOps ops;
} BongoCatRendererModule;

/* 组件必须导出这一个符号: 传入宿主的 ABI 版本与服务表, 返回描述符或 NULL。
   返回 NULL 表示"我配合不了这个宿主"(版本不符/缺服务), 宿主会记录下来并回落诊断实现。 */
typedef const BongoCatRendererModule *(*BongoCatRendererModuleQuery)(
    uint32_t abi, const BongoCatRendererModuleHost *host);

/* 组件侧导出宏: 必须加在 query 函数上。Windows 上不加 __declspec(dllexport) 就等于"没有
   这个符号"(宿主会报"不是 BongoCat 渲染组件"), 这是本 ABI 最容易踩的一脚。 */
#if defined(_WIN32)
#define BONGO_CAT_RENDERER_MODULE_EXPORT __declspec(dllexport)
#else
#define BONGO_CAT_RENDERER_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif
