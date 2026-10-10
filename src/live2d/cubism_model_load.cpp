#include "cubism_model.hpp"
#include "cubism_viewer_look.hpp"
#include "bongo_cat/file.h"
#include "bongo_cat/image.h"
#include "bongo_cat/json.h"

#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Id/CubismIdManager.hpp>
#include <Motion/CubismExpressionUpdater.hpp>
#include <Motion/CubismBreathUpdater.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/CubismEyeBlinkUpdater.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismPhysicsUpdater.hpp>
#include <Motion/CubismPoseUpdater.hpp>
#include <Motion/ICubismUpdater.hpp>
#include <Physics/CubismPhysics.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

namespace bongo_cat {

struct TextureProgressContext {
    BongoCatLive2DLoadProgress callback;
    void *userdata;
    float start;
    float span;
};

static void texture_progress(void *userdata, float progress) {
    auto *context = static_cast<TextureProgressContext *>(userdata);
    if (context && context->callback)
        context->callback(context->userdata,
            context->start + context->span * progress);
}

NativeModel::NativeModel() {
    _mocConsistency = true;
    _motionConsistency = true;
}

/* 析构顺序是有意的: 先停动作、再删动作对象 (SDK 的队列里可能还持有它们的指针), 然后
 * 才释放渲染资源, 最后删 setting_ (模型数据引用过它)。 */
NativeModel::~NativeModel() {
    _motionManager->StopAllMotions();
    _expressionManager->StopAllMotions();
    for (auto &item : motions_) Csm::ACubismMotion::Delete(item.second);
    for (auto &item : expressions_) Csm::ACubismMotion::Delete(item.second);
    release_render_resources();
    delete setting_;
}

/* 一次性整读, 并且带上限: maximum 用来挡住"被改坏的 model3.json / moc3 声称自己有
 * 几个 GB"这类情况, 超限一律当读取失败。返回空 vector 表示失败, 调用方按失败处理。 */
std::vector<unsigned char> NativeModel::read(const std::string &file, size_t maximum) const {
    FILE *stream = bongo_cat_file_open(file.c_str(), "rb");
    if (!stream || std::fseek(stream, 0, SEEK_END) != 0) {
        if (stream) std::fclose(stream);
        return {};
    }
    long size = std::ftell(stream);
    if (size <= 0 || (size_t)size > maximum || std::fseek(stream, 0, SEEK_SET) != 0) {
        std::fclose(stream);
        return {};
    }
    std::vector<unsigned char> bytes((size_t)size);
    bool read = std::fread(bytes.data(), 1, (size_t)size, stream) == (size_t)size;
    std::fclose(stream);
    if (!read) return {};
    return bytes;
}

std::string NativeModel::path(const char *relative) const {
    return directory_ + (relative ? relative : "");
}

bool NativeModel::load(const char *directory, const char *setting_file,
    bool direct_textures, BongoCatLive2DLoadProgress progress, void *userdata,
    BongoCatError *error) {
    if (!directory || !setting_file) return false;
    visual_state_ready_ = false;
    visual_state_ = BongoCatLive2DVisualState{};
    direct_textures_ = direct_textures;
    directory_ = directory;
    if (!directory_.empty() && directory_.back() != '/' && directory_.back() != '\\')
        directory_ += '/';
    std::vector<unsigned char> json = read(path(setting_file), 4 * 1024 * 1024);
    if (json.empty()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot read model setting: %s", setting_file);
        return false;
    }
    /* 有些手工改过的 Mver 清单不是严格 JSON (注释、尾逗号、多一对 "] }"), 解析器会先按
     * 宽松规则救回来, 并把 normalized 标为真。这种"救回来"的文档必须重新序列化成严格
     * JSON 再交给 SDK —— SDK 自己的解析器不容忍这些瑕疵。 */
    bool normalized = false;
    yyjson_doc *document = bongo_cat_model_json_parse(
        reinterpret_cast<const char *>(json.data()), json.size(), &normalized);
    if (document && normalized) {
        size_t size = 0;
        char *canonical = yyjson_write(document, 0, &size);
        if (!canonical) {
            yyjson_doc_free(document);
            bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
                "Cannot prepare model setting: %s", setting_file);
            return false;
        }
        json.assign(canonical, canonical + size);
        std::free(canonical);
    }
    yyjson_doc_free(document);
    if (!validate_model_setting_json(json, setting_file, error)) return false;
    if (progress) progress(userdata, .10f);
    setting_ = new(std::nothrow)
        Csm::CubismModelSettingJson(json.data(), (Csm::csmSizeInt)json.size());
    if (!setting_) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Cannot allocate model setting");
        return false;
    }
    if (!load_model(error)) return false;
    if (progress) progress(userdata, .25f);
    load_expressions();
    if (progress) progress(userdata, .31f);
    load_effects();
    if (progress) progress(userdata, .35f);
    load_motions(progress, userdata);
    Csm::csmMap<Csm::csmString, Csm::csmFloat32> layout;
    setting_->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);
    save_parameters();
    if (progress) progress(userdata, .49f);
    return true;
}
bool NativeModel::load_model(BongoCatError *error) {
    const char *name = setting_->GetModelFileName();
    std::vector<unsigned char> bytes = read(path(name));
    if (bytes.empty()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot read moc3: %s", name);
        return false;
    }
    LoadModel(bytes.data(), (Csm::csmSizeInt)bytes.size(), true);
    if (!_model) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM, "Cubism rejected moc3: %s", name);
        return false;
    }
    /* 记下模型的出厂参数值, 它同时是"动作基线"的初值: 参数覆盖和动作恢复都需要一个确定
     * 的参照点, 而这是我们在加载期能直接读到的那一份。 */
    const size_t parameter_count = (size_t)_model->GetParameterCount();
    parameter_override_values_.assign(parameter_count, 0.0f);
    parameter_baseline_values_.resize(parameter_count);
    parameter_overrides_.assign(parameter_count, 0);
    for (size_t i = 0; i < parameter_count; ++i)
        parameter_baseline_values_[i] = _model->GetParameterValue((int)i);
    parameter_overrides_applied_ = false;
    return true;
}
void NativeModel::load_expressions() {
    expression_names_.resize((size_t)setting_->GetExpressionCount());
    for (int i = 0; i < setting_->GetExpressionCount(); ++i) {
        const char *name = setting_->GetExpressionName(i);
        std::vector<unsigned char> bytes = read(path(setting_->GetExpressionFileName(i)));
        if (bytes.empty()) continue;
        Csm::ACubismMotion *motion = LoadExpression(bytes.data(),
            (Csm::csmSizeInt)bytes.size(), name);
        if (!motion) continue;
        expressions_[name] = motion;
        expression_names_[(size_t)i] = name;
    }
    if (!expressions_.empty())
        _updateScheduler.AddUpdatableList(
            CSM_NEW Csm::CubismExpressionUpdater(*_expressionManager));
}

void NativeModel::load_effects() {
    if (setting_->GetPhysicsFileName()[0]) {
        auto bytes = read(path(setting_->GetPhysicsFileName()));
        if (!bytes.empty()) LoadPhysics(bytes.data(), (Csm::csmSizeInt)bytes.size());
        if (_physics) _updateScheduler.AddUpdatableList(
            CSM_NEW Csm::CubismPhysicsUpdater(*_physics));
    }
    if (setting_->GetPoseFileName()[0]) {
        auto bytes = read(path(setting_->GetPoseFileName()));
        if (!bytes.empty()) LoadPose(bytes.data(), (Csm::csmSizeInt)bytes.size());
        if (_pose) _updateScheduler.AddUpdatableList(CSM_NEW Csm::CubismPoseUpdater(*_pose));
    }
    if (setting_->GetUserDataFile()[0]) {
        auto bytes = read(path(setting_->GetUserDataFile()));
        if (!bytes.empty()) LoadUserData(bytes.data(), (Csm::csmSizeInt)bytes.size());
    }
    viewer_look_ = CSM_NEW ViewerLookUpdater(*_model);
    _updateScheduler.AddUpdatableList(viewer_look_);
    /* 呼吸参数表沿用 SDK 示例的标定值 (幅度/周期/相位), 它决定待机时"呼吸感"的强弱,
     * 不是随手取的数 —— 要改先和示例比对。 */
    _breath = Csm::CubismBreath::Create();
    if (_breath) {
        Csm::csmVector<Csm::CubismBreath::BreathParameterData> parameters;
        Csm::CubismIdManager *ids = Csm::CubismFramework::GetIdManager();
        parameters.PushBack(Csm::CubismBreath::BreathParameterData(
            ids->GetId("ParamAngleX"), 0.0f, 15.0f, 6.5345f, 0.5f));
        parameters.PushBack(Csm::CubismBreath::BreathParameterData(
            ids->GetId("ParamAngleY"), 0.0f, 8.0f, 3.5345f, 0.5f));
        parameters.PushBack(Csm::CubismBreath::BreathParameterData(
            ids->GetId("ParamAngleZ"), 0.0f, 10.0f, 5.5345f, 0.5f));
        parameters.PushBack(Csm::CubismBreath::BreathParameterData(
            ids->GetId("ParamBodyAngleX"), 0.0f, 4.0f, 15.5345f, 0.5f));
        parameters.PushBack(Csm::CubismBreath::BreathParameterData(
            ids->GetId("ParamBreath"), 0.5f, 0.5f, 3.2345f, 0.5f));
        _breath->SetParameters(parameters);
        _updateScheduler.AddUpdatableList(
            CSM_NEW Csm::CubismBreathUpdater(*_breath));
    }
    add_parameter_override_updater();
    for (int i = 0; i < setting_->GetEyeBlinkParameterCount(); ++i)
        eye_blink_ids_.PushBack(setting_->GetEyeBlinkParameterId(i));
    for (int i = 0; i < setting_->GetLipSyncParameterCount(); ++i)
        lip_sync_ids_.PushBack(setting_->GetLipSyncParameterId(i));
    if (setting_->GetEyeBlinkParameterCount() > 0) {
        _eyeBlink = Csm::CubismEyeBlink::Create(setting_);
        _updateScheduler.AddUpdatableList(
            CSM_NEW Csm::CubismEyeBlinkUpdater(motion_updated_, *_eyeBlink));
    }
    _updateScheduler.SortUpdatableList();
}

void NativeModel::load_motions(BongoCatLive2DLoadProgress progress,
    void *userdata) {
    idle_motion_keys_.clear();
    motion_signatures_.clear();
    motion_states_.clear();
    motion_toggle_partners_.clear();
    motion_toggle_owners_.clear();
    selected_motion_keys_.clear();
    clear_motion_runs();
    int total = 0, completed = 0;
    for (int group_index = 0; group_index < setting_->GetMotionGroupCount();
        ++group_index)
        total += setting_->GetMotionCount(setting_->GetMotionGroupName(group_index));
    for (int group_index = 0; group_index < setting_->GetMotionGroupCount(); ++group_index) {
        const char *group = setting_->GetMotionGroupName(group_index);
        for (int i = 0; i < setting_->GetMotionCount(group); ++i) {
            if (progress) progress(userdata, .35f + .14f *
                (float)(++completed) / (float)(total > 0 ? total : 1));
            auto bytes = read(path(setting_->GetMotionFileName(group, i)));
            if (bytes.empty()) continue;
            std::string key = std::string(group) + "_" + std::to_string(i);
            auto *motion = static_cast<Csm::CubismMotion *>(LoadMotion(bytes.data(),
                (Csm::csmSizeInt)bytes.size(), key.c_str(), nullptr, nullptr,
                setting_, group, i, _motionConsistency));
            if (!motion) continue;
            motion->SetEffectIds(eye_blink_ids_, lip_sync_ids_);
            motions_[key] = motion;
            load_motion_state(key, group, i, bytes);
            if (std::strcmp(group, "Idle") == 0) idle_motion_keys_.push_back(key);
        }
    }
    pair_motion_states();
    _motionManager->StopAllMotions();
}

bool NativeModel::load_textures(BongoCatError *error,
    BongoCatLive2DLoadProgress progress, void *userdata) {
    release_textures();
    int count = setting_->GetTextureCount();
    textures_.assign((size_t)count, 0);
    /*
     * 必须在创建贴图之前挂载: Vulkan 分支的贴图上传要用后端句柄, 而挂载本身
     * 依赖 CubismFramework 已启动 (这里已满足 —— 模型加载发生在框架启动之后)。
     * 之前把挂载放在 create_renderer 里, 比贴图创建晚, 于是贴图失败、整个模型
     * 加载失败、渲染器永远不会被创建。
     */
    if (bongo_cat_cubism_renderer_uses_vulkan() && !bongo_cat_live2d_vulkan_ready())
        bongo_cat_live2d_vulkan_attach(NULL, 0, 0);
    vulkan_textures_.assign((size_t)count, nullptr);
    texture_alpha_.assign((size_t)count, {});
    TextureProgressContext texture_context = {progress, userdata, .50f,
        .45f / (float)(count > 0 ? count : 1)};
    for (int i = 0; i < count; ++i) {
        texture_context.start = .50f + .45f * (float)i /
            (float)(count > 0 ? count : 1);
        if (bongo_cat_cubism_renderer_uses_vulkan()) {
            BongoCatImage image = {0};
            const std::string texture_path = path(setting_->GetTextureFileName(i));
            if (bongo_cat_image_load(texture_path.c_str(), &image, error) !=
                    BONGO_CAT_OK || !image.pixels) {
                release_textures();
                return false;
            }
            /*
             * 贴图的 alpha 掩码必须在这里生成: 它是纯 CPU 数据, 但后面
             * prepare_expression_frame 要靠它剔除全透明的三角形来算"可见包络",
             * 包络又决定窗口的 frame 内边距 —— 少了它, 透明画布会被当成可见内容,
             * 窗口按错误的尺寸开出来, 模型看起来就没跟着缩放走。
             * GL 分支由 bongo_cat_image_texture_model 顺手填好, Vulkan 分支
             * 之前一直是空的。
             */
            bongo_cat_image_make_alpha_mask(&image, &texture_alpha_[(size_t)i]);
            vulkan_textures_[(size_t)i] = bongo_cat_live2d_vulkan_create_texture_attached(
                image.pixels, image.width, image.height);
            bongo_cat_image_free(&image);
            if (!vulkan_textures_[(size_t)i]) {
                release_textures();
                return false;
            }
            if (progress) progress(userdata, .50f + .45f * (float)(i + 1) /
                (float)(count > 0 ? count : 1));
            continue;
        }
        textures_[(size_t)i] = bongo_cat_image_texture_model(
            path(setting_->GetTextureFileName(i)).c_str(), direct_textures_,
            nullptr, nullptr, &texture_alpha_[(size_t)i],
            progress ? texture_progress : nullptr, &texture_context, error);
        if (!textures_[(size_t)i]) {
            release_textures();
            return false;
        }
        if (progress) progress(userdata, .50f + .45f * (float)(i + 1) /
            (float)(count > 0 ? count : 1));
    }
    prepare_expression_frame();
    /* 换贴图必须重建渲染器: 渲染器持有贴图绑定与遮罩缓冲, 不重建就会继续引用旧句柄。 */
    release_renderer();
    if (!create_renderer(error)) {
        release_textures();
        return false;
    }
    renderer_width_ = width_;
    renderer_height_ = height_;
    return true;
}
} // namespace bongo_cat
