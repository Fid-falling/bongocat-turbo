/*
 * Windows 上的 Cubism Core 动态库加载与校验。
 *
 * 为什么不用 SDL_LoadObject: SDL 的加载器把错误信息塞进 SDL_GetError 的单例字符串,
 * 而"Core 加载失败"是用户要照着排查的路径(缺依赖 DLL / 架构不对 / 被安全软件拦),
 * 这里需要 GetLastError 的原始码与明确分类。所以直接用 LoadLibraryW, 只在探测符号时
 * 复用 SDL_LoadFunction 的语义(GetProcAddress + UTF-8 名字)。
 *
 * 坑位记录:
 *   * LoadLibrary 会在"找不到依赖 DLL"时也成功返回后再在需要时才失败(延迟加载),
 *     所以必须在加载后**立刻**探测三个符号, 而不是等第一次渲染时才用;
 *   * 路径必须走 bongo_cat_windows_wide: Core 可能被用户放在带中文/emoji 的目录里;
 *   * 加载成功的句柄要一直持有到模型实例与 GPU 资源全部销毁为止(见 live2d_core.h)。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"

#include "windows_utf8.h"

#include <SDL3/SDL.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Core 的三个导出符号。三符号而不是一个: 只查 csmGetVersion 会把任何"同名但不同版本/
   不同库"的 DLL 当成可用 Core, 而真正崩的地方是后面的 moc 复活与模型初始化。 */
static const char *const core_probe_symbols[] = {
    "csmGetVersion",
    "csmReviveMocInPlace",
    "csmInitializeModelInPlace"
};

void *bongo_cat_platform_live2d_core_open(const char *path, BongoCatError *error) {
    if (!path || !path[0]) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot load Cubism Core without a path");
        return NULL;
    }
    wchar_t *wide = bongo_cat_windows_wide(path);
    if (!wide) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cubism Core path is not valid UTF-8: %s", path);
        return NULL;
    }
    /* LOAD_WITH_ALTERED_SEARCH_PATH: 官方 SDK 解压目录里 Core 旁边可能还有别的 DLL,
       让同目录优先于系统目录, 避免撞上同名旧版本。 */
    HMODULE module = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD failure = GetLastError();
    free(wide);
    if (!module) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Cannot load the Cubism Core library (%lu): %s", (unsigned long)failure,
            path);
        return NULL;
    }
    return (void *)module;
}

void bongo_cat_platform_live2d_core_close(void *handle) {
    if (handle) FreeLibrary((HMODULE)handle);
}

void *bongo_cat_platform_live2d_core_symbol(void *handle, const char *name) {
    if (!handle || !name || !name[0]) return NULL;
    return (void *)GetProcAddress((HMODULE)handle, name);
}

bool bongo_cat_platform_live2d_core_probe(void *handle) {
    if (!handle) return false;
    for (size_t i = 0; i < sizeof(core_probe_symbols) / sizeof(core_probe_symbols[0]);
        ++i) {
        if (!bongo_cat_platform_live2d_core_symbol(handle,
            core_probe_symbols[i])) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] Core 缺少导出符号 %s, 判定为不可用",
                core_probe_symbols[i]);
            return false;
        }
    }
    return true;
}

/* PE 头里的 Machine 字段必须与本进程一致。
 *
 * 为什么不用 IsWow64Process 之类: 用户可能拿来 x86 或 ARM64 的 Core(官方 SDK 三个架构
 * 都提供)。架构不匹配时 LoadLibrary 在 x64 进程里加载 x86 DLL 会失败并给出一个含糊的
 * 错误码(193, %1 is not a valid Win32 application), 用户看不懂; 这里提前给出"你选的是
 * x86 的包, 本机是 x86_64"这种能直接照做的结论。 */
bool bongo_cat_platform_live2d_core_arch_matches(const char *path) {
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    unsigned char header[64];
    bool read = fread(header, 1, sizeof(header), file) == sizeof(header);
    fclose(file);
    if (!read || header[0] != 'M' || header[1] != 'Z') return false;
    long pe_offset = (long)header[0x3c] | ((long)header[0x3d] << 8) |
        ((long)header[0x3e] << 16) | ((long)header[0x3f] << 24);
    if (pe_offset <= 0) return false;
    file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    unsigned char machine[6] = {0};
    bool ok = fseek(file, pe_offset, SEEK_SET) == 0 &&
        fread(machine, 1, sizeof(machine), file) == sizeof(machine);
    fclose(file);
    if (!ok || machine[0] != 'P' || machine[1] != 'E' || machine[2] != 0 ||
        machine[3] != 0) return false;
    unsigned machine_id = (unsigned)machine[4] | ((unsigned)machine[5] << 8);
#if defined(_M_ARM64)
    const unsigned expected = 0xaa64;
#elif defined(_WIN64)
    const unsigned expected = 0x8664;
#else
    const unsigned expected = 0x014c;
#endif
    return machine_id == expected;
}
