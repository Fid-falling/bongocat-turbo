/*
 * 面捕组件的运行库预装载 (Windows)。
 *
 * 为什么需要: 组件 bongo_facetrack.dll 依赖 onnxruntime.dll, 两者由宿主下载到同一个
 * 目录。Windows 解析依赖时先看**宿主 exe 所在目录**和 PATH, 而不是"正在加载的这个 DLL"
 * 的目录 —— 于是很容易撞上系统里另一份同名 onnxruntime.dll (Edge、VS Code 都自带),
 * GetApi(20) 直接返回空, 表现成"组件 ABI 不匹配"。
 *
 * 做法: 先把组件目录里的 onnxruntime.dll 用 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 装进来,
 * 之后组件加载时就会绑定到这一份。
 */
#include "bongo_cat/platform.h"

#include <windows.h>

#include <string.h>

void bongo_cat_platform_preload_facetrack_runtime(const char *dll_path) {
    if (!dll_path || !dll_path[0]) return;
    char dir[BONGO_CAT_PATH_CAP] = {0};
    size_t length = strlen(dll_path);
    if (length >= sizeof(dir)) return;
    memcpy(dir, dll_path, length + 1);
    char *slash = strrchr(dir, '\\');
    char *forward = strrchr(dir, '/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (!slash) return;
    *slash = 0;
    char runtime[BONGO_CAT_PATH_CAP] = {0};
    const size_t dir_length = strlen(dir);
    if (dir_length + 20 >= sizeof(runtime)) return;
    memcpy(runtime, dir, dir_length);
    memcpy(runtime + dir_length, "\\onnxruntime.dll", sizeof("\\onnxruntime.dll"));
    if (GetFileAttributesA(runtime) == INVALID_FILE_ATTRIBUTES) return;
    LoadLibraryExA(runtime, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
