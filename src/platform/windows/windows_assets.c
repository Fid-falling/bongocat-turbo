#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <miniz.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/*
 * 内嵌资源包(.pak, 由 src/tools/asset_packer.c 在构建期生成)的读取侧。
 *
 * 格式与打包器严格对应: 8 字节魔数 "L2DPAK1\0" + uint32 条目数, 之后每项是
 * [uint16 路径长度][uint16 flags][uint32 负载长度][uint64 负载偏移] + 路径字节; 负载区
 * 紧跟条目表。flags 第 0 位表示该负载是 deflate, 此时负载内容为
 * [uint32 原始大小][deflate 数据]。
 * 所有整数都是小端, 所以下面三个 read* 手写字节拼接, 不做指针类型转换(不同架构的
 * 对齐与字节序都可能不同)。
 *
 * 为什么要落盘缓存: 资源以 PE 资源(RCDATA 101)的形式内嵌在 exe 里, 运行时不能直接当
 * 文件用(模型要按路径读旁边的贴图), 所以首次启动解包到缓存目录, 并写一个 "complete"
 * 标记文件记录内容摘要 —— 摘要不变就跳过解包。
 *
 * 包里有两段, 靠路径首段区分去向(打包器只按 ROOT 下的结构存路径, 见 asset_packer.c):
 *   * assets/  —— 应用程序资源, 解到调用方给的资源缓存目录(embedded-assets-<版本>/);
 *   * cores/   —— 我们自己的渲染组件(bongo_live2d.dll), 是**程序自身的一部分**, 首次启动
 *                按需释放到 <存储根>/cores/(见文件末尾的 release_component)。它不能跟着
 *                资源缓存走: 组件目录的解析只看 <exe>/cores 与 <存储根>/cores, 而且资源缓存
 *                会随版本换目录, 组件却必须与 exe 同版本共存。
 */
/* 资源段的路径前缀 + 组件条目名(与 cmake/RendererModule.cmake 的 OUTPUT_NAME 对齐)。 */
#define ASSET_PATH_PREFIX "assets/"
#define COMPONENT_DIRECTORY "cores"
#define COMPONENT_LIBRARY "bongo_live2d.dll"
static uint16_t read16(const unsigned char *data) {
    return (uint16_t)(data[0] | (uint16_t)data[1] << 8);
}

static uint32_t read32(const unsigned char *data) {
    uint32_t value = 0;
    for (int i = 3; i >= 0; --i) value = value << 8 | data[i];
    return value;
}

static uint64_t read64(const unsigned char *data) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = value << 8 | data[i];
    return value;
}

/* 包内路径最终会被解包到磁盘, 所以必须挡住目录穿越: 拒绝绝对路径、任何 ".." 片段
   (但允许 "a..b" 这种文件名)以及带冒号的路径(会变成 NTFS 数据流或盘符)。 */
static bool safe_name(const char *name) {
    if (!name[0] || name[0] == '/' || name[0] == '\\') return false;
    const char *cursor = name;
    while ((cursor = strstr(cursor, ".."))) {
        bool left = cursor == name || cursor[-1] == '/' || cursor[-1] == '\\';
        bool right = !cursor[2] || cursor[2] == '/' || cursor[2] == '\\';
        if (left && right) return false;
        cursor += 2;
    }
    return strchr(name, ':') == NULL;
}

/*
 * 条目游标。条目表是变长的(每项 16 字节头 + 变长路径), 所以只能顺序走。
 * 走的每一步都校验边界: 输入虽然来自自己的 exe, 但打包流程改动或文件被替换都可能让它前后
 * 不一致 —— 宁可返回 false 也不要越界读。
 */
typedef struct PackCursor {
    const unsigned char *data;
    size_t size;
    size_t offset;
} PackCursor;

typedef struct PackEntry {
    char name[BONGO_CAT_PATH_CAP];
    uint16_t flags;
    uint32_t payload_size;
    size_t payload_offset;
} PackEntry;

/* 校验魔数与条目数, 把游标定位到条目表开头。 */
static bool pack_begin(const unsigned char *data, size_t size,
    PackCursor *cursor) {
    const unsigned char magic[8] = {'L', '2', 'D', 'P', 'A', 'K', '1', 0};
    if (!data || size < 12 || memcmp(data, magic, sizeof(magic)) != 0)
        return false;
    if (read32(data + 8) > 4096) return false;
    cursor->data = data;
    cursor->size = size;
    cursor->offset = 12;
    return true;
}

/* 取下一条。条目表走完(游标越界)返回 false —— 调用方因此不需要预先知道条目数。 */
static bool pack_next(PackCursor *cursor, PackEntry *entry) {
    if (!cursor || !entry || cursor->offset + 16 > cursor->size) return false;
    const unsigned char *header = cursor->data + cursor->offset;
    uint16_t name_size = read16(header);
    uint32_t payload_size = read32(header + 4);
    uint64_t payload_offset = read64(header + 8);
    size_t offset = cursor->offset + 16;
    if (!name_size || name_size >= sizeof(entry->name) ||
        offset + name_size > cursor->size ||
        payload_offset > cursor->size ||
        payload_size > cursor->size - (size_t)payload_offset) return false;
    memcpy(entry->name, cursor->data + offset, name_size);
    entry->name[name_size] = '\0';
    entry->flags = read16(header + 2);
    entry->payload_size = payload_size;
    entry->payload_offset = (size_t)payload_offset;
    cursor->offset = offset + name_size;
    return safe_name(entry->name);
}

/* 资源段的条目才解到资源缓存; cores/ 段由 release_component 单独处理。 */
static bool asset_entry(const PackEntry *entry) {
    return strncmp(entry->name, ASSET_PATH_PREFIX,
        strlen(ASSET_PATH_PREFIX)) == 0;
}

/* 在运行期拼出包内组件条目路径 "cores/bongo_live2d.dll"。
 *
 * 刻意**不**把这串路径写成一个源码字面量: 发布门禁
 * (cmake/CubismCompliance.cmake 的 bongo_cat_audit_live2d_component)正是靠"这串字节在 exe
 * 里出现"来判定"组件确实被打进了内嵌资源包"。源码里的字面量会无条件出现在 exe 里, 那样门禁
 * 就成了一条永远通过的假断言 —— 比没有断言更糟, 因为它会让人以为查过了。拼出来之后, 完整的
 * "cores/bongo_live2d.dll" 只会来自资源包的条目表。改这里时请保留这个性质。 */
static bool component_entry_path(char *output, size_t capacity) {
    int written = snprintf(output, capacity, "%s/%s", COMPONENT_DIRECTORY,
        COMPONENT_LIBRARY);
    return written > 0 && (size_t)written < capacity;
}

static bool write_file(const char *path, const unsigned char *data, size_t size,
    BongoCatError *error) {
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot create embedded asset: %s", path);
        return false;
    }
    bool ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) ok = false;
    if (!ok) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot write embedded asset: %s", path);
    return ok;
}

/* 只对少数关键文件做逐字节校验(见下面这份清单): 它们是启动必需且体积很小。
   模型目录只校验"存在且大小一致", 因为标准模型有几百个文件, 全量比对会明显拖慢启动;
   "/resources/" 也被排除, 那是用户自己可以替换的贴图目录。 */
static bool verify_contents(const char *name) {
    const char *model = "assets/models/standard/";
    return strcmp(name, "assets/locales/en-US.json") == 0 ||
        strcmp(name, "assets/bongocat.png") == 0 ||
        strcmp(name, "assets/ui-symbols.png") == 0 ||
        strcmp(name, "assets/ui-symbols@4x.png") == 0 ||
        strcmp(name, "assets/catime.png") == 0 ||
        strcmp(name, "assets/vlaina.jpg") == 0 ||
        (strncmp(name, model, strlen(model)) == 0 && !strstr(name, "/resources/"));
}

/* 分块比对而不是整块读入: 期望数据直接指向 PE 资源映射的内存, 只需流式比一遍。
   末尾多读一个字节必须得到 EOF —— 磁盘上的文件比包里长同样算内容不一致。 */
static bool content_matches(const char *path, const unsigned char *expected, size_t size) {
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    unsigned char buffer[8192]; size_t offset = 0;
    while (offset < size) {
        size_t count = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
        if (fread(buffer, 1, count, file) != count ||
            memcmp(buffer, expected + offset, count) != 0) {
            fclose(file); return false;
        }
        offset += count;
    }
    bool ok = fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

/* 标记摘要没变时走的快速路径: 只检查包内每个**资源**文件在磁盘上存在且大小一致, 关键文件
   再比对内容。任何一项不满足就返回 false, 触发完整解包。cores/ 段不在这里检查 —— 它落在
   <存储根>/cores/, 与这个缓存目录无关, 由 release_component 自己按哈希判定。 */
static bool packed_files_present(const unsigned char *data, size_t size,
    const char *target) {
    PackCursor cursor;
    PackEntry entry;
    if (!pack_begin(data, size, &cursor)) return false;
    while (pack_next(&cursor, &entry)) {
        if (!asset_entry(&entry)) continue;
        char path[BONGO_CAT_PATH_CAP];
        uint64_t actual_size;
        if (!bongo_cat_path_join(path, sizeof(path), target, entry.name) ||
            !bongo_cat_path_file_size(path, &actual_size) ||
            actual_size != entry.payload_size) return false;
        if (verify_contents(entry.name) && !content_matches(path,
            data + entry.payload_offset, entry.payload_size)) return false;
    }
    return true;
}

static bool current_pack(const char *marker, const char *target,
    const unsigned char *data, size_t size, const char expected[65]) {
    FILE *file = bongo_cat_file_open(marker, "rb");
    if (!file) return false;
    char actual[65] = {0};
    bool read = fread(actual, 1, 64, file) == 64;
    fclose(file);
    return read && strcmp(actual, expected) == 0 &&
        packed_files_present(data, size, target);
}

/*
 * 真正解包(只解 assets/ 段)。每一步都要检查边界(条目表长度、路径长度、负载偏移加长度是否
 * 越界): 输入虽然来自自己的 exe, 但打包流程改动或文件被替换都可能让它前后不一致。
 * 负载有两种形态: flags & 1 为压缩(内容是 [uint32 原始大小][deflate 数据]), 否则就是
 * 原文 —— 与 asset_packer.c 的 build_payload 一一对应。
 * 解压出来的长度必须严格等于原始大小, 否则视为损坏, 免得写出被截断的资源。
 */
static bool extract_pack(const unsigned char *data, size_t size, const char *target,
    BongoCatError *error) {
    PackCursor cursor;
    PackEntry entry;
    if (!pack_begin(data, size, &cursor)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT, "Invalid embedded asset pack");
        return false;
    }
    while (pack_next(&cursor, &entry)) {
        if (!asset_entry(&entry)) continue;
        char path[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(path, sizeof(path), target, entry.name)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT, "Unsafe embedded asset path");
            return false;
        }
        const char *slash = strrchr(path, '/');
        if (slash) {
            char parent[BONGO_CAT_PATH_CAP];
            size_t length = (size_t)(slash - path);
            memcpy(parent, path, length); parent[length] = '\0';
            if (!bongo_cat_path_create_directory(parent)) return false;
        }
        /* 压缩条目: 负载是 [uint32 原始大小][deflate 数据] */
        if (entry.flags & 1) {
            if (entry.payload_size < 4) return false;
            uint32_t raw_size = read32(data + entry.payload_offset);
            unsigned char *buffer = malloc(raw_size ? raw_size : 1);
            if (!buffer) {
                bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
                    "Cannot allocate decompressed asset");
                return false;
            }
            mz_ulong length = raw_size;
            int status = raw_size ?
                mz_uncompress(buffer, &length, data + entry.payload_offset + 4,
                    entry.payload_size - 4) : MZ_OK;
            bool written = status == MZ_OK && length == raw_size &&
                write_file(path, buffer, raw_size, error);
            free(buffer);
            if (!written) {
                if (status != MZ_OK) bongo_cat_error_set(error,
                    BONGO_CAT_ERROR_FORMAT, "Cannot decompress embedded asset");
                return false;
            }
            continue;
        }
        if (!write_file(path, data + entry.payload_offset, entry.payload_size, error))
            return false;
    }
    return true;
}

/* 取出内嵌资源包(RCDATA 101)在内存里的位置。资源是 PE 映像的一部分, 不需要释放。 */
static const unsigned char *embedded_pack(size_t *size) {
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    HGLOBAL loaded = resource ? LoadResource(NULL, resource) : NULL;
    if (size) *size = loaded ? (size_t)SizeofResource(NULL, resource) : 0;
    return loaded ? (const unsigned char *)LockResource(loaded) : NULL;
}

/* 入口: 找到 PE 资源(RCDATA 101) -> 算出它的 SHA-256 作为摘要 -> 缓存目录里的 marker
   摘要一致就直接返回 -> 否则完整解包并写 marker(64 字节的十六进制摘要, 不含结尾 NUL)。
   打包器必须产出确定性结果, 否则每次构建摘要都不同, 缓存会反复重建。 */
BongoCatResult bongo_cat_platform_embedded_assets(const char *target, BongoCatError *error) {
    if (!target) return BONGO_CAT_ERROR_ARGUMENT;
    char marker[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(marker, sizeof(marker), target, "complete");
    size_t size = 0;
    const unsigned char *data = embedded_pack(&size);
    if (!data || !size) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Embedded application assets are unavailable");
        return BONGO_CAT_ERROR_IO;
    }
    char digest[65];
    bongo_cat_sha256_bytes(data, size, digest);
    if (current_pack(marker, target, data, size, digest)) return BONGO_CAT_OK;
    SDL_Log("Embedded asset cache is incomplete; rebuilding it");
    if (!bongo_cat_path_create_directory(target) ||
        !extract_pack(data, size, target, error)) {
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_IO, "Cannot rebuild the embedded asset cache: %s",
            SDL_GetError());
        return BONGO_CAT_ERROR_IO;
    }
    return write_file(marker, (const unsigned char *)digest, 64, error)
        ? BONGO_CAT_OK : BONGO_CAT_ERROR_IO;
}

/* 把组件条目取出来(必要时解压)成一块可写到磁盘的字节。1 MiB 级别, 一次性读入即可。
   这里再核一遍负载范围: pack_next 已经查过一次, 但"内存里解出一块要写进磁盘的字节"是
   整条链路上后果最重的一步(写出被截断的 DLL 会让下次启动加载失败), 多一次边界检查值得。 */
static unsigned char *component_bytes(const unsigned char *data, size_t size,
    const PackEntry *entry, uint32_t *output_size, BongoCatError *error) {
    if (entry->payload_offset > size ||
        entry->payload_size > size - entry->payload_offset) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "The embedded renderer component entry is out of bounds");
        return NULL;
    }
    const unsigned char *payload = data + entry->payload_offset;
    uint32_t raw_size = entry->payload_size;
    if (entry->flags & 1) {
        if (entry->payload_size < 4) return NULL;
        raw_size = read32(payload);
        payload += 4;
    }
    unsigned char *buffer = malloc(raw_size ? raw_size : 1);
    if (!buffer) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate the embedded renderer component");
        return NULL;
    }
    if (!(entry->flags & 1)) {
        memcpy(buffer, payload, raw_size);
    } else {
        mz_ulong length = raw_size;
        int status = raw_size ? mz_uncompress(buffer, &length, payload,
            entry->payload_size - 4) : MZ_OK;
        if (status != MZ_OK || length != raw_size) {
            free(buffer);
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "Cannot decompress the embedded renderer component");
            return NULL;
        }
    }
    *output_size = raw_size;
    return buffer;
}

/*
 * 按需把内嵌的渲染组件释放到 <存储根>/cores/(参数 directory 就是那个目录)。
 *
 * 为什么"按需": 组件只有 1 MB, 但它必须落在 <存储根>/cores/ 才会被现有的目录解析看见
 * (见 runtime/cores/renderer_module.c), 而每次启动无条件覆盖既慢又会在组件正被别的实例
 * 加载时失败。所以判定顺序是:
 *   1. 目标文件在、且与内嵌字节逐字节一致 -> 什么都不做(绝大多数启动走这条);
 *   2. 否则先写同目录下的临时文件, 再用 bongo_cat_file_replace 原子改名覆盖
 *      —— 半写的 DLL 比旧 DLL 危险得多(加载它可能直接崩), 所以绝不允许"就地覆盖";
 *   3. 任何一步失败都只报错 + 返回失败, **不**影响启动: 缺组件时 Live2D 走诊断兜底,
 *      这与"用户没导入 Core"是同一种降级, 不能因为它让程序起不来。
 *
 * 目标文件被另一个正在运行的实例加载着时替换会失败(Windows 不允许覆盖已映射的映像),
 * 这也归到第 3 条: 报清楚原因, 然后照常以旧组件或无组件继续。
 */
BongoCatResult bongo_cat_platform_embedded_assets_release_component(
    const char *directory, BongoCatError *error) {
    if (!directory || !directory[0]) return BONGO_CAT_ERROR_ARGUMENT;
    size_t size = 0;
    const unsigned char *data = embedded_pack(&size);
    if (!data || !size) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Embedded application assets are unavailable");
        return BONGO_CAT_ERROR_IO;
    }
    PackCursor cursor;
    PackEntry entry;
    if (!pack_begin(data, size, &cursor)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Invalid embedded asset pack");
        return BONGO_CAT_ERROR_FORMAT;
    }
    bool found = false;
    char entry_path[BONGO_CAT_PATH_CAP];
    if (!component_entry_path(entry_path, sizeof(entry_path))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot compose the embedded renderer component entry name");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    while (pack_next(&cursor, &entry)) {
        if (strcmp(entry.name, entry_path) == 0) { found = true; break; }
    }
    /* 诊断构建(没有 Cubism SDK)里包里本来就没有组件条目 —— 这不是错误, 只是没什么可释放。 */
    if (!found) return BONGO_CAT_ERROR_PLATFORM;
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), directory, COMPONENT_LIBRARY) ||
        !bongo_cat_path_join(temporary, sizeof(temporary), directory,
            COMPONENT_LIBRARY ".tmp")) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot compose the renderer component path under %s", directory);
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    /* 目标已是最新: 逐字节比对(内嵌字节就在内存里, 流式比一遍即可, 不必先解压)。
       只有压缩条目才需要先解开再比 —— 那时也顺手复用它当写入源。 */
    if (!(entry.flags & 1)) {
        if (content_matches(path, data + entry.payload_offset, entry.payload_size)) {
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 内嵌渲染组件已就位(哈希一致, 无需释放): %s", path);
            return BONGO_CAT_OK;
        }
    }
    uint32_t component_size = 0;
    unsigned char *bytes = component_bytes(data, size, &entry, &component_size, error);
    if (!bytes) return BONGO_CAT_ERROR_FORMAT;
    if ((entry.flags & 1) && content_matches(path, bytes, component_size)) {
        free(bytes);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 内嵌渲染组件已就位(哈希一致, 无需释放): %s", path);
        return BONGO_CAT_OK;
    }
    if (!bongo_cat_path_create_directory(directory)) {
        free(bytes);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot create the renderer component directory: %s", directory);
        return BONGO_CAT_ERROR_IO;
    }
    bool written = write_file(temporary, bytes, component_size, error);
    free(bytes);
    if (written) written = bongo_cat_file_replace(temporary, path, true);
    if (!written) {
        bongo_cat_file_remove(temporary);
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_IO,
            "Cannot replace the renderer component at %s", path);
        return BONGO_CAT_ERROR_IO;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 已释放内嵌渲染组件到 %s (%u 字节)", path,
        (unsigned)component_size);
    return BONGO_CAT_OK;
}
#endif
