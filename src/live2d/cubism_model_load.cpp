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

bool NativeModel::settings_begin(const char *directory, const char *setting_file,
    BongoCatError *error) {
    setting_json_.clear();
    if (!directory || !setting_file) return false;
    visual_state_ready_ = false;
    visual_state_ = BongoCatLive2DVisualState{};
    directory_ = directory;
    if (!directory_.empty() && directory_.back() != '/' && directory_.back() != '\\')
        directory_ += '/';
    setting_json_ = read(path(setting_file), 4 * 1024 * 1024);
    if (setting_json_.empty()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot read model setting: %s", setting_file);
        return false;
    }
    /* 有些手工改过的 Mver 清单不是严格 JSON (注释、尾逗号、多一对 "] }"), 解析器会先按
     * 宽松规则救回来, 并把 normalized 标为真。这种"救回来"的文档必须重新序列化成严格
     * JSON 再交给 SDK —— SDK 自己的解析器不容忍这些瑕疵。 */
    bool normalized = false;
    yyjson_doc *document = bongo_cat_model_json_parse(
        reinterpret_cast<const char *>(setting_json_.data()), setting_json_.size(),
        &normalized);
    if (document && normalized) {
        size_t size = 0;
        char *canonical = yyjson_write(document, 0, &size);
        if (!canonical) {
            yyjson_doc_free(document);
            bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
                "Cannot prepare model setting: %s", setting_file);
            setting_json_.clear();
            return false;
        }
        setting_json_.assign(canonical, canonical + size);
        std::free(canonical);
    }
    yyjson_doc_free(document);
    return true;
}

/* settings_begin 之后必须调它一次: 校验 -> 建 setting_ -> 模型/表情/效果。动作仍由
 * motions_* 那三个原语负责, 这样"读动作文件"这段大开销才能被切碎。 */
bool NativeModel::settings_finish(BongoCatError *error) {
    if (setting_json_.empty()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Model setting was not read before finishing the load");
        return false;
    }
    if (!validate_model_setting_json(setting_json_,
            setting_ ? setting_->GetModelFileName() : "model3.json", error)) return false;
    setting_ = new(std::nothrow)
        Csm::CubismModelSettingJson(setting_json_.data(),
            (Csm::csmSizeInt)setting_json_.size());
    if (!setting_) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Cannot allocate model setting");
        return false;
    }
    if (!load_model(error)) return false;
    load_expressions();
    load_effects();
    return true;
}

bool NativeModel::load(const char *directory, const char *setting_file,
    bool direct_textures, BongoCatLive2DLoadProgress progress, void *userdata,
    BongoCatError *error) {
    direct_textures_ = direct_textures;
    if (!settings_begin(directory, setting_file, error)) return false;
    if (progress) progress(userdata, .10f);
    if (!settings_finish(error)) return false;
    if (progress) progress(userdata, .25f);
    if (progress) progress(userdata, .31f);
    if (progress) progress(userdata, .35f);
    motions_begin();
    while (motions_step())
        if (progress) progress(userdata, .35f + .14f *
            (float)motions_done_ / (float)(motion_total_ > 0 ? motion_total_ : 1));
    motions_end();
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

/* 进入"逐个读动作"的状态: 清空上一轮的动作状态, 记下总数与游标。真正的读取在
 * motions_step 里, 一次一个, 这样调用方能在两个动作之间出帧。 */
void NativeModel::motions_begin() {
    idle_motion_keys_.clear();
    motion_signatures_.clear();
    motion_states_.clear();
    motion_toggle_partners_.clear();
    motion_toggle_owners_.clear();
    selected_motion_keys_.clear();
    clear_motion_runs();
    motion_group_ = 0;
    motion_index_ = 0;
    motion_total_ = 0;
    motions_done_ = 0;
    if (!setting_) return;
    for (int group_index = 0; group_index < setting_->GetMotionGroupCount();
        ++group_index)
        motion_total_ += setting_->GetMotionCount(
            setting_->GetMotionGroupName(group_index));
}

/* 读一个动作。返回 false = 全部读完, 此时已经做了收尾 (配对动作状态并停掉播放队列)。
 * 游标只在"这一条处理完"之后前进, 所以中途返回也不会漏读或重读。 */
bool NativeModel::motions_step() {
    if (!setting_) return false;
    while (motion_group_ < setting_->GetMotionGroupCount()) {
        const char *group = setting_->GetMotionGroupName(motion_group_);
        if (motion_index_ >= setting_->GetMotionCount(group)) {
            motion_group_++;
            motion_index_ = 0;
            continue;
        }
        int index = motion_index_++;
        motions_done_++;
        auto bytes = read(path(setting_->GetMotionFileName(group, index)));
        if (!bytes.empty()) {
            std::string key = std::string(group) + "_" + std::to_string(index);
            auto *motion = static_cast<Csm::CubismMotion *>(LoadMotion(bytes.data(),
                (Csm::csmSizeInt)bytes.size(), key.c_str(), nullptr, nullptr,
                setting_, group, index, _motionConsistency));
            if (motion) {
                motion->SetEffectIds(eye_blink_ids_, lip_sync_ids_);
                motions_[key] = motion;
                load_motion_state(key, group, index, bytes);
                if (std::strcmp(group, "Idle") == 0) idle_motion_keys_.push_back(key);
            }
        }
        return true;
    }
    motions_end();
    return false;
}

void NativeModel::motions_end() {
    pair_motion_states();
    if (_motionManager) _motionManager->StopAllMotions();
    motion_group_ = 0;
    motion_index_ = 0;
}

/* 进入"逐个上传贴图"的状态。挂载必须在建贴图之前: Vulkan 分支的上传要用后端句柄,
 * 而挂载本身依赖 CubismFramework 已启动 (模型加载必然在框架启动之后)。 */
void NativeModel::textures_begin() {
    release_textures();
    texture_count_ = setting_ ? setting_->GetTextureCount() : 0;
    textures_.assign((size_t)texture_count_, 0);
    if (bongo_cat_cubism_renderer_uses_vulkan() && !bongo_cat_live2d_vulkan_ready())
        bongo_cat_live2d_vulkan_attach(NULL, 0, 0);
    vulkan_textures_.assign((size_t)texture_count_, nullptr);
    texture_alpha_.assign((size_t)texture_count_, {});
}

/* 上传第 index 张贴图。一次只做一张, 调用方负责限时 —— 解码一张图是这里最长的单步,
 * 所以"每帧一张"就是切换期间帧间隔的上界。失败会把这一轮已经建好的贴图整体释放。 */
bool NativeModel::textures_step(int index, BongoCatError *error) {
    if (!setting_ || index < 0 || index >= texture_count_) return true;
    if (bongo_cat_cubism_renderer_uses_vulkan()) {
        BongoCatImage image = {0};
        const std::string texture_path = path(setting_->GetTextureFileName(index));
        if (bongo_cat_image_load(texture_path.c_str(), &image, error) !=
                BONGO_CAT_OK || !image.pixels) {
            release_textures();
            return false;
        }
        /*
         * 掩码留到 textures_mask_step 里做: 它是纯 CPU 的一段 (逐像素), 和"解码 + 上传"
         * 分开之后每一步都更短, 于是主循环更容易在两步之间插进一帧。
         */
        pending_alpha_image_ = image;
        vulkan_textures_[(size_t)index] = bongo_cat_live2d_vulkan_create_texture_attached(
            image.pixels, image.width, image.height);
        if (!vulkan_textures_[(size_t)index]) {
            bongo_cat_image_free(&pending_alpha_image_);
            pending_alpha_image_ = BongoCatImage{0};
            release_textures();
            if (error && !error->message[0])
                bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
                    "Cannot upload Live2D texture %d to Vulkan", index);
            return false;
        }
        return true;
    }
    /* GL 路径的渐进回调只用来把"这一张图解码到哪了"报给 UI; 分步调用方不需要它,
     * 因为分步本身已经把"每帧一张图"变成了帧间隔的上界。掩码由它顺手填好。 */
    textures_[(size_t)index] = bongo_cat_image_texture_model(
        path(setting_->GetTextureFileName(index)).c_str(), direct_textures_,
        nullptr, nullptr, &texture_alpha_[(size_t)index], nullptr, nullptr, error);
    if (!textures_[(size_t)index]) {
        release_textures();
        return false;
    }
    return true;
}

/*
 * 为上一张上传的贴图生成 alpha 掩码, 然后立刻释放那幅 CPU 位图。
 *
 * 为什么必须生成: 后面 prepare_expression_frame 要靠它剔除全透明的三角形来算"可见包络",
 * 包络又决定窗口的 frame 内边距 —— 少了它, 透明画布会被当成可见内容, 窗口按错误的尺寸开
 * 出来, 模型看起来就没跟着缩放走。GL 分支由 bongo_cat_image_texture_model 顺手填好,
 * Vulkan 分支早先一直是空的。
 *
 * 幂等: 没有待处理的位图时直接返回, 所以调用方可以无条件按顺序推进。
 */
void NativeModel::textures_mask_step(int index) {
    if (!pending_alpha_image_.pixels) return;
    if (index >= 0 && index < texture_count_)
        bongo_cat_image_make_alpha_mask(&pending_alpha_image_,
            &texture_alpha_[(size_t)index]);
    bongo_cat_image_free(&pending_alpha_image_);
    pending_alpha_image_ = BongoCatImage{0};
}

/* 全部贴图就绪之后的收尾: 算可见包络, 然后重建渲染器 —— 渲染器持有贴图绑定与遮罩缓冲,
 * 不重建就会继续引用旧句柄。 */
bool NativeModel::textures_end(BongoCatError *error) {
    if (!setting_) return false;
    prepare_expression_frame();
    release_renderer();
    if (!create_renderer(error)) {
        release_textures();
        return false;
    }
    renderer_width_ = width_;
    renderer_height_ = height_;
    return true;
}

bool NativeModel::load_textures(BongoCatError *error,
    BongoCatLive2DLoadProgress progress, void *userdata) {
    textures_begin();
    const int count = texture_count_;
    TextureProgressContext texture_context = {progress, userdata, .50f,
        .45f / (float)(count > 0 ? count : 1)};
    for (int i = 0; i < count; ++i) {
        texture_context.start = .50f + .45f * (float)i /
            (float)(count > 0 ? count : 1);
        /* 渐进回调是"这一张图解码到哪了", 只在 GL 路径上有 (它内部分块解码);
         * Vulkan 路径一张图是一次 SDL_LoadFile + 一次上传, 没有中间态可报。 */
        if (!bongo_cat_cubism_renderer_uses_vulkan()) {
            textures_[(size_t)i] = bongo_cat_image_texture_model(
                path(setting_->GetTextureFileName(i)).c_str(), direct_textures_,
                nullptr, nullptr, &texture_alpha_[(size_t)i],
                progress ? texture_progress : nullptr, &texture_context, error);
            if (!textures_[(size_t)i]) {
                release_textures();
                return false;
            }
        } else if (!textures_step(i, error)) {
            return false;
        }
        if (progress) progress(userdata, .50f + .45f * (float)(i + 1) /
            (float)(count > 0 ? count : 1));
    }
    return textures_end(error);
}
} // namespace bongo_cat
