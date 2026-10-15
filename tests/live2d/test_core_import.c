/*
 * Cubism Core 导入链的"坏输入"矩阵。
 *
 * 这个用例只打压缩包侧(live2d_core_archive.c): 那是唯一处理不可信输入的地方。每条断言都对应
 * 一条具体的攻击面, 而不是"跑一遍看看没崩":
 *   1. zip-slip(../)、绝对路径(/...)、盘符(C:/...)、空路径分量(a//b);
 *   2. 符号链接: zip 中央目录里的外部属性是 0xA000 —— 命中必须**拒绝整包**(绝不能物化链接);
 *   3. 白名单: SDK 里那上万个非 Core 条目一个都不落盘; 平台/架构不匹配要给出"包里有什么";
 *   4. 超长条目名: miniz 的文件名缓冲是 260 字节, 超过就拒绝整包(不能拿截断后的名字做判断);
 *   5. 成功路径: 只有唯一匹配本机平台/架构的那一个条目被解出来, 内容逐字节一致, 并且
 *      SDK 根目录名被回报出来(支持排查用)。
 *
 * 这里不测"是不是真的能加载": 那需要一份真实的 Core 二进制(专有, 不进仓库)。伪 Core 会在
 * install_core 的架构/三符号校验处失败 —— 用例里也断言了"裸文件路径对垃圾输入必须失败",
 * 保证校验不是摆设。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/path.h"

#include "live2d_core_internal.h"

#include <SDL3/SDL.h>
#include <miniz.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static char workspace[BONGO_CAT_PATH_CAP];

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, message); \
        ++failures; \
    } \
} while (0)

static bool join(char *output, size_t capacity, const char *name) {
    return bongo_cat_path_join(output, capacity, workspace, name);
}

/* 建一个只有名字与内容的 zip。用来构造"攻击者能控制的输入"。 */
static bool write_zip(const char *path, const char *const *names,
    const char *const *payloads, size_t count) {
    mz_zip_archive zip = {0};
    if (!mz_zip_writer_init_file(&zip, path, 0)) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < count; ++i) {
        ok = mz_zip_writer_add_mem(&zip, names[i], payloads[i],
            strlen(payloads[i]), MZ_BEST_SPEED) != 0;
    }
    if (mz_zip_writer_finalize_archive(&zip) == 0) ok = false;
    if (mz_zip_writer_end(&zip) == 0) ok = false;
    return ok;
}

/*
 * 把中央目录里第一个条目的"外部文件属性"改写掉。
 * miniz 的写入 API 不暴露这个字段, 而符号链接判定恰恰只看它 —— 所以这里手工改:
 * 中央目录头签名是 PK\1\2, 其偏移 38 起的 4 字节就是 external attributes(小端)。
 * 这是唯一一处直接碰二进制布局的地方, 因此在这里写清楚依据。
 */
static bool patch_first_external_attr(const char *path, uint32_t attributes) {
    FILE *file = fopen(path, "r+b");
    if (!file) return false;
    unsigned char signature[4] = {0};
    bool patched = false;
    while (!patched && fread(signature, 1, 4, file) == 4) {
        if (signature[0] == 'P' && signature[1] == 'K' &&
            signature[2] == 1 && signature[3] == 2) {
            unsigned char value[4] = {(unsigned char)(attributes & 0xff),
                (unsigned char)((attributes >> 8) & 0xff),
                (unsigned char)((attributes >> 16) & 0xff),
                (unsigned char)((attributes >> 24) & 0xff)};
            patched = fseek(file, 38, SEEK_CUR) == 0 &&
                fwrite(value, 1, 4, file) == 4;
            break;
        }
        /* 没命中就往前挪 1 字节继续找(中央目录头不会跨在签名匹配位置上)。 */
        if (fseek(file, -3, SEEK_CUR) != 0) break;
    }
    fclose(file);
    return patched;
}

/* 调一次解压; staged 是目标文件, hint 收 SDK 根目录名。 */
static BongoCatResult extract(const char *archive, const char *staged,
    char *hint, size_t hint_capacity, BongoCatError *error) {
    return bongo_cat_live2d_core_extract_archive(archive, staged, hint,
        hint_capacity, error);
}

static const char *host_platform(void) {
    return bongo_cat_live2d_core_platform_name();
}

static const char *host_arch(void) {
    return bongo_cat_live2d_core_arch_name();
}

/* 拼一个 "CubismSdkForNative-5-r.5/Core/dll/<平台>/<架构>/<库名>" 形式的条目名。 */
static void core_entry_name(char *output, size_t capacity, const char *platform,
    const char *arch) {
    snprintf(output, capacity,
        "CubismSdkForNative-5-r.5/Core/dll/%s/%s/%s", platform, arch,
        bongo_cat_live2d_core_library_name());
}

static void test_whitelist_and_success(void) {
    char archive[BONGO_CAT_PATH_CAP], staged[BONGO_CAT_PATH_CAP];
    join(archive, sizeof(archive), "whitelist.zip");
    join(staged, sizeof(staged), "whitelist-out.bin");
    char core_name[256], header_name[256];
    core_entry_name(core_name, sizeof(core_name), host_platform(), host_arch());
    snprintf(header_name, sizeof(header_name),
        "CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h");
    const char *names[] = {header_name, core_name};
    const char *payloads[] = {"/* header */", "MZfake-core-bytes"};
    CHECK(write_zip(archive, names, payloads, 2), "cannot build the fixture zip");
    char hint[128] = {0};
    BongoCatError error = {0};
    CHECK(extract(archive, staged, hint, sizeof(hint), &error) == BONGO_CAT_OK,
        "a whitelisted Core entry must extract");
    /* 内容必须逐字节一致: 只取白名单那一条, 头文件不落盘。 */
    uint64_t size = 0, modified = 0;
    CHECK(bongo_cat_path_file_info(staged, &size, &modified) &&
        size == strlen(payloads[1]), "staged size must match the Core payload");
    FILE *file = fopen(staged, "rb");
    char buffer[64] = {0};
    size_t read = file ? fread(buffer, 1, sizeof(buffer) - 1, file) : 0;
    if (file) fclose(file);
    CHECK(read == strlen(payloads[1]) && !strcmp(buffer, payloads[1]),
        "staged content must be the Core payload");
    CHECK(!strcmp(hint, "CubismSdkForNative-5-r.5"),
        "the SDK root name must be reported for support");
    char leftover[BONGO_CAT_PATH_CAP];
    join(leftover, sizeof(leftover), "CubismSdkForNative-5-r.5");
    CHECK(!bongo_cat_path_is_dir(leftover),
        "no other archive entry may be materialized on disk");
}

static void test_platform_mismatch(void) {
    char archive[BONGO_CAT_PATH_CAP], staged[BONGO_CAT_PATH_CAP];
    join(archive, sizeof(archive), "mismatch.zip");
    join(staged, sizeof(staged), "mismatch-out.bin");
    /* 故意放一个**别的**平台/架构: 报错必须说清包里到底有什么。 */
    const char *other_platform = strcmp(host_platform(), "linux") ? "linux" : "macos";
    char core_name[256];
    core_entry_name(core_name, sizeof(core_name), other_platform, "x86_64");
    const char *names[] = {core_name};
    const char *payloads[] = {"MZfake-core-bytes"};
    CHECK(write_zip(archive, names, payloads, 1), "cannot build the fixture zip");
    BongoCatError error = {0};
    CHECK(extract(archive, staged, NULL, 0, &error) == BONGO_CAT_ERROR_FORMAT,
        "a mismatched platform must fail");
    CHECK(strstr(error.message, other_platform) != NULL,
        "the error must list what the archive actually contains");
    CHECK(!bongo_cat_path_is_file(staged),
        "nothing may be staged when the platform does not match");
}

/* 逐条坏路径: 每条都必须让**整包**失败, 而不是"跳过这一条继续"。 */
static void test_unsafe_entry_names(void) {
    const char *bad_names[] = {
        "../escape/Core/dll/x/y/Live2DCubismCore.dll",
        "/absolute/Core/dll/x/y/Live2DCubismCore.dll",
        "C:/drive/Core/dll/x/y/Live2DCubismCore.dll",
        "sdk//double/Core/dll/x/y/Live2DCubismCore.dll",
        "./relative/Core/dll/x/y/Live2DCubismCore.dll"
    };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); ++i) {
        char archive[BONGO_CAT_PATH_CAP], staged[BONGO_CAT_PATH_CAP];
        char name[32];
        snprintf(name, sizeof(name), "unsafe-%zu.zip", i);
        join(archive, sizeof(archive), name);
        join(staged, sizeof(staged), "unsafe-out.bin");
        const char *names[] = {bad_names[i]};
        const char *payloads[] = {"MZfake-core-bytes"};
        if (!write_zip(archive, names, payloads, 1)) {
            /* miniz 的写入器自己就拒绝这种名字(例如空路径分量)。那也算"不能被利用",
               记一条提示继续, 不当作失败 —— 我们要断言的是**读**的时候必须拒绝,
               而写入器拒绝只会让这条用例无从构造。 */
            printf("note: the zip writer refused to create %s\n", bad_names[i]);
            continue;
        }
        BongoCatError error = {0};
        CHECK(extract(archive, staged, NULL, 0, &error) == BONGO_CAT_ERROR_FORMAT,
            "an archive with an unsafe path must fail as a whole");
        CHECK(!bongo_cat_path_is_file(staged),
            "an unsafe archive must not produce a staged file");
    }
}

static void test_symlink_entry_is_refused(void) {
    char archive[BONGO_CAT_PATH_CAP], staged[BONGO_CAT_PATH_CAP];
    join(archive, sizeof(archive), "symlink.zip");
    join(staged, sizeof(staged), "symlink-out.bin");
    char core_name[256];
    core_entry_name(core_name, sizeof(core_name), host_platform(), host_arch());
    const char *names[] = {core_name};
    const char *payloads[] = {"MZfake-core-bytes"};
    CHECK(write_zip(archive, names, payloads, 1), "cannot build the fixture zip");
    /* Unix 模式位高 16 位 = 0xA000(S_IFLNK) —— 正是"这条是符号链接"的判据。 */
    CHECK(patch_first_external_attr(archive, 0xA000u << 16),
        "cannot patch the fixture's external attributes");
    BongoCatError error = {0};
    CHECK(extract(archive, staged, NULL, 0, &error) == BONGO_CAT_ERROR_FORMAT,
        "a symlink entry must refuse the whole archive");
    CHECK(strstr(error.message, "link") != NULL,
        "the error must say a link was found");
    CHECK(!bongo_cat_path_is_file(staged), "a symlink must never be materialized");
}

static void test_overlong_entry_name(void) {
    char archive[BONGO_CAT_PATH_CAP], staged[BONGO_CAT_PATH_CAP];
    join(archive, sizeof(archive), "long-name.zip");
    join(staged, sizeof(staged), "long-out.bin");
    /* 前缀 600 个 'a' + 合法尾部: 名字超长必须拒绝整包, 因为 miniz 的文件名缓冲只有 260
       字节, 截断后的名字不足以支撑"这一条是不是我们要的"的判断。 */
    char name[800];
    memset(name, 'a', 600);
    snprintf(name + 600, sizeof(name) - 600, "/Core/dll/%s/%s/%s", host_platform(),
        host_arch(), bongo_cat_live2d_core_library_name());
    const char *names[] = {name};
    const char *payloads[] = {"MZfake-core-bytes"};
    CHECK(write_zip(archive, names, payloads, 1), "cannot build the fixture zip");
    BongoCatError error = {0};
    CHECK(extract(archive, staged, NULL, 0, &error) == BONGO_CAT_ERROR_FORMAT,
        "an over-long entry name must fail the archive");
}

/* 裸文件路径: 垃圾输入必须在"校验"这一步被拦住(架构 magic + 三符号探测)。 */
static void test_plain_file_rejects_garbage(void) {
    char source[BONGO_CAT_PATH_CAP];
    join(source, sizeof(source), "not-a-core.dll");
    FILE *file = fopen(source, "wb");
    CHECK(file != NULL, "cannot create the garbage fixture");
    if (file) {
        fwrite("this is not a library", 1, 21, file);
        fclose(file);
    }
    BongoCatLive2DCoreImport report = {0};
    BongoCatError error = {0};
    BongoCatResult result = bongo_cat_live2d_core_import(source, &report, &error);
    CHECK(result == BONGO_CAT_ERROR_CUBISM, "garbage must be rejected as Cubism");
    CHECK(error.message[0] != '\0', "a rejection must explain itself");
}

int main(void) {
    const char *base = SDL_getenv("TEMP") ? SDL_getenv("TEMP") : SDL_getenv("TMPDIR");
    if (!base || !base[0]) base = ".";
    char token[64];
    snprintf(token, sizeof(token), "bongo-live2d-core-test-%llu",
        (unsigned long long)SDL_GetTicksNS());
    if (!bongo_cat_path_join(workspace, sizeof(workspace), base, token)) return 1;
    if (!bongo_cat_path_create_directory(workspace)) return 1;
    test_whitelist_and_success();
    test_platform_mismatch();
    test_unsafe_entry_names();
    test_symlink_entry_is_refused();
    test_overlong_entry_name();
    test_plain_file_rejects_garbage();
    bongo_cat_path_remove(workspace);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("live2d core import attack-surface matrix passed");
    return 0;
}
