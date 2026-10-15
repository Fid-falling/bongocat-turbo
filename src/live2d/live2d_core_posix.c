/*
 * POSIX(Linux/macOS)上的 Cubism Core 动态库加载与校验。与 windows_live2d_core.c 一一对应,
 * 语义必须保持一致(三符号探测、架构判定、错误分类), 否则"同一份 SDK 换个系统就报不同原因"。
 *
 * RTLD_NOW: 立刻解析全部重定位, 让"缺符号"在加载期就暴露成本函数的失败, 而不是等第一次
 * 渲染时才在用户面前崩。RTLD_LOCAL: 不把 Core 的符号灌进全局命名空间 —— 否则插件里同名
 * 符号(比如将来某个自带的 csm 兼容层)会与 Core 的符号互相抢占。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    /* dlerror 是"读一次就清空"的接口: 必须先清掉上一次的残留, 否则可能读到旧消息。 */
    (void)dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *reason = dlerror();
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Cannot load the Cubism Core library: %s", reason ? reason : path);
        return NULL;
    }
    return handle;
}

void bongo_cat_platform_live2d_core_close(void *handle) {
    if (handle) dlclose(handle);
}

void *bongo_cat_platform_live2d_core_symbol(void *handle, const char *name) {
    if (!handle || !name || !name[0]) return NULL;
    return dlsym(handle, name);
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

/* 读文件头若干字节; 头部长度不足直接失败(空文件/被截断的下载)。 */
static bool read_head(const char *path, void *buffer, size_t size) {
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    bool ok = fread(buffer, 1, size, file) == size;
    fclose(file);
    return ok;
}

/* ELF: e_machine(offset 18, 小端) 必须是本机架构。 */
static bool elf_arch_matches(const unsigned char *head) {
    if (head[4] != 2) return false; /* 只支持 64 位; 32 位 SDK 早就不再发布 */
    unsigned machine = (unsigned)head[18] | ((unsigned)head[19] << 8);
#if defined(__aarch64__) || defined(__arm64__)
    return machine == 0xb7;
#elif defined(__x86_64__)
    return machine == 0x3e;
#else
    return machine == 0x03;
#endif
}

/* Mach-O: 支持 fat(0xcafebabe)与 thin(64 位 0xfeedfacf)。 */
static bool macho_arch_matches(const unsigned char *head) {
    uint32_t magic = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16) |
        ((uint32_t)head[2] << 8) | (uint32_t)head[3];
    if (magic == 0xcafebabe) return true; /* fat 包含本机切片, 交给 dyld 选 */
    if (magic != 0xfeedfacf) return false;
    uint32_t cpu = ((uint32_t)head[4] << 24) | ((uint32_t)head[5] << 16) |
        ((uint32_t)head[6] << 8) | (uint32_t)head[7];
#if defined(__aarch64__) || defined(__arm64__)
    return cpu == 0x0100000c;
#elif defined(__x86_64__)
    return cpu == 0x01000007;
#else
    return false;
#endif
}

bool bongo_cat_platform_live2d_core_arch_matches(const char *path) {
    unsigned char head[32];
    if (!read_head(path, head, sizeof(head))) return false;
    if (head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F')
        return elf_arch_matches(head);
    return macho_arch_matches(head);
}
