/* 滚轮手势常量: 事件间隔超过 GESTURE_IDLE 视为新手势 (要重采基准), 动画两帧之间至少隔 FRAME_INTERVAL 才推进; round_position 是负数也远离零取整的公共实现 (自测与几何换算共用)。 */
#ifndef BONGO_CAT_WINDOW_WHEEL_INTERNAL_H
#define BONGO_CAT_WINDOW_WHEEL_INTERNAL_H

#define BONGO_CAT_WHEEL_GESTURE_IDLE_NS 120000000ull
#define BONGO_CAT_WHEEL_FRAME_INTERVAL_NS 8000000ull

int bongo_cat_window_wheel_round_position(float value);

#endif
