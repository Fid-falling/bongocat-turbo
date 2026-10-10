#include "bongo_cat/platform.h"

#ifdef _WIN32
/*
 * 只读地取当前窗口不透明度。写入在 windows_layered_state.c 的 set_opacity 里 ——
 * 那里负责真正应用到窗口并在失败时回滚, 这里只把平台状态里记录的当前值交出去
 * (设置界面与诊断日志读它)。platform 为 NULL 时按完全不透明处理, 方便日志路径
 * 直接调用。
 */
float bongo_cat_platform_get_opacity(const BongoCatPlatform *platform) {
    return platform ? platform->window_opacity : 1.0f;
}
#endif
