#ifndef BONGO_CAT_PREFERENCES_UPDATE_STATUS_H
#define BONGO_CAT_PREFERENCES_UPDATE_STATUS_H

#include "preferences_state.h"

/*
 * 一键更新的状态条 (窗口底部居中的小胶囊): 下载进度、当前用的下载源、校验/准备重启阶段,
 * 以及失败原因。没有正在进行的更新时不画任何东西。
 * 为什么要有它: 按钮上的文案只在"关于"页看得见, 而用户点了检查更新之后很可能切到别的
 * 设置页或者干脆让窗口留在别的页上; 进度与失败原因必须一直可见。
 */
void bongo_cat_preferences_update_status_draw(BongoCatPreferences *value,
    struct nk_context *context, float width, float height);

#endif
