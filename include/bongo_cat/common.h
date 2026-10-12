#ifndef BONGO_CAT_COMMON_H
#define BONGO_CAT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bongo_cat/version.h"

/* 显示名: 只用于窗口标题 / 托盘提示 / 关于页这些"给人看"的地方, 不参与任何路径或身份标识。 */
#define BONGO_CAT_NAME "BongoCat Turbo"
#define BONGO_CAT_PET_WINDOW_TITLE BONGO_CAT_NAME
#define BONGO_CAT_TRAY_TITLE BONGO_CAT_NAME " " BONGO_CAT_VERSION_DISPLAY " \xC2\xB7 by vladelaina & Fid_falling"
#define BONGO_CAT_SLUG "bongocat"
/* 唯一按文件名找"自己"的地方: 副宠子进程的可执行文件回退名 (首选 app->executable_path)。
   必须与 CMake 里 set_target_properties(bongo_cat OUTPUT_NAME ...) 完全一致, 否则拉不起副宠。 */
#define BONGO_CAT_EXECUTABLE "BongoCatTurbo"
#define BONGO_CAT_APP_ID "com.bongocat.desktop"
#define BONGO_CAT_NAME_W L"BongoCat Turbo"
#define BONGO_CAT_PET_WINDOW_TITLE_W BONGO_CAT_NAME_W
/*
 * 以下三个是"零迁移"的磁盘/系统身份, 刻意与显示名解耦:
 * 1.15.x 用户升级到改名后的版本时, 设置 / 模型 / 封面 / 开机自启项必须落在同一个位置,
 * 一旦跟着显示名一起改, 老用户会直接丢配置和已导入的模型。改名时不要动它们。
 * (BONGO_CAT_APP_ID 与 BONGO_CAT_SLUG 同理, 分别是 macOS 偏好/通知标识与 XDG/Linux 目录名。)
 */
#define BONGO_CAT_STORAGE_DIR "BongoCat"
#define BONGO_CAT_STORAGE_DIR_W L"BongoCat"
/* 启动文件夹里的快捷方式文件名保持不变: 改名后用户原来的自启项仍然有效, 关闭自启时
   删的也还是同一个文件, 不会留下指向已不存在 exe 的孤儿快捷方式。 */
#define BONGO_CAT_STARTUP_SHORTCUT_W L"BongoCat"
#define BONGO_CAT_PATH_CAP 1024
#define BONGO_CAT_ID_CAP 128
#define BONGO_CAT_SHORTCUT_CAP 128
#define BONGO_CAT_MODEL_CAP 128
#define BONGO_CAT_ACTIVE_MODEL_CAP 8
#define BONGO_CAT_ADDITIONAL_MODEL_CAP (BONGO_CAT_ACTIVE_MODEL_CAP - 1)
#define BONGO_CAT_BEHAVIOR_CAP 128
#define BONGO_CAT_BEHAVIOR_ID_CAP 384
#define BONGO_CAT_BEHAVIOR_BINDING_CAP 256
#define BONGO_CAT_MENU_LABEL_CAP (BONGO_CAT_ID_CAP + BONGO_CAT_SHORTCUT_CAP + 4)
#define BONGO_CAT_SCHEDULED_RELEASE_CAP 64
#define BONGO_CAT_UPDATE_VERSION_CAP 32
#define BONGO_CAT_MODEL_ADAPTER_FILE ".bongo-cat-adapter.json"
#define BONGO_CAT_MODEL_BUILTIN_MARKER ".bongo-cat-builtin"
#define BONGO_CAT_MODEL_ADAPTER_SCHEMA 1
#define BONGO_CAT_MODEL_ADAPTER_GENERATOR 1

typedef enum BongoCatResult {
    BONGO_CAT_OK = 0,
    BONGO_CAT_ERROR_ARGUMENT,
    BONGO_CAT_ERROR_IO,
    BONGO_CAT_ERROR_FORMAT,
    BONGO_CAT_ERROR_MEMORY,
    BONGO_CAT_ERROR_PLATFORM,
    BONGO_CAT_ERROR_CUBISM
} BongoCatResult;

typedef struct BongoCatError {
    BongoCatResult code;
    char message[256];
} BongoCatError;

#ifdef __cplusplus
extern "C" {
#endif

void bongo_cat_error_set(BongoCatError *error, BongoCatResult code, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
