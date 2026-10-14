#ifndef BONGO_CAT_CUBISM_RUNTIME_HPP
#define BONGO_CAT_CUBISM_RUNTIME_HPP

#include "cubism_model.hpp"

/*
 * C 侧看到的"Live2D 运行时"就是这个结构: 当前模型 + 退休队列 + 最近一次画布尺寸。
 * 它是 POD 聚合, 由 cubism_bridge.cpp 用 new 建、cubism_bridge_load.cpp 填 model,
 * 所以别在这里加需要构造/析构的成员 (会被 C 侧的 delete 语义绕开)。
 */
struct BongoCatRetiredModel {
    bongo_cat::NativeModel *model = nullptr;
    unsigned frames_remaining = 0;
};

struct BongoCatLive2D {
    /* 退休队列长度: 连续快速换模型时攒下的旧模型数量上限, 见 cubism_bridge_load.cpp */
    static constexpr unsigned retired_capacity = 4;
    bongo_cat::NativeModel *model;
    BongoCatRetiredModel retired[retired_capacity]{};
    unsigned retired_count = 0;
    /* 默认值就是内置 Mver 模型的授权画布尺寸: 模型还没加载完成时, 窗口与视口都按它算,
     * 这样加载期间不会出现尺寸为 0 的中间态。 */
    int width = 612;
    int height = 354;
};

/*
 * 可分步的模型加载会话。
 *
 * 为什么需要它: 一次加载要 1~5 秒 (读 moc3/动作/贴图 + 建渲染器), 整段同步跑完就是用户看到
 * 的"切换模型时整个程序卡住"。于是把同一件事拆成"每次只做一小步", 由调用方在主循环里按时间
 * 预算推进 —— 两次调用之间主循环照常出帧、设置窗口照常收输入。
 *
 * 不变量 (改这里之前先读):
 *   - pending 是新模型, 在提交之前**绝不**写 runtime->model: 旧模型必须一直能画, 这样加载
 *     失败时画面保持原样, 而不是变黑或半加载;
 *   - 每一步都必须可中断: 游标记在 session 里、动作/贴图下标记在 NativeModel 里, 不许有
 *     跨步的栈上临时状态;
 *   - 只有 OpenGL 后端需要临时切换上下文, 且两条出口 (session_end / abort) 都要还原;
 *   - Vulkan 的贴图上传与离屏目标重建走的是后端自己的命令池, 且都在**主线程**上执行 ——
 *     分步不引入第二个线程, 因此不需要新的锁, 与后端"设备就绪/共享"的既有约束一致。
 *
 * 定义在 cubism_bridge_load_session.cpp (C 侧只通过 bongo_cat/model.h 的不透明句柄看到它),
 * 所以这里只声明。
 */
struct BongoCatLive2DLoadSession;

/* 分步加载的游标。跨多次 step 调用存活。 */
enum class LoadCursor {
    kSettingsRead,      /* settings_begin: 读 model3.json + 宽松 JSON 归一化 */
    kSettingsApply,     /* settings_finish: 校验 + 建 setting_ + moc3 + 表情/效果 */
    kMotions,           /* 一次一个动作文件 */
    kTexturesBegin,     /* 建贴图数组 + (Vulkan) 挂载后端句柄 */
    kTextures,          /* 一次一张贴图 (解码 + 上传) */
    kTexturesMask,      /* 一次一张贴图的 alpha 掩码 (纯 CPU, 与上传分开以缩短单步) */
    kFinalize,          /* 可见包络 + 重建渲染器 + 提交给 runtime */
    kDone,
    kFailed
};

/* 游标的个数 (含 kFailed): 诊断计时数组按它开大小。放在枚举之后, 加游标时不会忘记同步。 */
constexpr unsigned kLoadStageCount = (unsigned)LoadCursor::kFailed + 1;

/* 各阶段的中文名, 只用于诊断日志。顺序必须与 LoadCursor 一致。 */
const char *bongo_cat_live2d_load_stage_name(LoadCursor cursor);
/* 分段耗时日志 (bongo_cat_live2d_load_session_log_timing) 的声明在 bongo_cat/model.h 里:
 * 它的调用方是 runtime 层的 C 代码, 必须走 extern "C" 那一套。 */

/* 下面两个是 C++ 内部工具 (定义在 cubism_bridge_load_session.cpp), 不是 C API:
 * report 只在步与步之间播报进度 (用途是让上层画一帧"正在切换"的中间态);
 * discard 释放 pending 模型并还原上下文, 失败路径靠它不留半成品。 */
void bongo_cat_live2d_session_report(BongoCatLive2DLoadSession *session,
    float progress);
void bongo_cat_live2d_session_discard(BongoCatLive2DLoadSession *session);

#endif
