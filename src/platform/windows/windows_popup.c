#include "windows_popup.h"

#ifdef _WIN32
/* 菜单结束后必须往 owner 投一条 WM_NULL(即使 owner 已不可用也要忽略错误): 这是
   TrackPopupMenu 文档要求的收尾动作, 负责把菜单的内部状态和焦点清干净。 */
void bongo_cat_windows_popup_complete(HWND owner) {
    if (owner) PostMessageW(owner, WM_NULL, 0, 0);
}

/* 前置的 SetForegroundWindow 是为了让菜单能正确收到"点到别处就关闭"的消息。
   返回 TrackPopupMenu 的命令 id(0 表示用户取消, 或菜单项没有 id)。 */
UINT bongo_cat_windows_popup_track(HWND owner, HMENU menu, UINT flags,
    int x, int y) {
    if (!owner || !menu) return 0;
    SetForegroundWindow(owner);
    UINT command = TrackPopupMenu(menu, flags, x, y, 0, owner, NULL);
    bongo_cat_windows_popup_complete(owner);
    return command;
}
#endif
