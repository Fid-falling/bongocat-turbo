#include "cubism_model.hpp"
#include "cubism_viewer_look.hpp"
#include "live2d_vulkan_internal.h"

#include <Id/CubismIdManager.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Model/CubismModel.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cmath>
namespace bongo_cat {

void NativeModel::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    width_ = width;
    height_ = height;
    update_viewport();
    if (!_model || (width == renderer_width_ && height == renderer_height_)) return;
    if (!_model->IsBlendModeEnabled()) {
        renderer_width_ = width_;
        renderer_height_ = height_;
        return;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
        "Live2D render target resized in place: %dx%d -> %dx%d",
        renderer_width_, renderer_height_, width_, height_);
    /*
     * SDK 内部的模型/遮罩目标要和桥的超采样目标同尺寸, 否则遮罩与成品对不上。
     * 注意桥的离屏目标就是 2 倍分辨率 (见 live2d_vulkan_internal.h 的
     * BONGO_CAT_LIVE2D_VULKAN_SUPERSAMPLE), 合成时再线性缩小回窗口。
     */
    /* 倍数必须取自和桥同一个函数 bongo_cat_live2d_vulkan_supersample_for(): 桥用它定
     * 离屏目标, 这里用它定 SDK 的模型/遮罩目标, 两处一旦不一致遮罩就会和成品错位。 */
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat_cubism_renderer_uses_vulkan()) {
        SetRenderTargetSize(
            (Csm::csmUint32)(width_ * bongo_cat_live2d_vulkan_supersample_for(width_, height_)),
            (Csm::csmUint32)(height_ * bongo_cat_live2d_vulkan_supersample_for(width_, height_)));
    } else
#endif
    SetRenderTargetSize((Csm::csmUint32)width_, (Csm::csmUint32)height_);
    renderer_width_ = width_;
    renderer_height_ = height_;
}

/*
 * reshape 与 resize 的分工: reshape 只更新画布尺寸并重算内容视口, resize 还会把新尺寸
 * 推给 SDK 的重绘目标。加载期用 reshape (那时渲染器和贴图都还不存在, 推给 SDK 没意义),
 * 之后每次窗口变化走 resize。
 */
void NativeModel::reshape(int width, int height) {
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
        update_viewport();
    }
}

/* 用一份浮点快照做脏检查: 参数/部件数量可能变 (换模型), 所以先比数组尺寸再比数值。
 * 1e-5 的容差是留给 SDK 插值噪声的余量, 太小会让静止的猫每帧都被判成"变了"。 */
template<typename Getter>
static bool changed(std::vector<float> &snapshot, int count, Getter value) {
    bool result = snapshot.size() != (size_t)count;
    if (result) snapshot.resize((size_t)count);
    for (int i = 0; i < count; ++i) {
        float current = value(i);
        if (result || std::fabs(snapshot[(size_t)i] - current) > 0.00001f) result = true;
        snapshot[(size_t)i] = current;
    }
    return result;
}

/*
 * 返回值是"这一帧有没有可见变化"的脏标记, 上层据此决定要不要重绘 (静止的猫不必每帧
 * 重画)。delta 上限 0.25s 是为了让窗口拖动/系统卡顿后的补帧不会一步跳过大段动画 ——
 * 宁可慢放也不要抽搐。
 */
bool NativeModel::update(float delta_seconds) {
    if (!_model || delta_seconds <= 0.0f) return false;
    if (delta_seconds > 0.25f) delta_seconds = 0.25f;
    motion_updated_ = suppress_eye_blink_;
    _model->LoadParameters();
    parameter_overrides_applied_ = false;
    if (automatic_idle_ && _motionManager->IsFinished())
        start_idle_motion();
    else if (!_motionManager->IsFinished())
        motion_updated_ = _motionManager->UpdateMotion(_model, delta_seconds);
    expire_motion_runs();
    save_parameters();
    _updateScheduler.OnLateUpdate(_model, delta_seconds);
    expression_frame_pending_ = false;
    expire_expression_fade();
    apply_parameter_overrides();
    _opacity = _model->GetModelOpacity();
    _model->Update();
    bool result = changed(parameter_snapshot_, _model->GetParameterCount(),
        [this](int index) { return _model->GetParameterValue(index); });
    result = changed(part_snapshot_, _model->GetPartCount(),
        [this](int index) { return _model->GetPartOpacity(index); }) || result;
    if (std::fabs(opacity_snapshot_ - _opacity) > 0.00001f) result = true;
    opacity_snapshot_ = _opacity;
    return result;
}

/*
 * 整体旋转 (度): 绕画布中心转, 并按 1/(|cos|+|sin|) 缩小。
 *
 * 为什么两件事必须成对: 旋转后模型的外接矩形变成 |cos|+|sin| 倍, 窗口会同步放大这么多
 * (见 window_scale.c); 如果模型投影不缩小回去, 视觉上就变成"边转边放大"。反过来只缩窗口
 * 不缩模型则会裁掉四角。
 *
 * 为什么往模型空间里乘: CubismMatrix44 是行主序 + 列向量, MultiplyByMatrix(m) 得到
 * m·P, 也就是 m 先作用 —— 在画布空间(各向同性)旋转, 不会被非方形窗口的纵横比拉斜。
 * 旋转中心是画布原点 (Live2D 的画布坐标以中心为 0,0)。
 */
static void apply_rotation(Csm::CubismMatrix44 &projection, float degrees) {
    if (fabsf(degrees) < 0.01f) return;
    const float rad = degrees * 3.14159265358979f / 180.0f;
    const float c = cosf(rad), s = sinf(rad);
    const float ext = fabsf(c) + fabsf(s);
    const float inv = ext > 0.0001f ? 1.0f / ext : 1.0f;
    Csm::CubismMatrix44 rot;
    rot.LoadIdentity();
    float *m = rot.GetArray();
    /* 行主序: [0][1] 是第二列... 这里直接按 2D 旋转 + 等比缩放填前两行两列 */
    m[0] = c * inv;
    m[1] = -s * inv;
    m[4] = s * inv;
    m[5] = c * inv;
    projection.MultiplyByMatrix(&rot);
}

void NativeModel::build_projection(Csm::CubismMatrix44 &projection,
    int width, int height) {
    if (!_model || width <= 0 || height <= 0) return;
    if (render_options_.mver_projection) {
        float aspect = (float)render_options_.reference_width /
            (float)render_options_.reference_height;
        projection.Scale((mirror_ ? -1.0f : 1.0f) *
            render_options_.projection_scale,
            render_options_.projection_scale * aspect);
        projection.Translate(render_options_.offset_x, render_options_.offset_y);
        projection.MultiplyByMatrix(_modelMatrix);
    } else {
        /* 与 X 一致: 只在局部拷贝上 SetWidth, 避免永久污染成员 _modelMatrix。 */
        Csm::CubismModelMatrix model_matrix(*_modelMatrix);
        if (_model->GetCanvasWidth() > 1.0f && width < height) {
            model_matrix.SetWidth(2.0f);
            projection.Scale(mirror_ ? -1.0f : 1.0f,
                (float)width / (float)height);
        } else {
            projection.Scale((mirror_ ? -1.0f : 1.0f) *
                (float)height / (float)width, 1.0f);
        }
        projection.MultiplyByMatrix(&model_matrix);
    }
    if (render_options_.mver_projection) {
        // Mver 0.1.6's Core reports canvas units at twice the modern scale.
        // Preserve authored Layout translation while matching its model scale.
        float *matrix = projection.GetArray();
        constexpr float mver_core_canvas_scale = 0.5f;
        matrix[0] *= mver_core_canvas_scale;
        matrix[1] *= mver_core_canvas_scale;
        matrix[4] *= mver_core_canvas_scale;
        matrix[5] *= mver_core_canvas_scale;
    }
    apply_rotation(projection, rotation_deg_);
}

void NativeModel::draw() {
    if (!_model || width_ <= 0 || height_ <= 0) return;
    /* Vulkan: 桥负责每帧的离屏目标切换与命令提交 (SDK 渲染器自行提交) */
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat_cubism_renderer_uses_vulkan()) {
        auto *vulkan = GetRenderer<Csm::Rendering::CubismRenderer_Vulkan>();
        if (!vulkan || !bongo_cat_live2d_vulkan_begin_frame()) return;
        Csm::CubismMatrix44 projection;
        build_projection(projection, viewport_width_, viewport_height_);
        reflect_vertical_flip(projection);
        apply_viewport_projection(projection);
        visual_state_ = BongoCatLive2DVisualState{};
        visual_state_.fit_scale = 1.0f;
        visual_state_.mver_projection = render_options_.mver_projection;
        record_visible_state(projection);
        visual_state_ready_ = true;
        vulkan->SetMvpMatrix(&projection);
        vulkan->DrawModel();
        bongo_cat_live2d_vulkan_end_frame();
        return;
    }
#endif
#ifdef CSM_TARGET_MAC_GL
    CoreProfileBinding binding(core_buffers_);
#endif
    update_mask_buffers();
    auto *manager = Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance();
    manager->BeginFrameProcess();
    Csm::CubismMatrix44 projection;
    build_projection(projection, viewport_width_, viewport_height_);
    reflect_vertical_flip(projection);
    apply_viewport_projection(projection);
    visual_state_ = BongoCatLive2DVisualState{};
    visual_state_.fit_scale = 1.0f;
    visual_state_.mver_projection = render_options_.mver_projection;
    record_visible_state(projection);
    visual_state_ready_ = true;
    auto *renderer = GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    renderer->SetMvpMatrix(&projection);
    renderer->DrawModel();
    manager->EndFrameProcess();
}

void NativeModel::set_mirror(bool mirror) { mirror_ = mirror; }

void NativeModel::set_vertical_flip(bool flipped) { vertical_flip_ = flipped; }

void NativeModel::set_rotation(float degrees) {
    /* 只在真正变化时记, 调用方每帧都会下发 */
    if (rotation_deg_ == degrees) return;
    rotation_deg_ = degrees;
}

void NativeModel::set_render_options(const BongoCatLive2DRenderOptions &options) {
    render_options_ = options;
}

void NativeModel::set_dragging(float x, float y, bool angle_z) {
    if (!viewer_look_) return;
    x = std::max(-1.0f, std::min(1.0f, x));
    y = std::max(-1.0f, std::min(1.0f, y));
    viewer_look_->set_target(x, y, angle_z);
}

void NativeModel::prepare_viewer_audit() {
    if (!_model) return;
    automatic_idle_ = false;
    suppress_eye_blink_ = true;
    _motionManager->StopAllMotions();
    clear_motion_runs();
    std::fill(parameter_overrides_.begin(), parameter_overrides_.end(), 0);
    std::fill(parameter_override_values_.begin(),
        parameter_override_values_.end(), 0.0f);
    parameter_overrides_applied_ = false;
    for (int i = 0; i < _model->GetParameterCount(); ++i) {
        _model->SetParameterValue(i, _model->GetParameterDefaultValue(i));
    }
    save_parameters();
}

bool NativeModel::prepare_cover_capture() {
    if (!_model) return false;
    settle_pending_expression_for_cover();
    apply_parameter_overrides();
    /* Persistent motions are already stored in the Cubism baseline. Updating
       drawables does not advance idle motion, physics, breathing, or blinking. */
    _model->Update();
    return true;
}

bool NativeModel::set_parameter(const char *id, float value) {
    if (!_model || !id) return false;
    Csm::CubismIdHandle handle = Csm::CubismFramework::GetIdManager()->GetId(id);
    int index = _model->GetParameterIndex(handle);
    if (index < 0 || index >= _model->GetParameterCount()) return false;
    parameter_override_values_[(size_t)index] = value;
    parameter_overrides_[(size_t)index] = 1;
    if (parameter_overrides_applied_)
        _model->SetParameterValue(index, value);
    else apply_parameter_overrides();
    return true;
}

bool NativeModel::parameter(const char *id, float *minimum, float *maximum, float *value) {
    if (!_model || !id) return false;
    Csm::CubismIdHandle handle = Csm::CubismFramework::GetIdManager()->GetId(id);
    int index = _model->GetParameterIndex(handle);
    if (index < 0 || index >= _model->GetParameterCount()) return false;
    if (minimum) *minimum = _model->GetParameterMinimumValue(index);
    if (maximum) *maximum = _model->GetParameterMaximumValue(index);
    if (value) *value = _model->GetParameterValue(index);
    return true;
}

} // namespace bongo_cat
