/*
 * Core 导入的"压缩包侧": 白名单、路径安全、符号链接/特殊文件、上限、解压。
 *
 * 单独成文件的原因与攻击面有关: 这个文件处理的是**不可信输入**(用户从网上下到的 zip 可能
 * 被替换/篡改), 而 live2d_core_import.c 处理的是可信的本机磁盘状态。把二者分开, 安全规则
 * 可以单独审计、单独测试(test_core_import.c 的 zip 攻击面用例全打在这里)。
 *
 * 规则(逐条都是攻击面):
 *   1. 条目白名单: 只接受 .../Core/dll/<平台>/<架构>/<库名> 的普通文件, 其余条目**一个字节
 *      都不落盘** —— 压缩包里那上万个 SDK 源码文件与我们无关, 不该出现在用户磁盘上;
 *   2. zip-slip: 按**路径分量**判定(拒绝空段、`.`/`..`、绝对路径、盘符、UNC), 而不是
 *      strstr("..") —— 后者既误杀合法的 a..b, 又漏掉 UNC;
 *   3. 符号链接/重解析点/设备文件/加密条目: 查 zip 外部属性, 一律拒绝整包, 绝不物化链接;
 *   4. 上限: zip <= 512 MiB、单条目 <= 64 MiB、条目数 <= 20000、路径 <= 512 字符;
 *   5. 声明尺寸与实际写入都要对得上(见 extract_entry), 畸形条目不会写到一半。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include "live2d_core_internal.h"

#include <SDL3/SDL.h>
#include <miniz.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const core_platforms[] = {"windows", "linux", "macos"};
static const char *const core_arches[] = {"x86_64", "x86", "arm64"};
/* miniz 的随机读回调: 偏移来自归档自身的索引, 超过 INT64_MAX 直接失败, 否则
   SDL_SeekIO 截断后会在错误的位置继续读, 解压出垃圾。 */
static size_t read_archive(void *userdata, mz_uint64 offset, void *buffer,
    size_t size) {
    SDL_IOStream *stream = userdata;
    if (offset > INT64_MAX ||
        SDL_SeekIO(stream, (Sint64)offset, SDL_IO_SEEK_SET) < 0) return 0;
    return SDL_ReadIO(stream, buffer, size);
}

static unsigned char lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value - 'A' + 'a') : value;
}

static bool equal_ci(const char *left, size_t left_length, const char *right) {
    size_t right_length = strlen(right);
    if (left_length != right_length) return false;
    for (size_t i = 0; i < left_length; ++i)
        if (lower((unsigned char)left[i]) != lower((unsigned char)right[i]))
            return false;
    return true;
}

static bool in_list_ci(const char *text, size_t length,
    const char *const *list, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (equal_ci(text, length, list[i])) return true;
    return false;
}

/*
 * 归一化并校验条目路径: 反斜杠统一成 '/' 之后按分量判定。
 * 前置条件: name 是 zip 里记录的原始名字。
 * 后置条件: 通过时 output 是归一化路径(不含前导 '/'), 失败返回 false。
 */
static bool normalize_entry(const char *name, char *output, size_t capacity) {
    size_t length = strlen(name);
    if (!length || length >= capacity || length > BONGO_CAT_CORE_PATH_LIMIT) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)name[i];
        if (value < 0x20 || value == 0x7f) return false;
        output[i] = name[i] == '\\' ? '/' : name[i];
    }
    output[length] = '\0';
    if (output[0] == '/') return false;
    /* 盘符与 UNC: "C:..." 与 "//server" 在 Windows 上会写到别处。 */
    if (length >= 2 && output[1] == ':') return false;
    unsigned depth = 0;
    for (char *part = output; *part;) {
        char *end = strchr(part, '/');
        size_t part_length = end ? (size_t)(end - part) : strlen(part);
        if (!part_length) return false; /* 空段: 连续 '/' 或结尾 '/' */
        if (part_length == 1 && part[0] == '.') return false;
        if (part_length == 2 && part[0] == '.' && part[1] == '.') return false;
        /* 段尾是 '.' 或空格会被 Windows 静默截断, 让落地名与记录不符。 */
        if (part[part_length - 1] == '.' || part[part_length - 1] == ' ')
            return false;
        if (++depth > 12) return false;
        part = end ? end + 1 : part + part_length;
    }
    return true;
}

/* 把归一化后的路径切成段。返回段数(0 表示超过容量, 调用方按"太深"处理)。
   白名单判定与"这个包里有哪些平台"提示都基于同一份切分结果, 不重复解析字符串。 */
#define CORE_MAX_SEGMENTS 16
static int split_segments(const char *path, const char **starts,
    size_t *lengths) {
    int count = 0;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        if (count >= CORE_MAX_SEGMENTS) return 0;
        starts[count] = part;
        lengths[count] = end ? (size_t)(end - part) : strlen(part);
        ++count;
        if (!end) break;
        part = end + 1;
    }
    return count;
}

/* 白名单: 末五段必须是 Core / dll / <平台> / <架构> / <库名>。前缀(SDK 根目录名)任意,
   因为官方压缩包的顶层目录随版本变(CubismSdkForNative-5-r.5 之类)。 */
static bool entry_is_core(const char *path, const char *library) {
    const char *segments[CORE_MAX_SEGMENTS];
    size_t lengths[CORE_MAX_SEGMENTS];
    int count = split_segments(path, segments, lengths);
    if (count < 5) return false;
    int base = count - 5;
    return equal_ci(segments[base], lengths[base], "Core") &&
        equal_ci(segments[base + 1], lengths[base + 1], "dll") &&
        in_list_ci(segments[base + 2], lengths[base + 2], core_platforms, 3) &&
        in_list_ci(segments[base + 3], lengths[base + 3], core_arches, 3) &&
        equal_ci(segments[base + 4], lengths[base + 4], library);
}

/* 危险条目: 符号链接(Unix 模式位 0xA000)、Windows 重解析点、设备/管道等特殊文件。
   命中即拒绝整包 —— 与其"跳过这一条", 不如让用户换一个来源可信的压缩包。 */
static bool entry_is_unsafe(const mz_zip_archive_file_stat *stat) {
    unsigned kind = (unsigned)((stat->m_external_attr >> 16) & 0170000);
    if (kind && kind != 0100000 && kind != 0040000) return true;
    if (stat->m_external_attr & 0x400u) return true; /* FILE_ATTRIBUTE_REPARSE_POINT */
    return stat->m_is_encrypted || !stat->m_is_supported;
}

typedef struct CoreArchiveScan {
    mz_uint index;          /* 命中的条目序号 */
    bool matched;
    bool platform_mismatch; /* 有 Core, 但不是本机平台/架构 */
    char path[BONGO_CAT_CORE_PATH_LIMIT + 1];
    char sdk_hint[128];     /* 压缩包内的 SDK 根目录名, 便于支持排查 */
    uint64_t size;
    char found_platforms[128]; /* 出现过的 <平台>/<架构> 组合, 用于报错提示 */
} CoreArchiveScan;

/* 把 "windows/x86_64" 追加到提示串里(去重靠简单查找, 组合最多 9 种)。 */
static void note_platform(CoreArchiveScan *scan, const char *platform,
    size_t platform_length, const char *arch, size_t arch_length) {
    char entry[64];
    int written = snprintf(entry, sizeof(entry), "%.*s/%.*s", (int)platform_length,
        platform, (int)arch_length, arch);
    if (written <= 0 || (size_t)written >= sizeof(entry)) return;
    if (strstr(scan->found_platforms, entry)) return;
    size_t used = strlen(scan->found_platforms);
    if (used && used + 1 < sizeof(scan->found_platforms))
        scan->found_platforms[used++] = ',';
    snprintf(scan->found_platforms + used, sizeof(scan->found_platforms) - used,
        "%s", entry);
}

static BongoCatResult scan_archive(mz_zip_archive *zip, CoreArchiveScan *scan,
    BongoCatError *error) {
    mz_uint count = mz_zip_reader_get_num_files(zip);
    if (!count || count > BONGO_CAT_CORE_ENTRY_COUNT_LIMIT) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "The selected archive has an unusable entry count (%u)", (unsigned)count);
        return BONGO_CAT_ERROR_FORMAT;
    }
    const char *library = bongo_cat_live2d_core_library_name();
    const char *host_platform = bongo_cat_live2d_core_platform_name();
    const char *host_arch = bongo_cat_live2d_core_arch_name();
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(zip, i, &stat)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "The selected archive is damaged and cannot be read");
            return BONGO_CAT_ERROR_FORMAT;
        }
        mz_uint length = mz_zip_reader_get_filename(zip, i, NULL, 0);
        char name[BONGO_CAT_CORE_PATH_LIMIT + 2];
        if (!length || length >= sizeof(name)) {
            /* 超长名字不足以判定, 但绝不能"当作没看见": 直接拒绝整包。 */
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "The selected archive contains an entry with an over-long name");
            return BONGO_CAT_ERROR_FORMAT;
        }
        if (mz_zip_reader_get_filename(zip, i, name, sizeof(name)) != length)
            return BONGO_CAT_ERROR_FORMAT;
        if (entry_is_unsafe(&stat)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "The selected archive contains a link or a special file, which is "
                "never extracted: %s", name);
            return BONGO_CAT_ERROR_FORMAT;
        }
        if (!entry_is_core(name, library)) continue;
        if (stat.m_uncomp_size > BONGO_CAT_CORE_ENTRY_LIMIT) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "The Cubism Core inside the archive is larger than the accepted "
                "limit (%llu bytes)", (unsigned long long)stat.m_uncomp_size);
            return BONGO_CAT_ERROR_FORMAT;
        }
        /* 白名单已经保证末五段的形状, 所以平台/架构就是倒数第 4/3 段。
           记下这个包里出现过哪些组合, 报错时能直接告诉用户他下的是什么。 */
        const char *segments[CORE_MAX_SEGMENTS];
        size_t lengths[CORE_MAX_SEGMENTS];
        /* 不要把名字取成 count: 外层那个 count 是包内条目总数, 而这里是本条目路径
           切出来的段数; 同名会在 /WX 下直接编译失败 (C4456)。 */
        int segment_count = split_segments(name, segments, lengths);
        if (segment_count < 5) continue;
        int base = segment_count - 5;
        const char *platform = segments[base + 2], *arch = segments[base + 3];
        note_platform(scan, platform, lengths[base + 2], arch, lengths[base + 3]);
        bool host_match = equal_ci(platform, lengths[base + 2], host_platform) &&
            equal_ci(arch, lengths[base + 3], host_arch);
        if (!host_match) {
            scan->platform_mismatch = true;
            continue;
        }
        /* 同一架构可能有多个命中(重复条目): 取第一个, 后面按同样规则忽略。 */
        if (scan->matched) continue;
        scan->matched = true;
        scan->index = i;
        scan->size = stat.m_uncomp_size;
        snprintf(scan->path, sizeof(scan->path), "%s", name);
        const char *root_end = strstr(name, "/Core/");
        if (root_end && (size_t)(root_end - name) < sizeof(scan->sdk_hint))
            snprintf(scan->sdk_hint, sizeof(scan->sdk_hint), "%.*s",
                (int)(root_end - name), name);
    }
    if (!scan->matched) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "No Cubism Core for %s/%s was found inside the archive (it contains: %s)",
            host_platform, host_arch,
            scan->found_platforms[0] ? scan->found_platforms : "no Core at all");
        return BONGO_CAT_ERROR_FORMAT;
    }
    return BONGO_CAT_OK;
}

/* 把命中的条目解压到 staging 文件: 先整块读进内存(有 64 MiB 上限)再写盘, 并以
   "实际写入字节"记账 —— 声明尺寸与实际不符时立即失败, 不让畸形容器写到一半。 */
static BongoCatResult extract_entry(mz_zip_archive *zip,
    const CoreArchiveScan *scan, const char *target, BongoCatError *error) {
    size_t size = (size_t)scan->size;
    unsigned char *data = size ? malloc(size) : NULL;
    if (size && !data) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate %llu bytes to read the Cubism Core",
            (unsigned long long)size);
        return BONGO_CAT_ERROR_MEMORY;
    }
    BongoCatResult result = BONGO_CAT_OK;
    if (!mz_zip_reader_extract_to_mem(zip, scan->index, data, size, 0)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Cannot extract the Cubism Core from the archive");
        result = BONGO_CAT_ERROR_FORMAT;
    } else {
        FILE *file = bongo_cat_file_open(target, "wb");
        size_t written = file ? fwrite(data, 1, size, file) : 0;
        bool closed = file && fclose(file) == 0;
        if (!file || written != size || !closed) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot write the Cubism Core into the staging directory");
            result = BONGO_CAT_ERROR_IO;
        }
    }
    free(data);
    return result;
}

/* 压缩包入口: 打开 -> 扫描(白名单/安全/上限) -> 解出唯一命中的那一个 Core 到 staged。
   SDL_IOFromFile 走 UTF-8 路径(Windows 上内部转宽字符), 所以中文/emoji 目录也能用。 */
BongoCatResult bongo_cat_live2d_core_extract_archive(const char *source,
    const char *staged, char *sdk_hint, size_t sdk_hint_capacity,
    BongoCatError *error) {
    if (sdk_hint && sdk_hint_capacity) sdk_hint[0] = '\0';
    SDL_IOStream *input = SDL_IOFromFile(source, "rb");
    if (!input) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot open the selected archive: %s", source);
        return BONGO_CAT_ERROR_IO;
    }
    Sint64 size = SDL_GetIOSize(input);
    if (size <= 0 || (uint64_t)size > BONGO_CAT_CORE_ZIP_LIMIT) {
        SDL_CloseIO(input);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "The selected archive is empty or larger than 512 MiB");
        return BONGO_CAT_ERROR_FORMAT;
    }
    mz_zip_archive zip = {0};
    zip.m_pRead = read_archive;
    zip.m_pIO_opaque = input;
    BongoCatResult result = BONGO_CAT_ERROR_FORMAT;
    if (mz_zip_reader_init(&zip, (mz_uint64)size, 0)) {
        CoreArchiveScan scan = {0};
        result = scan_archive(&zip, &scan, error);
        if (result == BONGO_CAT_OK) {
            if (sdk_hint && sdk_hint_capacity)
                snprintf(sdk_hint, sdk_hint_capacity, "%s", scan.sdk_hint);
            result = extract_entry(&zip, &scan, staged, error);
        }
        mz_zip_reader_end(&zip);
    } else {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "The selected file is not a readable ZIP archive: %s", source);
    }
    SDL_CloseIO(input);
    return result;
}