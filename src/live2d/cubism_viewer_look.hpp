#ifndef BONGO_CAT_CUBISM_VIEWER_LOOK_HPP
#define BONGO_CAT_CUBISM_VIEWER_LOOK_HPP

#include <Motion/ICubismUpdater.hpp>

namespace bongo_cat {

/*
 * 注视跟随的更新器。
 *
 * 它挂在 SDK 的更新调度器上 (CubismUpdateOrder_Look 顺序), 每帧由 SDK 回调, 不需要外部
 * 驱动, 也不会和物理/呼吸/眨眼的顺序打架 —— 顺序由 SDK 统一决定。
 *
 * 阻尼模型的状态 (face_/velocity_) 必须留在这里而不是参数上: 参数只是每帧的输出, 真正的
 * "惯性"得由我们自己维护, 否则鼠标一停头也会立刻停住。
 */
class ViewerLookUpdater final : public Csm::ICubismUpdater {
public:
    explicit ViewerLookUpdater(Csm::CubismModel &model);
    void set_target(Csm::csmFloat32 x, Csm::csmFloat32 y,
        bool angle_z_enabled);
    void OnLateUpdate(Csm::CubismModel *model,
        Csm::csmFloat32 delta_seconds) override;

private:
    enum class Axis { X, Y, Z };
    struct Parameter {
        Csm::csmInt32 index;
        Axis axis;
        Csm::csmFloat32 weight;
    };

    void add_parameter(Csm::CubismModel &model, const char *id, Axis axis);
    void update_target(Csm::csmFloat32 delta_seconds);
    Csm::csmFloat32 x() const;
    Csm::csmFloat32 y() const;

    Parameter parameters_[6]{};
    Csm::csmInt32 parameter_count_ = 0;
    Csm::csmFloat32 target_x_ = 0.0f;
    Csm::csmFloat32 target_y_ = 0.0f;
    Csm::csmFloat32 face_x_ = 0.0f;
    Csm::csmFloat32 face_y_ = 0.0f;
    Csm::csmFloat32 velocity_x_ = 0.0f;
    Csm::csmFloat32 velocity_y_ = 0.0f;
    bool angle_z_enabled_ = false;
    bool initialized_ = false;
};

} // namespace bongo_cat

#endif
