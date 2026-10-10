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

#endif
