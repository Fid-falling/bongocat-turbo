#ifndef BONGO_CAT_CUBISM_MODEL_HPP
#define BONGO_CAT_CUBISM_MODEL_HPP

#ifdef CSM_TARGET_MAC_GL
#include "cubism_core_profile.hpp"
#endif

#include "bongo_cat/model.h"
#include "bongo_cat/image.h"
#include "bongo_cat/live2d_vulkan.h"
#include "cubism_renderer_select.hpp"

#include <Model/CubismUserModel.hpp>
#include <CubismModelSettingJson.hpp>
#include <Motion/ACubismMotion.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#ifdef BONGO_CAT_HAS_VULKAN
#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>
#endif
#include <SDL3/SDL_opengl.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bongo_cat {

bool validate_model_setting_json(const std::vector<unsigned char> &json,
    const char *setting_file, BongoCatError *error);
class ViewerLookUpdater;
class ParameterOverrideUpdater;

/*
 * 一个 Live2D 模型实例, 也是它全部 Cubism 资源的所有者: 模型数据、动作/表情、贴图、
 * 渲染器、遮罩缓冲都在这里。GL 与 Vulkan 两套贴图句柄 (textures_ / vulkan_textures_)
 * 互斥使用, 由 cubism_renderer_select.hpp 的运行时开关决定, 同一时刻只有一套非空。
 *
 * 生命周期约定: 先 load() 读 model3.json 与动作, 再 load_textures() 上传贴图并创建
 * 渲染器 (内部会先 release_renderer 再 create_renderer), 之后每帧 update()/draw()。
 * 析构自己释放全部资源, 但贴图的真正释放时机受 cubism_bridge_load.cpp 的退休队列约束。
 */
class NativeModel final : public Csm::CubismUserModel {
public:
    NativeModel();
    ~NativeModel() override;
    bool load(const char *directory, const char *setting_file, bool direct_textures,
        BongoCatLive2DLoadProgress progress, void *userdata,
        BongoCatError *error);
    bool load_textures(BongoCatError *error,
        BongoCatLive2DLoadProgress progress, void *userdata);
    size_t texture_count() const { return textures_.size(); }
    void release_render_resources();
    bool canvas_size(int *width, int *height) const;
    bool frame(BongoCatLive2DFrame *frame) const;
    bool viewport(int *x, int *y, int *width, int *height) const;
    void resize(int width, int height);
    void reshape(int width, int height);
    bool update(float delta_seconds);
    void draw();
    void set_mirror(bool mirror);
    void set_vertical_flip(bool flipped);
    void set_render_options(const BongoCatLive2DRenderOptions &options);
    void set_dragging(float x, float y, bool angle_z = false);
    void prepare_viewer_audit();
    bool prepare_cover_capture();
    bool set_parameter(const char *id, float value);
    bool parameter(const char *id, float *minimum, float *maximum, float *value);
    bool start_motion(const char *group, int index);
    bool restore_motion_state(const char *group, int index);
    bool preview_motion(const char *group, int index);
    bool restore_motion_preview();
    bool commit_motion_preview(const char *group, int index);
    bool motion_selected(const char *group, int index) const;
    bool motion_persistent(const char *group, int index) const;
    bool motion_visible(const char *group, int index) const;
    bool motion_same_toggle(const char *left_group, int left_index,
        const char *right_group, int right_index) const;
    bool set_expression(int index);
    int expression() const { return expression_index_; }
    bool visual_state(BongoCatLive2DVisualState *state) const;

public:
    using MotionMap = std::map<std::string, Csm::ACubismMotion *>;
    using MotionSignatures = std::map<std::string, std::string>;
    struct MotionStateCurve {
        std::string target, id;
        float start = 0.0f, end = 0.0f;
        float normal = 0.0f;
        int parameter = -1;
        int part = -1;
        bool model_opacity = false;
    };
    struct MotionState {
        std::string group;
        int index = -1;
        std::vector<MotionStateCurve> curves;
        bool self_contained = false;
    };
private:
    friend class ParameterOverrideUpdater;
    struct MotionRun {
        Csm::CubismMotionQueueEntryHandle handle =
            Csm::InvalidMotionQueueEntryHandleValue;
        std::string key;
        bool one_shot = false;
        bool selected = false;
        bool committed = false;
    };
    struct ModelBounds {
        float min_x = 0.0f;
        float min_y = 0.0f;
        float max_x = 0.0f;
        float max_y = 0.0f;
        bool valid = false;
    };
    struct DrawableBounds { ModelBounds bounds; float area; };
    bool load_model(BongoCatError *error);
    void load_expressions();
    void load_effects();
    void load_motions(BongoCatLive2DLoadProgress progress, void *userdata);
public:
    /*
     * 分步加载原语。存在的理由只有一个: 主线程必须能在加载中途出帧, 所以"一次加载"被切
     * 成"很多次只做一小步"。语义上等价于 load()/load_textures(), 但每一步都必须在两次
     * 调用之间保持可中断 —— 所以这里**不许**有跨步的临时状态留在栈上, 全部记在成员里。
     *
     * 与 load()/load_textures() 的分工:
     *   - settings_begin/settings_finish 对应 load() 里"读文件 + 解析 + 建 setting_"那一段;
     *   - motions_begin/step/end 对应 load_motions() 里逐个动作的那段循环;
     *   - textures_begin/step/end 对应 load_textures() 里逐个贴图 + 收尾建渲染器那一段。
     * 一次性路径 (load/load_textures) 保留原样, 供启动与不分步的调用方使用。
     */
    bool settings_begin(const char *directory, const char *setting_file,
        BongoCatError *error);
    bool settings_finish(BongoCatError *error);
    void motions_begin();
    bool motions_step();
    void motions_end();
    void textures_begin();
    /* 一张贴图分两步走: decode 读文件+解码 (Vulkan 上传路径的最贵那一步), mask 生成 alpha
     * 掩码 (纯 CPU, 供 capture_visible_bounds 剔除全透明三角形)。分开是为了让单步更短 ——
     * 切换期间的帧间隔上界就是"预算 + 单步最长耗时"。GL 路径在 decode 步骤里由
     * bongo_cat_image_texture_model 顺手把掩码填好, mask 步骤于是成为空操作。 */
    bool textures_step(int index, BongoCatError *error);
    void textures_mask_step(int index);
    int texture_total() const { return texture_count_; }
    bool textures_end(BongoCatError *error);
private:
    void start_idle_motion();
    ModelBounds capture_visible_bounds() const;
    void prepare_expression_frame();
    void build_projection(Csm::CubismMatrix44 &projection,
        int width, int height);
    void apply_viewport_projection(Csm::CubismMatrix44 &projection) const;
    void reflect_vertical_flip(Csm::CubismMatrix44 &projection) const;
    void update_viewport();
    void record_visible_state(Csm::CubismMatrix44 &projection);
    void capture_motion_preview();
    void restore_motion_preview_state();
    void load_motion_state(const std::string &key, const char *group, int index,
        const std::vector<unsigned char> &bytes);
    void pair_motion_states();
    std::string motion_to_play(const std::string &key, bool *selected) const;
    bool motion_is_persistent(const std::string &key) const;
    void record_motion_run(Csm::CubismMotionQueueEntryHandle handle,
        const std::string &key, bool selected, bool committed);
    void stop_motion_runs(const std::string &key);
    void expire_motion_runs();
    void clear_motion_runs();
    void expire_expression_fade();
    void settle_pending_expression_for_cover();
    void capture_parameter_baseline();
    void save_parameters();
    void add_parameter_override_updater();
    void apply_parameter_overrides();
    bool apply_motion_curve(const MotionStateCurve &curve, float value);
    bool restore_motion_defaults(const std::string &key);
    void select_motion(const std::string &key, bool selected);
    void release_textures();
    void release_renderer();
    bool create_renderer(BongoCatError *error);
    void update_mask_buffers();
    void bind_textures();
    std::vector<unsigned char> read(const std::string &path,
        size_t maximum = (size_t)-1) const;
    std::string path(const char *relative) const;

    Csm::CubismModelSettingJson *setting_ = nullptr;
    MotionMap motions_;
    MotionSignatures motion_signatures_;
    std::map<std::string, MotionState> motion_states_;
    std::map<std::string, std::string> motion_toggle_partners_;
    std::map<std::string, std::string> motion_toggle_owners_;
    std::set<std::string> selected_motion_keys_;
    MotionMap expressions_;
    std::vector<std::string> expression_names_;
    std::vector<GLuint> textures_;
    /* Vulkan 后端下的贴图句柄 (CubismImageVulkan*), 与 textures_ 一一对应 */
    std::vector<void *> vulkan_textures_;
    /* 每张贴图的 alpha 掩码, 供 capture_visible_bounds 剔除全透明三角形 */
    std::vector<BongoCatImageAlphaMask> texture_alpha_;
    mutable std::vector<std::vector<unsigned char>> triangle_alpha_;
    mutable std::vector<DrawableBounds> bounds_scratch_;
    std::vector<float> parameter_snapshot_;
    std::vector<float> part_snapshot_;
    std::vector<float> parameter_override_values_;
    /* 动作基线: Cubism 的 SaveParameters() 存的是"没有外部输入时的参数值", 动作恢复
     * 终态要写这里而不是覆盖值 —— 两者语义不同, 混用会让模型停在错误的姿势上。 */
    std::vector<float> parameter_baseline_values_;
    std::vector<float> parameter_save_scratch_;
    std::vector<unsigned char> parameter_overrides_;
    /* 预览事务的快照, 仅在 motion_preview_active_ 为真时有意义 (见 cubism_motion_preview.cpp) */
    std::vector<float> motion_preview_parameters_;
    std::vector<float> motion_preview_parts_;
    Csm::csmVector<Csm::CubismIdHandle> eye_blink_ids_;
    Csm::csmVector<Csm::CubismIdHandle> lip_sync_ids_;
    std::string directory_;
    /* 分步加载的游标: 读进来的 model3.json 原文 (settings_begin 填, settings_finish 用),
     * 以及动作/贴图两步各自走到哪了。它们跨步存活, 所以不能用局部变量代替。 */
    std::vector<unsigned char> setting_json_;
    int motion_group_ = 0;
    int motion_index_ = 0;
    int motion_total_ = 0;
    int motions_done_ = 0;
    int texture_count_ = 0;
    /* 刚解码但还没生成 alpha 掩码的那幅位图 (Vulkan 路径)。它必须跨两次 step 存活 (先上传、
     * 再算掩码), 所以不能是局部变量。 */
    BongoCatImage pending_alpha_image_{};
#ifdef CSM_TARGET_MAC_GL
    CoreProfileBuffers core_buffers_;
#endif
    int width_ = 612;
    int height_ = 354;
    int renderer_width_ = 0;
    int renderer_height_ = 0;
    /* 遮罩图集的运行时记账 (GL 路径): 纹理上限 / 当前尺寸 / 网格划分 / 上次生效的窗口尺寸 */
    int mask_texture_limit_ = 0;
    int mask_buffer_size_ = 0;
    int mask_layout_divisions_ = 1;
    int mask_last_width_ = 0;
    int mask_last_height_ = 0;
    int viewport_x_ = 0;
    int viewport_y_ = 0;
    int viewport_width_ = 612;
    int viewport_height_ = 354;
    int expression_index_ = -1;
    bool expression_clearing_ = false;
    bool expression_frame_pending_ = false;
    BongoCatLive2DFrame frame_{};
    BongoCatLive2DVisualState visual_state_{};
    bool visual_state_ready_ = false;
    bool motion_updated_ = false;
    bool suppress_eye_blink_ = false;
    bool automatic_idle_ = true;
    bool mirror_ = false;
    bool vertical_flip_ = false;
    BongoCatLive2DRenderOptions render_options_{};
    bool direct_textures_ = false;
    bool parameter_overrides_applied_ = false;
    std::vector<std::string> idle_motion_keys_;
    std::vector<MotionRun> motion_runs_;
    std::vector<unsigned char> motion_finished_scratch_;
    ViewerLookUpdater *viewer_look_ = nullptr;
    int last_idle_motion_ = -1;
    float opacity_snapshot_ = -1.0f;
    float motion_preview_opacity_ = 1.0f;
    std::string motion_preview_key_;
    bool motion_preview_selected_ = false;
    bool motion_preview_active_ = false;
    bool motion_preview_completed_ = false;
};

bool motion_toggle_pair(const NativeModel::MotionState &left,
    const NativeModel::MotionState &right, bool *left_enabled);
bool motion_enables_state(const NativeModel::MotionState &state);
bool motion_run_clears_selection(bool one_shot, bool committed,
    bool replacement_running);

} // namespace bongo_cat

#endif
