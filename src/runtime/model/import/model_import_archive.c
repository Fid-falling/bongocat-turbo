/* ZIP 导入的解压器，同时是这条路径上的安全边界：解压到 SDL_GetPrefPath 下的随机临时目录，
 * 只接受普通文件与目录，拒绝加密、链接、绝对路径、".."、Windows 保留名与超限归档。
 * 时序：由会话安装（session_install）与渐进导入（工作线程）调用，解压结果随后交给 discover。
 * 契约：输出 directory 是 temporary 的子目录；任何失败都会清掉临时树并把两个输出清空，
 * 因此调用方无论成败都可以直接调 archive_cleanup。最易改错：safe_path 的拒绝规则与大小累计
 * 上限 —— 放宽任何一条都会让归档能写到临时目录之外。 */
#include "model_import_archive.h"
#include "model_import_path.h"
#include "model_storage.h"
#include "bongo_cat/path.h"
#include <SDL3/SDL.h>
#include <miniz.h>
#include <stdio.h>
#include <string.h>

/* 拒绝阈值（不是截断阈值）：条目数与所有条目未压缩大小之和任一超限即整体失败，用来挡住
 * zip bomb 与超长解压；2 GiB 与 20000 也是本文件唯一的资源预算。 */
#define ARCHIVE_BYTES_LIMIT (UINT64_C(2) * 1024 * 1024 * 1024)
#define ARCHIVE_ENTRY_LIMIT 20000

/* 只看后缀（大小写不敏感）且排除同名目录：调用方靠它决定“解压还是直接读目录”。 */
bool bongo_cat_import_is_archive(const char *source) {
    return source && bongo_cat_import_has_suffix_ci(source, ".zip") &&
        !bongo_cat_path_is_dir(source);
}

/* miniz 的随机读回调：offset 来自归档自身的索引，超过 INT64_MAX 直接失败，否则 SDL_SeekIO
 * 截断后会在错误的位置继续读，解压出垃圾数据。 */
static size_t read_archive(void *userdata, mz_uint64 offset, void *buffer,
    size_t size) {
    SDL_IOStream *stream = userdata;
    if (offset > INT64_MAX || SDL_SeekIO(stream, (Sint64)offset,
        SDL_IO_SEEK_SET) < 0) return 0;
    return SDL_ReadIO(stream, buffer, size);
}

/* 写入回调的账本：remaining 卡住实际落盘字节数，让“解压器报告的未压缩大小”与真实写入一致
 * （畸形归档可能声明很少却吐出更多）；failed 把写失败带回调用方。 */
typedef struct ArchiveOutput {
    SDL_IOStream *stream;
    uint64_t remaining;
    bool failed;
} ArchiveOutput;

static size_t write_archive(void *userdata, mz_uint64 offset,
    const void *buffer, size_t size) {
    (void)offset;
    ArchiveOutput *output = userdata;
    if (size > output->remaining) return 0;
    size_t written = SDL_WriteIO(output->stream, buffer, size);
    output->remaining -= written;
    if (written != size) output->failed = true;
    return written;
}

/* Reject aliases and special Windows paths on every platform, so an archive
   has the same meaning wherever it is imported. Never materialize links. */
static bool safe_path(char *path) {
    for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
    /* 先全部换成 '/'，之后的前缀、深度与保留名判定都以 '/' 为准，'\' 写法绕不过检查。 */
    if (!path[0] || path[0] == '/') return false;
    unsigned depth = 0;
    for (char *part = path; *part;) {
        char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        /* 空段（连续的 '/' 或结尾的 '/'）、深度超过 24、段尾是 '.' 或空格都拒绝：后两者在
         * Windows 上会被静默截断，让落地文件名与归档记录对不上。 */
        if (!length || ++depth > 24 || part[length - 1] == '.' ||
            part[length - 1] == ' ') return false;
        for (size_t i = 0; i < length; ++i)
            if ((unsigned char)part[i] < 32 || strchr(":*?\"<>|", part[i]))
                return false;
        size_t stem = 0;
        while (stem < length && part[stem] != '.') ++stem;
        /* 与 model_import_identity 共用同一套保留设备名规则，避免解压出 CON、NUL、COM1 这类名字。 */
        if ((stem == 3 && (!SDL_strncasecmp(part, "CON", 3) ||
            !SDL_strncasecmp(part, "PRN", 3) ||
            !SDL_strncasecmp(part, "AUX", 3) ||
            !SDL_strncasecmp(part, "NUL", 3))) ||
            (stem == 4 && (!SDL_strncasecmp(part, "COM", 3) ||
            !SDL_strncasecmp(part, "LPT", 3)) &&
            part[3] >= '0' && part[3] <= '9')) return false;
        part = end ? end + 1 : part + length;
    }
    return true;
}

/* 逐条校验后立刻写入：在同一次遍历里检查条目数、名字安全、加密与特殊类型、累计大小，
 * 任何一条不合规就整体失败 —— 所以本函数自己不做回滚，清理整棵临时树是调用方的事
 * （失败时 extract 已经把输出清空了）。 */
static BongoCatResult extract_entries(mz_zip_archive *zip, const char *root) {
    mz_uint count = mz_zip_reader_get_num_files(zip);
    if (!count || count > ARCHIVE_ENTRY_LIMIT) return BONGO_CAT_ERROR_FORMAT;
    uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        char name[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
        if (!mz_zip_reader_file_stat(zip, i, &stat))
            return BONGO_CAT_ERROR_FORMAT;
        mz_uint length = mz_zip_reader_get_filename(zip, i, NULL, 0);
        if (!length || length > sizeof(name) ||
            mz_zip_reader_get_filename(zip, i, name, sizeof(name)) != length)
            return BONGO_CAT_ERROR_FORMAT;
        /* 从外部属性取 Unix 模式位：只放行普通文件（0100000）与目录（0040000），符号链接、
         * 设备等一律拒绝 —— 链接在解压时绝不物化。 */
        unsigned kind = (stat.m_external_attr >> 16) & 0170000;
        if (!length || length > sizeof(name) || strlen(name) + 1 != length ||
            !safe_path(name) || stat.m_is_encrypted || !stat.m_is_supported ||
            (kind && kind != 0100000 && kind != 0040000) ||
            stat.m_uncomp_size > ARCHIVE_BYTES_LIMIT - total ||
            !bongo_cat_path_join(target, sizeof(target), root, name))
            return BONGO_CAT_ERROR_FORMAT;
        total += stat.m_uncomp_size;
        if (stat.m_is_directory) {
            if (!bongo_cat_path_create_directory(target)) return BONGO_CAT_ERROR_IO;
            continue;
        }
        char parent[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_is_file(target) || bongo_cat_path_is_dir(target))
            return BONGO_CAT_ERROR_FORMAT;
        if (!bongo_cat_import_parent_path(target, parent, sizeof(parent)) ||
            !bongo_cat_path_create_directory(parent)) return BONGO_CAT_ERROR_IO;
        SDL_IOStream *output = SDL_IOFromFile(target, "wb");
        if (!output) return BONGO_CAT_ERROR_IO;
        ArchiveOutput destination = {output, stat.m_uncomp_size, false};
        bool extracted = mz_zip_reader_extract_to_callback(zip, i,
            write_archive, &destination, 0) != 0;
        bool closed = SDL_CloseIO(output);
        if (!closed || destination.failed) return BONGO_CAT_ERROR_IO;
        if (!extracted || destination.remaining) return BONGO_CAT_ERROR_FORMAT;
    }
    return BONGO_CAT_OK;
}

/* 删除整棵临时解压树；失败只记一条警告 —— 清理是尽力而为，不能盖住真正的导入错误。
 * temporary 为空时什么都不做，所以失败路径上重复调用是安全的。 */
void bongo_cat_import_archive_cleanup(const char *temporary) {
    if (temporary && temporary[0] &&
        !bongo_cat_model_remove_tree(temporary, NULL))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot remove model archive temporary directory: %s", temporary);
}

/* 解压入口：成功时 directory 是解压出的包目录（名字取自 zip 文件名去掉 .zip 后经安全校验，
 * 不合法就退回 "model"），temporary 是调用方稍后要清理的随机根；失败时两者都清空。 */
BongoCatResult bongo_cat_import_archive_extract(const char *source,
    char directory[BONGO_CAT_PATH_CAP], char temporary[BONGO_CAT_PATH_CAP],
    BongoCatError *error) {
    directory[0] = temporary[0] = '\0';
    SDL_IOStream *input = SDL_IOFromFile(source, "rb");
    if (!input) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot open ZIP: %s", source);
        return BONGO_CAT_ERROR_IO;
    }
    mz_zip_archive zip = {0};
    zip.m_pRead = read_archive;
    zip.m_pIO_opaque = input;
    Sint64 size = SDL_GetIOSize(input);
    BongoCatResult result = BONGO_CAT_ERROR_FORMAT;
    if (size > 0 && mz_zip_reader_init(&zip, (mz_uint64)size, 0)) {
        /* SDL_GetPrefPath 的返回值必须 SDL_free，所以从这里到函数末尾不能有提前 return，
         * 失败只能记在 result 上统一收尾。 */
        char *base = SDL_GetPrefPath("BongoCat", "archive-import");
        char token[80], name[BONGO_CAT_PATH_CAP];
        snprintf(token, sizeof(token), "%016llx-%016llx",
            (unsigned long long)SDL_GetPerformanceCounter(),
            (unsigned long long)SDL_GetTicksNS());
        /* 临时目录名用“性能计数器 + 纳秒”拼出来并确认不存在，撞名时直接失败而不复用，
         * 以免把上一次导入的残留当成本次结果。 */
        int written = snprintf(name, sizeof(name), "%s", bongo_cat_path_name(source));
        result = BONGO_CAT_ERROR_IO;
        if (base && written >= 4 && (size_t)written < sizeof(name)) {
            name[written - 4] = '\0';
            if (!safe_path(name)) snprintf(name, sizeof(name), "model");
            char candidate[BONGO_CAT_PATH_CAP];
            if (bongo_cat_path_join(candidate, sizeof(candidate), base, token) &&
                !bongo_cat_path_is_dir(candidate) &&
                !bongo_cat_path_is_file(candidate) &&
                bongo_cat_path_create_directory(candidate)) {
                snprintf(temporary, BONGO_CAT_PATH_CAP, "%s", candidate);
                if (bongo_cat_path_join(directory, BONGO_CAT_PATH_CAP, temporary, name) &&
                    bongo_cat_path_create_directory(directory))
                    result = extract_entries(&zip, directory);
            }
        }
        SDL_free(base);
        mz_zip_reader_end(&zip);
    }
    SDL_CloseIO(input);
    if (result != BONGO_CAT_OK) {
        bongo_cat_import_archive_cleanup(temporary);
        directory[0] = temporary[0] = '\0';
        bongo_cat_error_set(error, result,
            "Cannot extract ZIP (damaged, encrypted, unsafe, too large, or inaccessible): %s",
            source);
    }
    return result;
}
