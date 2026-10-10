/*
 * Mver 动作 JSON 的曲线解析, 以及"开/关"配对动作的识别。
 *
 * Cubism 自己的动作格式由 SDK 解析, 但 Mver 转换过来的动作还额外需要两件事:
 *  1) 把每条曲线的端点抄下来 —— 动作播完后要把参数恢复成原值, 而 SDK 不会替我们做;
 *  2) 认出互为反向的一对动作 (开灯/关灯), 让 UI 把它们显示成一个开关而不是两个按钮。
 * 两个判据都来自同一份 JSON, 所以放在一起解析一次。
 */
#include "cubism_model.hpp"

#include <CubismFramework.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismModel.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <yyjson.h>

namespace bongo_cat {

/* Segments 的布局是 [类型, 起点, ...控制点/终点]: 起点在 1 号位, 终点永远在最后一位,
 * 所以不必先解析中间是什么类型就能取到两端。 */
static bool curve_endpoints(yyjson_val *segments, float *start, float *end) {
    if (!yyjson_is_arr(segments) || yyjson_arr_size(segments) < 2) return false;
    yyjson_val *first = yyjson_arr_get(segments, 1);
    yyjson_val *last = yyjson_arr_get(segments, yyjson_arr_size(segments) - 1);
    if (!yyjson_is_num(first) || !yyjson_is_num(last)) return false;
    *start = (float)yyjson_get_num(first);
    *end = (float)yyjson_get_num(last);
    return true;
}

/* "自包含"判定: 一条曲线如果两端都等于默认值、但中间确实偏离过, 说明这个动作播完自己
 * 就回到了原状, 不需要我们再恢复; 否则必须靠 motion_states_ 里记下的端点值手动还原。
 * 中间段的点数按类型推 (贝塞尔 3 个点, 其余 1 个点), 布局对不上就整条曲线放弃, 不要猜。 */
static bool curve_returns_to_default(yyjson_val *segments, float normal) {
    float start = 0.0f, end = 0.0f;
    if (!curve_endpoints(segments, &start, &end) ||
        std::fabs(start - normal) > .0001f ||
        std::fabs(end - normal) > .0001f) return false;
    bool deviates = false;
    size_t cursor = 2, count = yyjson_arr_size(segments);
    while (cursor < count) {
        yyjson_val *kind_value = yyjson_arr_get(segments, cursor);
        if (!yyjson_is_int(kind_value) && !yyjson_is_uint(kind_value))
            return false;
        int kind = (int)yyjson_get_int(kind_value);
        size_t points = kind == 1 ? 3 :
            (kind == 0 || kind == 2 || kind == 3 ? 1 : 0);
        if (!points || cursor + points * 2 >= count) return false;
        for (size_t point = 0; point < points; ++point) {
            yyjson_val *value = yyjson_arr_get(segments,
                cursor + 2 + point * 2);
            if (!yyjson_is_num(value)) return false;
            if (std::fabs((float)yyjson_get_num(value) - normal) > .0001f)
                deviates = true;
        }
        cursor += 1 + points * 2;
    }
    return deviates;
}

static bool curve_has_state_target(const NativeModel::MotionStateCurve &curve) {
    return curve.parameter >= 0 || curve.part >= 0 || curve.model_opacity;
}

/* 逐条曲线解析。Target 决定恢复到哪一类对象 (参数 / 部件不透明度 / 整模型不透明度),
 * normal 是该对象当前的"常态"值, 端点值留到动作结束时写回。同时把"这条动作碰过哪些
 * target:id"累进 motion_signatures_ —— 配对时先用签名做粗筛, 签名不同就不可能是一对。 */
void NativeModel::load_motion_state(const std::string &key, const char *group,
    int motion_index, const std::vector<unsigned char> &bytes) {
    yyjson_doc *document = yyjson_read(
        reinterpret_cast<const char *>(bytes.data()), bytes.size(), 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val *curves = yyjson_is_obj(root) ? yyjson_obj_get(root, "Curves") : nullptr;
    MotionState state; state.group = group ? group : "";
    state.index = motion_index;
    std::vector<std::string> targets;
    size_t index, count; yyjson_val *curve;
    if (yyjson_is_arr(curves)) yyjson_arr_foreach(curves, index, count, curve) {
        const char *target = yyjson_get_str(yyjson_obj_get(curve, "Target"));
        const char *id = yyjson_get_str(yyjson_obj_get(curve, "Id"));
        if (!target || !id) continue;
        targets.push_back(std::string(target) + ":" + id);
        MotionStateCurve value{target, id};
        yyjson_val *segments = yyjson_obj_get(curve, "Segments");
        if (std::strcmp(target, "Parameter") == 0) {
            auto handle = Csm::CubismFramework::GetIdManager()->GetId(id);
            value.parameter = _model->GetParameterIndex(handle);
            if (value.parameter >= 0 &&
                value.parameter < _model->GetParameterCount())
                value.normal = _model->GetParameterDefaultValue(value.parameter);
            else value.parameter = -1;
        } else if (std::strcmp(target, "PartOpacity") == 0) {
            auto handle = Csm::CubismFramework::GetIdManager()->GetId(id);
            value.part = _model->GetPartIndex(handle);
            if (value.part >= 0 && value.part < _model->GetPartCount())
                value.normal = _model->GetPartOpacity(value.part);
            else value.part = -1;
        } else if (std::strcmp(target, "Model") == 0 &&
            std::strcmp(id, "Opacity") == 0) {
            value.model_opacity = true;
            value.normal = _model->GetModelOpacity();
        }
        if (curve_has_state_target(value) &&
            curve_returns_to_default(segments, value.normal))
            state.self_contained = true;
        if (curve_endpoints(segments, &value.start, &value.end))
            state.curves.push_back(value);
    }
    if (document) yyjson_doc_free(document);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const std::string &target : targets)
        motion_signatures_[key] += target + '\n';
    std::sort(state.curves.begin(), state.curves.end(),
        [](const MotionStateCurve &a, const MotionStateCurve &b) {
            return a.target == b.target ? a.id < b.id : a.target < b.target;
        });
    motion_states_[key] = std::move(state);
}

/* 判定两个动作是不是一对开关: 必须在同一组、曲线条数与 target 一一对应, 且每条曲线
 * 要么两端完全相同 (这条曲线与开关无关), 要么端点对调 (互为反向)。此外必须恰好有一侧
 * 停在默认值 —— 停在默认值的那侧就是"关"。所有反向曲线给出的方向还得一致, 否则它们
 * 只是碰巧相似的两个独立动作, 不能当成开关。 */
bool motion_toggle_pair(const NativeModel::MotionState &a,
    const NativeModel::MotionState &b, bool *first_enabled) {
    if (!first_enabled || a.group != b.group ||
        a.curves.size() != b.curves.size() || a.curves.empty()) return false;
    bool direction_known = false;
    for (size_t i = 0; i < a.curves.size(); ++i) {
        const auto &left = a.curves[i];
        const auto &right = b.curves[i];
        if (left.target != right.target || left.id != right.id) return false;
        bool same = std::fabs(left.start - right.start) <= .0001f &&
            std::fabs(left.end - right.end) <= .0001f;
        bool reversed = std::fabs(left.start - right.end) <= .0001f &&
            std::fabs(left.end - right.start) <= .0001f;
        if (!same && !reversed) return false;
        if (!reversed || same) continue;
        if (!curve_has_state_target(left) || !curve_has_state_target(right))
            return false;
        bool left_normal = std::fabs(left.end - left.normal) <= .0001f;
        bool right_normal = std::fabs(right.end - right.normal) <= .0001f;
        if (left_normal == right_normal) return false;
        bool enabled = !left_normal;
        if (direction_known && enabled != *first_enabled) return false;
        *first_enabled = enabled;
        direction_known = true;
    }
    return direction_known;
}

bool motion_enables_state(const NativeModel::MotionState &state) {
    if (state.self_contained) return false;
    for (const auto &curve : state.curves)
        if (curve_has_state_target(curve) &&
            std::fabs(curve.end - curve.normal) > .0001f) return true;
    return false;
}

static bool same_signature(const NativeModel::MotionSignatures &signatures,
    const std::string &left, const std::string &right) {
    auto a = signatures.find(left), b = signatures.find(right);
    return a != signatures.end() && b != signatures.end() &&
        !a->second.empty() && a->second == b->second;
}

static size_t pair_candidates(
    const std::map<std::string, NativeModel::MotionState> &states,
    const NativeModel::MotionSignatures &signatures, const std::string &key,
    std::string *candidate, bool *key_enabled) {
    auto source = states.find(key);
    if (source == states.end()) return 0;
    size_t count = 0;
    for (const auto &item : states) {
        if (item.first == key || !same_signature(signatures, key, item.first))
            continue;
        bool enabled = false;
        if (!motion_toggle_pair(source->second, item.second, &enabled)) continue;
        if (++count == 1) {
            if (candidate) *candidate = item.first;
            if (key_enabled) *key_enabled = enabled;
        }
    }
    return count;
}

/* 只在配对唯一时才承认: 正反两个方向都必须恰好只有一个候选。有多个候选说明这组动作里
 * 存在重复曲线, 硬配会把 UI 的开关连错, 所以宁可放弃配对 —— 它们就显示成两个独立动作。 */
void NativeModel::pair_motion_states() {
    for (const auto &item : motion_states_) {
        if (motion_toggle_owners_.find(item.first) != motion_toggle_owners_.end())
            continue;
        std::string candidate;
        bool item_enabled = false;
        if (pair_candidates(motion_states_, motion_signatures_, item.first,
            &candidate, &item_enabled) != 1 ||
            pair_candidates(motion_states_, motion_signatures_, candidate,
                nullptr, nullptr) != 1) continue;
        const std::string &owner = item_enabled ? item.first : candidate;
        const std::string &partner = item_enabled ? candidate : item.first;
        motion_toggle_partners_[owner] = partner;
        motion_toggle_owners_[owner] = owner;
        motion_toggle_owners_[partner] = owner;
    }
}

/* 返回"这次点击真正该播的动作", 并把 UI 勾选态该变成什么写进 *selected。
 * owner/partner 的规范化保证同一个开关无论从哪一侧点进来都落到同一个 key 上。
 * 一次性动作只在本次播放期间保持勾选; 开关型动作则改播它的反向动作。 */
std::string NativeModel::motion_to_play(const std::string &key,
    bool *selected) const {
    auto owner = motion_toggle_owners_.find(key);
    const std::string &canonical = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    auto partner = motion_toggle_partners_.find(canonical);
    bool active = selected_motion_keys_.find(canonical) != selected_motion_keys_.end();
    bool persistent = motion_is_persistent(canonical);
    /* One-shot actions stay checked only while their own playback is alive. */
    if (selected) *selected = persistent ? !active : true;
    if (active && persistent && partner == motion_toggle_partners_.end()) return {};
    return active && partner != motion_toggle_partners_.end() ?
        partner->second : canonical;
}

bool NativeModel::motion_is_persistent(const std::string &key) const {
    auto owner = motion_toggle_owners_.find(key);
    const std::string &canonical = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    if (motion_toggle_partners_.find(canonical) != motion_toggle_partners_.end())
        return true;
    auto state = motion_states_.find(canonical);
    return state != motion_states_.end() && motion_enables_state(state->second);
}

bool NativeModel::motion_persistent(const char *group, int index) const {
    if (!group || index < 0) return false;
    return motion_is_persistent(
        std::string(group) + "_" + std::to_string(index));
}

bool NativeModel::restore_motion_defaults(const std::string &key) {
    auto state = motion_states_.find(key);
    if (_model == nullptr || state == motion_states_.end()) return false;
    bool had_run = false;
    for (const MotionRun &run : motion_runs_)
        if (run.key == key) { had_run = true; break; }
    bool restored = false;
    stop_motion_runs(key);
    for (const auto &curve : state->second.curves) {
        restored = apply_motion_curve(curve, curve.normal) || restored;
    }
    if (!restored) return had_run;
    save_parameters();
    return true;
}

/* 写回一个恢复值。参数要同时更新 baseline —— Cubism 的 SaveParameters() 保存的是"动作
 * 基线", 恢复值必须成为新的基线, 否则下一个动作会从恢复之前的旧值开始插值。
 * 外部输入覆盖正在生效时, 画面显示覆盖值、但基线照常更新 (两者刻意分离)。 */
bool NativeModel::apply_motion_curve(const MotionStateCurve &curve,
    float value) {
    if (!_model) return false;
    if (curve.parameter >= 0 &&
        curve.parameter < _model->GetParameterCount()) {
        const size_t index = (size_t)curve.parameter;
        parameter_baseline_values_[index] = value;
        float displayed = parameter_overrides_applied_ &&
            parameter_overrides_[index] ? parameter_override_values_[index] : value;
        _model->SetParameterValue(curve.parameter, displayed);
        return true;
    }
    if (curve.part >= 0 && curve.part < _model->GetPartCount()) {
        _model->SetPartOpacity(curve.part, value);
        return true;
    }
    if (curve.model_opacity) {
        _model->SetModelOpacity(value);
        _opacity = value;
        return true;
    }
    return false;
}

bool NativeModel::restore_motion_state(const char *group, int index) {
    if (!_model || !group || index < 0) return false;
    std::string key = std::string(group) + "_" + std::to_string(index);
    auto owner = motion_toggle_owners_.find(key);
    const std::string &canonical = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    auto state = motion_states_.find(canonical);
    if (state == motion_states_.end() || !motion_is_persistent(canonical))
        return false;
    bool restored = false;
    stop_motion_runs(canonical);
    for (const auto &curve : state->second.curves)
        restored = apply_motion_curve(curve, curve.end) || restored;
    if (!restored) return false;
    select_motion(canonical, true);
    save_parameters();
    return true;
}

} // namespace bongo_cat
