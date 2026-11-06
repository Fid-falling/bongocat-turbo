/*
 * 面捕组件的文件落盘: "把正在被本进程装载的文件换掉"以及清理上次的 .old。
 *
 * 单独成文件是为了守住 facetrack_install.c 的行数上限 (CheckLines.cmake 500 行),
 * 也让"Windows 上换被占用文件"这个坑只有一个出处。
 */
#include "facetrack_install.h"

#include "bongo_cat/path.h"

#include <windows.h>

#include <stdio.h>

/* 把 <path> 换成本进程刚下好的那份。
   坑: 组件 DLL/onnxruntime.dll 往往正被本进程装载 —— 对已装载的映像, Windows 允许
   **改名**但不允许删除/覆盖。所以先把它改成一个 .old 名字让路, 再把 .new 顶上去;
   下次启动时 .old 会被清掉 (cleanup_stale_backups)。 */
bool bongo_cat_facetrack_replace_file(const char *path, const char *part_path,
    char *error, size_t capacity) {
    if (remove(path) == 0 || GetLastError() == ERROR_FILE_NOT_FOUND) {
        if (rename(part_path, path) == 0) return true;
    }
    char backup[BONGO_CAT_PATH_CAP];
    snprintf(backup, sizeof(backup), "%s.old", path);
    remove(backup);
    if (MoveFileExA(path, backup, MOVEFILE_REPLACE_EXISTING)) {
        if (rename(part_path, path) == 0) return true;
        MoveFileExA(backup, path, MOVEFILE_REPLACE_EXISTING);   /* 失败就还原 */
    }
    snprintf(error, capacity, "无法替换 %s (文件被占用)", path);
    return false;
}

/* 启动时清掉上次更新留下的 .old (那时文件已经不被占用) */
void bongo_cat_facetrack_cleanup_backups(const char *root) {
    if (!root || !root[0]) return;
    static const char *names[] = { "bongo_facetrack.dll", "onnxruntime.dll",
        "models/mnv3_detection_opt.onnx", "models/lm_model0_opt.onnx" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        char backup[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(path, sizeof(path), root, names[i])) continue;
        snprintf(backup, sizeof(backup), "%s.old", path);
        remove(backup);
    }
}

