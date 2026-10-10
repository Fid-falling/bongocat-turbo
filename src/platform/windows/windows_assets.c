#include "bongo_cat/file.h"
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
 */
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

/* 标记摘要没变时走的快速路径: 只检查包内每个文件在磁盘上存在且大小一致, 关键文件再
   比对内容。任何一项不满足就返回 false, 触发完整解包。 */
static bool packed_files_present(const unsigned char *data, size_t size,
    const char *target) {
    if (size < 12) return false;
    uint32_t count = read32(data + 8); size_t offset = 12;
    if (count > 4096) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (offset + 16 > size) return false;
        uint16_t name_size = read16(data + offset);
        uint32_t file_size = read32(data + offset + 4);
        uint64_t file_offset = read64(data + offset + 8);
        offset += 16;
        char name[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
        if (!name_size || name_size >= sizeof(name) || offset + name_size > size ||
            file_offset > size || file_size > size - (size_t)file_offset) return false;
        memcpy(name, data + offset, name_size); name[name_size] = '\0'; offset += name_size;
        uint64_t actual_size;
        if (!safe_name(name) || !bongo_cat_path_join(path, sizeof(path), target, name) ||
            !bongo_cat_path_file_size(path, &actual_size) ||
            actual_size != file_size) return false;
        if (verify_contents(name) && !content_matches(path,
            data + (size_t)file_offset, file_size)) return false;
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
 * 真正解包。每一步都要检查边界(条目表长度、路径长度、负载偏移加长度是否越界): 输入
 * 虽然来自自己的 exe, 但打包流程改动或文件被替换都可能让它前后不一致。
 * 负载有两种形态: flags & 1 为压缩(内容是 [uint32 原始大小][deflate 数据]), 否则就是
 * 原文 —— 与 asset_packer.c 的 build_payload 一一对应。
 * 解压出来的长度必须严格等于原始大小, 否则视为损坏, 免得写出被截断的资源。
 */
static bool extract_pack(const unsigned char *data, size_t size, const char *target,
    BongoCatError *error) {
    const unsigned char magic[8] = {'L', '2', 'D', 'P', 'A', 'K', '1', 0};
    if (size < 12 || memcmp(data, magic, sizeof(magic)) != 0) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT, "Invalid embedded asset pack");
        return false;
    }
    uint32_t count = read32(data + 8);
    size_t offset = 12;
    if (count > 4096) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (offset + 16 > size) return false;
        uint16_t name_size = read16(data + offset);
        /* 条目头第二个 uint16 以前恒为 0 (reserved), 现在当压缩标志用 */
        uint16_t flags = read16(data + offset + 2);
        uint32_t file_size = read32(data + offset + 4);
        uint64_t file_offset = read64(data + offset + 8);
        offset += 16;
        char name[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
        if (!name_size || name_size >= sizeof(name) || offset + name_size > size ||
            file_offset > size || file_size > size - (size_t)file_offset) return false;
        memcpy(name, data + offset, name_size); name[name_size] = '\0';
        offset += name_size;
        if (!safe_name(name) ||
            !bongo_cat_path_join(path, sizeof(path), target, name)) {
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
        if (flags & 1) {
            if (file_size < 4) return false;
            uint32_t raw_size = read32(data + (size_t)file_offset);
            unsigned char *buffer = malloc(raw_size ? raw_size : 1);
            if (!buffer) {
                bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
                    "Cannot allocate decompressed asset");
                return false;
            }
            mz_ulong length = raw_size;
            int status = raw_size ?
                mz_uncompress(buffer, &length, data + (size_t)file_offset + 4,
                    file_size - 4) : MZ_OK;
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
        if (!write_file(path, data + (size_t)file_offset, file_size, error)) return false;
    }
    return true;
}

/* 入口: 找到 PE 资源(RCDATA 101) -> 算出它的 SHA-256 作为摘要 -> 缓存目录里的 marker
   摘要一致就直接返回 -> 否则完整解包并写 marker(64 字节的十六进制摘要, 不含结尾 NUL)。
   打包器必须产出确定性结果, 否则每次构建摘要都不同, 缓存会反复重建。 */
BongoCatResult bongo_cat_platform_embedded_assets(const char *target, BongoCatError *error) {
    if (!target) return BONGO_CAT_ERROR_ARGUMENT;
    char marker[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(marker, sizeof(marker), target, "complete");
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (!resource) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Embedded application assets are unavailable");
        return BONGO_CAT_ERROR_IO;
    }
    HGLOBAL loaded = LoadResource(NULL, resource);
    const unsigned char *data = loaded ? LockResource(loaded) : NULL;
    DWORD size = loaded ? SizeofResource(NULL, resource) : 0;
    char digest[65];
    if (data && size) bongo_cat_sha256_bytes(data, size, digest);
    if (data && size && current_pack(marker, target, data, size, digest))
        return BONGO_CAT_OK;
    SDL_Log("Embedded asset cache is incomplete; rebuilding it");
    if (!data || !size || !bongo_cat_path_create_directory(target) ||
        !extract_pack(data, size, target, error)) {
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_IO, "Cannot rebuild the embedded asset cache: %s",
            SDL_GetError());
        return BONGO_CAT_ERROR_IO;
    }
    return write_file(marker, (const unsigned char *)digest, 64, error)
        ? BONGO_CAT_OK : BONGO_CAT_ERROR_IO;
}
#endif
