#ifndef BONGO_CAT_UI_ICONS_H
#define BONGO_CAT_UI_ICONS_H

/*
 * 图标索引。这个顺序是外部契约: 图标图集按同样的顺序排列, ui_catime_tabs.c 里
 * "icons ? icons[i] : i" 直接用标签下标当图标用, 所以中间插入一个值会让所有
 * 后续图标的对应关系整体错位 (ui_catime_icons.c 的手绘兜底图标也按这套下标分支)。
 * 需要新增时追加在 CLOSE 之前, 并同步更新图集资源。
 */
typedef enum BongoCatUIIcon {
    BONGO_CAT_UI_ICON_CAT,
    BONGO_CAT_UI_ICON_SETTINGS,
    BONGO_CAT_UI_ICON_WAND,
    BONGO_CAT_UI_ICON_KEYBOARD,
    BONGO_CAT_UI_ICON_HEART,
    BONGO_CAT_UI_ICON_GITHUB,
    BONGO_CAT_UI_ICON_DISCORD,
    BONGO_CAT_UI_ICON_QQ,
    BONGO_CAT_UI_ICON_COFFEE,
    BONGO_CAT_UI_ICON_UPLOAD,
    BONGO_CAT_UI_ICON_CHECK,
    BONGO_CAT_UI_ICON_VERIFIED,
    BONGO_CAT_UI_ICON_SMILE,
    BONGO_CAT_UI_ICON_FOLDER,
    BONGO_CAT_UI_ICON_TRASH,
    BONGO_CAT_UI_ICON_CIRCLE_X,
    BONGO_CAT_UI_ICON_PLAY,
    BONGO_CAT_UI_ICON_SYNC,
    BONGO_CAT_UI_ICON_CLOSE,
    BONGO_CAT_UI_ICON_COUNT
} BongoCatUIIcon;

#endif
