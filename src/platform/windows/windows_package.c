#include "windows_package.h"
#include "windows_utf8.h"

#ifdef _WIN32
#include "bongo_cat/common.h"

#include <limits.h>
#include <stdlib.h>
#include <windows.h>
#include <shlobj.h>

/* 运行时从 kernel32 查找打包相关的导出: 老系统上没有这些符号, 静态链接会导致加载
   失败。找不到就当作"不是打包版" —— 这是唯一安全的降级方向。 */
typedef LONG (WINAPI *BongoCatGetPackageName)(UINT32 *, PWSTR);

static BongoCatGetPackageName package_api(const char *name) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    return kernel ? (BongoCatGetPackageName)(void *)GetProcAddress(kernel, name)
        : NULL;
}

/* 判定手法: 用长度为 0 的缓冲去问 GetCurrentPackageFullName —— 未打包时返回
   APPMODEL_ERROR_NO_PACKAGE, 已打包时返回 ERROR_INSUFFICIENT_BUFFER(需要更大缓冲)。
   靠错误码就能区分, 不必真的取出包名。 */
bool bongo_cat_windows_is_packaged(void) {
    BongoCatGetPackageName get_name = package_api("GetCurrentPackageFullName");
    if (!get_name) return false;
    UINT32 length = 0;
    return get_name(&length, NULL) == ERROR_INSUFFICIENT_BUFFER;
}

static wchar_t *package_family(void) {
    BongoCatGetPackageName get_family =
        package_api("GetCurrentPackageFamilyName");
    if (!get_family) return NULL;
    UINT32 length = 0;
    if (get_family(&length, NULL) != ERROR_INSUFFICIENT_BUFFER || !length)
        return NULL;
    wchar_t *family = calloc(length, sizeof(*family));
    if (!family || get_family(&length, family) != ERROR_SUCCESS) {
        free(family);
        return NULL;
    }
    return family;
}

/* 优先读环境变量(快); 变量缺失或过长时退回 SHGetFolderPath(慢一些但更可靠)。 */
static bool user_profile(wchar_t output[BONGO_CAT_PATH_CAP]) {
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", output,
        BONGO_CAT_PATH_CAP);
    if (length > 0 && length < BONGO_CAT_PATH_CAP) return true;
    output[0] = L'\0';
    return SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, output));
}

/* 目标路径是 %USERPROFILE%\AppData\Local\Packages\<包族名>\LocalCache\Local\
   <应用名>: 这是 MSIX 容器里"既对进程可写、卸载时又会被清理"的位置。
   capacity 超过 INT_MAX 时直接拒绝, 免得下游把它截成 int 时变成负数。 */
bool bongo_cat_windows_package_storage_root_for(const wchar_t *profile,
    const wchar_t *family, char *output, size_t capacity) {
    if (!output || !capacity || capacity > INT_MAX) return false;
    output[0] = '\0';
    if (!profile || !profile[0] || !family || !family[0]) return false;
    wchar_t path[BONGO_CAT_PATH_CAP];
    int length = swprintf(path, BONGO_CAT_PATH_CAP,
        L"%ls\\AppData\\Local\\Packages\\%ls\\LocalCache\\Local\\%ls",
        profile, family, BONGO_CAT_STORAGE_DIR_W);
    return length > 0 && length < BONGO_CAT_PATH_CAP &&
        bongo_cat_windows_utf8(path, output, capacity);
}

/* 三个前提都成立才算成功: 确实是打包版、能拿到包族名、能确定用户目录。任何一步失败
   都返回 false, 让调用方退回普通目录策略。 */
bool bongo_cat_windows_package_storage_root(char *output, size_t capacity) {
    if (!output || !capacity) return false;
    output[0] = '\0';
    if (!bongo_cat_windows_is_packaged()) return false;
    wchar_t profile[BONGO_CAT_PATH_CAP] = {0};
    wchar_t *family = package_family();
    bool resolved = family && user_profile(profile) &&
        bongo_cat_windows_package_storage_root_for(profile, family,
            output, capacity);
    free(family);
    return resolved;
}
#endif
