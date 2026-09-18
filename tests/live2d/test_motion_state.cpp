#include "cubism_model.hpp"
#include "test.h"

#include <initializer_list>

int bongo_cat_test_failures;

using MotionState = bongo_cat::NativeModel::MotionState;
using MotionStateCurve = bongo_cat::NativeModel::MotionStateCurve;

static MotionStateCurve parameter(const char *id, float start, float end,
    float normal, int index) {
    MotionStateCurve curve;
    curve.target = "Parameter";
    curve.id = id;
    curve.start = start;
    curve.end = end;
    curve.normal = normal;
    curve.parameter = index;
    return curve;
}

static MotionStateCurve part(const char *id, float start, float end,
    float normal, int index) {
    MotionStateCurve curve;
    curve.target = "PartOpacity";
    curve.id = id;
    curve.start = start;
    curve.end = end;
    curve.normal = normal;
    curve.part = index;
    return curve;
}

static MotionState motion(const char *group, int index,
    std::initializer_list<MotionStateCurve> curves) {
    MotionState state;
    state.group = group;
    state.index = index;
    state.curves = curves;
    return state;
}

int main() {
    MotionState show = motion("toggle", 6,
        {parameter("Visible", 0.0f, 1.0f, 0.0f, 4)});
    MotionState hide = motion("toggle", 9,
        {parameter("Visible", 1.0f, 0.0f, 0.0f, 4)});
    bool enabled = false;
    CHECK(bongo_cat::motion_toggle_pair(show, hide, &enabled));
    CHECK(enabled);

    MotionState part_show = motion("parts", 1,
        {part("Accessory", 0.0f, 1.0f, 0.0f, 3)});
    MotionState part_hide = motion("parts", 8,
        {part("Accessory", 1.0f, 0.0f, 0.0f, 3)});
    CHECK(bongo_cat::motion_toggle_pair(part_show, part_hide, &enabled));
    CHECK(enabled);
    CHECK(bongo_cat::motion_enables_state(part_show));
    CHECK(!bongo_cat::motion_enables_state(part_hide));
    CHECK(bongo_cat::motion_enables_state(show));
    CHECK(!bongo_cat::motion_enables_state(hide));
    MotionState one_shot = motion("action", 16,
        {parameter("Rotation", 360.0f, 45.0f, 0.0f, 4)});
    one_shot.self_contained = true;
    CHECK(!bongo_cat::motion_enables_state(one_shot));

    CHECK(bongo_cat::motion_run_clears_selection(true, true, false));
    CHECK(!bongo_cat::motion_run_clears_selection(false, true, false));
    CHECK(!bongo_cat::motion_run_clears_selection(true, false, false));
    CHECK(!bongo_cat::motion_run_clears_selection(true, true, true));

    enabled = true;
    CHECK(bongo_cat::motion_toggle_pair(hide, show, &enabled));
    CHECK(!enabled);

    MotionState multi_show = motion("other", 3, {
        parameter("Left", 0.0f, 1.0f, 0.0f, 1),
        parameter("Right", 0.0f, 1.0f, 0.0f, 2)});
    MotionState multi_hide = motion("other", 20, {
        parameter("Left", 1.0f, 0.0f, 0.0f, 1),
        parameter("Right", 1.0f, 0.0f, 0.0f, 2)});
    CHECK(bongo_cat::motion_toggle_pair(multi_show, multi_hide, &enabled));
    CHECK(enabled);

    MotionState wrong_group = hide;
    wrong_group.group = "different";
    CHECK(!bongo_cat::motion_toggle_pair(show, wrong_group, &enabled));

    MotionState contradictory = motion("other", 21, {
        parameter("Left", 1.0f, 0.0f, 0.0f, 1),
        parameter("Right", 1.0f, 0.0f, 1.0f, 2)});
    CHECK(!bongo_cat::motion_toggle_pair(multi_show, contradictory, &enabled));

    MotionState missing_parameter = hide;
    missing_parameter.curves[0].parameter = -1;
    CHECK(!bongo_cat::motion_toggle_pair(show, missing_parameter, &enabled));

    if (bongo_cat_test_failures) return 1;
    puts("motion state checks passed");
    return 0;
}
