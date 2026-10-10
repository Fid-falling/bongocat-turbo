/* 候选包与已安装包的“身份与命名”工具：能力位声明、显示名净化、package 与 variant id 生成
 * （含 Windows 非法字符、保留设备名的处理）、把 discovery 原地压缩成 metadata，以及给
 * nearby 条目填字段。位置在 discover 之后、写入存储之前。
 * 契约：生成的 id 要能直接当目录名（合法文件名、非保留设备名、UTF-8 合法），变体用 "~N"
 * 后缀且必须能被 package_base_id 反解 —— 已安装包索引（用包目录名）与 nearby（自拼
 * "nearby-<hash>-<mode>"）各自生成 id，但都依赖这里的净化与后缀规则。
 * display 与 source 名是数据不是界面文案，兜底用固定英文串，不进翻译表。 */
#include "model_import.h"
#include "model_import_path.h"
#include "model_import_mver_policy.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"
#include "bongo_cat/utf8.h"

#include <stdio.h>
#include <string.h>

#define IMPORT_COLLISION_SUFFIX_RESERVE 5

/* 能力位是“声明”而非探测：UI 据此决定露出哪些设置页，适配器据此决定写出哪些资源；四个
 * 基础位对所有格式成立，Tauri 提前返回，由归一化成 Mver 的那条路径在转换后补齐缺失的位。 */
uint32_t bongo_cat_import_candidate_capabilities(
    const BongoCatImportCandidate *candidate) {
    uint32_t result = BONGO_CAT_MODEL_CAPABILITY_LIVE2D |
        BONGO_CAT_MODEL_CAPABILITY_PREVIEW |
        BONGO_CAT_MODEL_CAPABILITY_RUNTIME_ADAPTER |
        BONGO_CAT_MODEL_CAPABILITY_BEHAVIORS;
    if (candidate->format == BONGO_CAT_IMPORT_TAURI) return result;
    result |= BONGO_CAT_MODEL_CAPABILITY_INPUT_IMAGES |
        BONGO_CAT_MODEL_CAPABILITY_KEYBOARD_INPUT |
        BONGO_CAT_MODEL_CAPABILITY_AUDIO |
        BONGO_CAT_MODEL_CAPABILITY_EFFECTS |
        BONGO_CAT_MODEL_CAPABILITY_MVER_PROJECTION;
    if (candidate->mode == BONGO_CAT_MODE_STANDARD)
        result |= BONGO_CAT_MODEL_CAPABILITY_POINTER_OVERLAY;
    if (candidate->mode == BONGO_CAT_MODE_GAMEPAD && candidate->gamepad_buttons)
        result |= BONGO_CAT_MODEL_CAPABILITY_GAMEPAD_INPUT;
    if (candidate->format == BONGO_CAT_IMPORT_MVER_PATCH)
        result |= BONGO_CAT_MODEL_CAPABILITY_IMAGE_PATCH;
    return result;
}

/* 身份与展示的来源根：patch 包用 patch_root（用户改的是覆盖层），没有时退回 package_root。 */
static const char *source_root(const BongoCatImportCandidate *candidate) {
    return candidate->format == BONGO_CAT_IMPORT_MVER_PATCH &&
        candidate->patch_root[0] ? candidate->patch_root : candidate->package_root;
}

/* 展示名不暴露 "xxx.patch" 这种目录名：当 patch_root 的目录名与 package_root 同名时返回它
 * 的父目录，让用户看到基础包的名字；parent 由调用方提供缓冲区。 */
static const char *display_source(const BongoCatImportCandidate *candidate,
    char parent[BONGO_CAT_PATH_CAP]) {
    const char *source = source_root(candidate);
    if (candidate->format == BONGO_CAT_IMPORT_MVER_PATCH &&
        bongo_cat_import_parent_path(source, parent, BONGO_CAT_PATH_CAP) &&
        !strcmp(bongo_cat_path_name(source),
            bongo_cat_path_name(candidate->package_root))) return parent;
    return source;
}

/* 按字节截断后回退到合法 UTF-8 边界：maximum 是允许的最大字节数（不含结尾 NUL）。
 * 结果会写进文件名与配置，留下半个多字节字符会让后续扫描读到不同的 id。 */
static void truncate_utf8(char *value, size_t maximum) {
    size_t length = value ? strlen(value) : 0;
    if (length > maximum) value[maximum] = '\0';
    length = value ? strlen(value) : 0;
    while (length && !bongo_cat_utf8_valid(value)) value[--length] = '\0';
}

/* 去掉结尾的空格与句点：Windows 建目录时会静默丢掉它们，不先自己处理就会让“磁盘上的名字”
 * 与“内存里的 id”不一致，下次扫描得到另一个 id。 */
static void trim_id_end(char *value) {
    size_t length = value ? strlen(value) : 0;
    while (length && (value[length - 1] == ' ' || value[length - 1] == '.'))
        value[--length] = '\0';
}

static bool ascii_equal(const char *value, size_t length, const char *expected) {
    if (strlen(expected) != length) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char left = (unsigned char)value[i];
        unsigned char right = (unsigned char)expected[i];
        if (left >= 'a' && left <= 'z') left = (unsigned char)(left - 'a' + 'A');
        if (right >= 'a' && right <= 'z') right = (unsigned char)(right - 'a' + 'A');
        if (left != right) return false;
    }
    return true;
}

/* Windows 保留设备名：只看扩展名之前的 stem 并去掉尾部空格；CON、PRN、AUX、NUL 与
 * COM1..COM9、LPT1..LPT9（注意 COM0 不算）命中就必须改名，否则建目录会落到设备上。 */
static bool reserved_device_name(const char *value) {
    size_t length = value ? strcspn(value, ".") : 0;
    while (length && value[length - 1] == ' ') length--;
    if (ascii_equal(value, length, "CON") || ascii_equal(value, length, "PRN") ||
        ascii_equal(value, length, "AUX") || ascii_equal(value, length, "NUL"))
        return true;
    return length == 4 && value[3] >= '1' && value[3] <= '9' &&
        (ascii_equal(value, 3, "COM") || ascii_equal(value, 3, "LPT"));
}

/* 显示名只用于 UI：保证 NUL 结尾、UTF-8 合法，并把控制字符换成空格；不做路径净化。 */
static void copy_display_text(char *output, size_t capacity, const char *value) {
    if (!output || !capacity) return;
    output[0] = '\0';
    if (!value) return;
    snprintf(output, capacity, "%s", value);
    output[capacity - 1] = '\0';
    truncate_utf8(output, capacity - 1);
    for (unsigned char *cursor = (unsigned char *)output; *cursor; ++cursor)
        if (*cursor < 0x20 || *cursor == 0x7f) *cursor = ' ';
}

/* id 中禁止的字节：控制字符、Windows 非法字符，以及 '~'（留给变体后缀，否则 base_id 反解有歧义）。 */
static bool forbidden_id_byte(unsigned char value) {
    return value < 0x20 || value == 0x7f || value == '/' || value == '\\' ||
        value == ':' || value == '*' || value == '?' || value == '"' ||
        value == '<' || value == '>' || value == '|' || value == '~';
}

/* 净化规则：只替换 ASCII 字节，多字节 UTF-8 原样保留；首字符 '.' 换成 '-' 以防隐藏文件；
 * 结尾空格与句点去掉；命中保留设备名时先截短一位再前置 '_'（保持可读性又避开冲突）。 */
static void normalize_id_base(char *output, size_t capacity,
    const char *value) {
    if (!output || !capacity) return;
    output[0] = '\0';
    if (!value) return;
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)value;
        *cursor && used + 1 < capacity; ++cursor) {
        unsigned char byte = *cursor;
        if (byte < 0x80 && (forbidden_id_byte(byte) || (!used && byte == '.')))
            byte = '-';
        output[used++] = (char)byte;
    }
    output[used] = '\0';
    truncate_utf8(output, capacity - 1);
    trim_id_end(output);
    if (reserved_device_name(output)) {
        truncate_utf8(output, capacity > 1 ? capacity - 2 : 0);
        trim_id_end(output);
        size_t length = strlen(output);
        if (length + 1 < capacity) {
            memmove(output + 1, output, length + 1);
            output[0] = '_';
        }
    }
}

/* 由用户可见的名字生成 package id：预留 IMPORT_COLLISION_SUFFIX_RESERVE 字节给 "~序号"
 * 后缀，保证同一个包的 16 个变体都不会把后缀挤掉；名字被净化成空时用固定英文兜底，
 * 返回 false 只发生在容量不足时。 */
bool bongo_cat_import_package_id(char *output, size_t capacity,
    const char *name) {
    if (!output || capacity < 2) return false;
    char normalized[BONGO_CAT_ID_CAP];
    normalize_id_base(normalized, sizeof(normalized), name);
    if (!normalized[0]) snprintf(normalized, sizeof(normalized), "Imported model");
    size_t maximum = capacity > IMPORT_COLLISION_SUFFIX_RESERVE + 1
        ? capacity - IMPORT_COLLISION_SUFFIX_RESERVE - 1 : 0;
    truncate_utf8(normalized, maximum);
    trim_id_end(normalized);
    if (!normalized[0]) snprintf(normalized, sizeof(normalized), "model");
    int written = snprintf(output, capacity, "%s", normalized);
    return written >= 0 && (size_t)written < capacity;
}

/* variant_index 为 0 时不加后缀（第一个变体保持干净的名字，也是 id 稳定性的来源），其余用
 * "~N"（N = index + 1）；base 要按后缀长度重新截断，后缀本身必须完整。 */
bool bongo_cat_import_variant_id(char *output, size_t capacity,
    const char *package_id, size_t variant_index) {
    if (!output || !capacity || !package_id || !package_id[0]) return false;
    if (!variant_index) {
        int written = snprintf(output, capacity, "%s", package_id);
        return written >= 0 && (size_t)written < capacity;
    }
    char suffix[32];
    int suffix_length = snprintf(suffix, sizeof(suffix), "~%zu",
        variant_index + 1);
    if (suffix_length < 0 || (size_t)suffix_length >= sizeof(suffix) ||
        capacity <= (size_t)suffix_length + 1) return false;
    char base[BONGO_CAT_ID_CAP];
    snprintf(base, sizeof(base), "%s", package_id);
    truncate_utf8(base, capacity - (size_t)suffix_length - 1);
    trim_id_end(base);
    int written = snprintf(output, capacity, "%s%s", base, suffix);
    return written >= 0 && (size_t)written < capacity;
}

/* 变体 id 的反解：只有 '~' 之后全是数字且非空才剥掉，其它形式原样返回（名字里本来就带 '~'
 * 的旧 id 不受影响）；导入进度靠它把变体折回包名，所以这个后缀格式是跨模块契约。 */
bool bongo_cat_import_package_base_id(char *output, size_t capacity,
    const char *model_id) {
    if (!output || capacity < 2 || !model_id || !model_id[0]) return false;
    int written = snprintf(output, capacity, "%s", model_id);
    if (written < 0 || (size_t)written >= capacity) return false;
    char *separator = strrchr(output, '~');
    if (!separator || !separator[1]) return true;
    for (char *cursor = separator + 1; *cursor; ++cursor)
        if (*cursor < '0' || *cursor > '9') return true;
    *separator = '\0';
    return output[0] != '\0';
}

/* 主流程：丢掉有完整基础包的 patch 候选 -> 逐个算内容指纹 -> 丢掉随包占位模板 -> 按
 * （mode，digest）去重 -> 把留下的候选原地压缩到 discovery 前部，并按顺序拼出 family 素材。
 * 关键不变量：循环中 discovery->candidates[0..output) 与 metadata[0..output) 一一对应
 * （第 output 个槽位是当前候选），去重循环正是靠这一点直接用 discovery 里的候选配 metadata；
 * 循环结束后 discovery->count 被改成 output，调用方不能再按原数量遍历。
 * metadata 由调用方分配；family_id 只在多变体时才填；display 与 source 名优先用
 * discovery->source_name（用户选中的那个），否则退回包目录名。 */
bool bongo_cat_import_prepare_package_metadata_cached(
    BongoCatImportDiscovery *discovery,
    BongoCatPackageMetadata *metadata, BongoCatImportDigestCache *cache,
    BongoCatError *error) {
    char family_material[BONGO_CAT_IMPORT_CANDIDATE_CAP * 66 + 32];
    size_t used = 0, output = 0;
    for (size_t i = 0; i < discovery->count; ++i) {
        BongoCatImportCandidate candidate = discovery->candidates[i];
        if (bongo_cat_import_patch_has_full_base(discovery, i)) continue;
        BongoCatPackageMetadata *item = &metadata[output];
        bool placeholder = false;
        if (!bongo_cat_import_candidate_inspect_cached(&candidate,
            item->content_digest, &placeholder, cache, error)) return false;
        if (placeholder) continue;
        bool duplicate = false;
        for (size_t j = 0; j < output; ++j)
            if (discovery->candidates[j].mode == candidate.mode &&
                strcmp(metadata[j].content_digest, item->content_digest) == 0) {
                duplicate = true;
                break;
            }
        if (duplicate) continue;
        discovery->candidates[output] = candidate;
        int written = snprintf(family_material + used,
            sizeof(family_material) - used, "%s|", item->content_digest);
        if (written < 0 || (size_t)written >= sizeof(family_material) - used)
            return false;
        used += (size_t)written;
        char parent[BONGO_CAT_PATH_CAP];
        const char *name = discovery->source_name[0] ? discovery->source_name :
            bongo_cat_path_name(display_source(&candidate, parent));
        copy_display_text(item->source_name, sizeof(item->source_name),
            name && name[0] ? name : "Imported model");
        if (!item->source_name[0]) copy_display_text(item->source_name,
            sizeof(item->source_name), "Imported model");
        copy_display_text(item->display_name, sizeof(item->display_name),
            item->source_name);
        item->capabilities = bongo_cat_import_candidate_capabilities(&candidate);
        /* Tauri candidates are normalized to Mver before they are stored. */
        if (candidate.format == BONGO_CAT_IMPORT_TAURI)
            item->capabilities |= BONGO_CAT_MODEL_CAPABILITY_INPUT_IMAGES |
                BONGO_CAT_MODEL_CAPABILITY_KEYBOARD_INPUT |
                BONGO_CAT_MODEL_CAPABILITY_AUDIO |
                BONGO_CAT_MODEL_CAPABILITY_EFFECTS |
                BONGO_CAT_MODEL_CAPABILITY_MVER_PROJECTION |
                (candidate.mode == BONGO_CAT_MODE_GAMEPAD
                    ? BONGO_CAT_MODEL_CAPABILITY_GAMEPAD_INPUT : 0);
        output++;
    }
    discovery->count = output;
    if (!output) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Selected model package contains only bundled placeholder modes");
        return false;
    }
    for (size_t i = 0; i < output; ++i) {
        char package_id[BONGO_CAT_ID_CAP];
        if (!bongo_cat_import_package_id(package_id, sizeof(package_id),
                metadata[i].source_name) ||
            !bongo_cat_import_variant_id(metadata[i].package_id,
                sizeof(metadata[i].package_id), package_id, i)) return false;
    }
    if (output > 1) {
        char digest[65];
        bongo_cat_sha256_bytes(family_material, used, digest);
        for (size_t i = 0; i < output; ++i)
            snprintf(metadata[i].family_id, sizeof(metadata[i].family_id),
                "family-%s", digest);
    }
    return true;
}

/* 无缓存包装（cache = NULL）：面向一次性调用，安装会话请传 session->digests 复用哈希。 */
bool bongo_cat_import_prepare_package_metadata(
    BongoCatImportDiscovery *discovery,
    BongoCatPackageMetadata *metadata, BongoCatError *error) {
    return bongo_cat_import_prepare_package_metadata_cached(discovery,
        metadata, NULL, error);
}

/* 填充内存中的 catalog 条目：只写字段，不碰磁盘。调用方必须先 memset 并自行设置
 * display_name（本函数不写它）；content_digest 直接用调用方算好的 identity，family_id 由
 * source_hash 前 16 位派生，managed = true 表示该条目由应用管理（可删除、可刷新）。 */
void bongo_cat_import_describe_nearby_entry(BongoCatModelEntry *entry,
    const BongoCatImportCandidate *candidate, const char *id,
    const char *identity, const char *source_hash, const char *source,
    const char *adapter) {
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->package_id, sizeof(entry->package_id), "%s", id);
    snprintf(entry->content_digest, sizeof(entry->content_digest), "%s", identity);
    snprintf(entry->family_id, sizeof(entry->family_id), "family-nearby-%.16s",
        source_hash);
    snprintf(entry->directory, sizeof(entry->directory), "%s", candidate->directory);
    snprintf(entry->adapter_directory, sizeof(entry->adapter_directory), "%s", adapter);
    snprintf(entry->storage_directory, sizeof(entry->storage_directory), "%s", source);
    snprintf(entry->setting_file, sizeof(entry->setting_file), "%s", candidate->setting);
    entry->mode = candidate->mode;
    entry->source_format = candidate->format == BONGO_CAT_IMPORT_TAURI
        ? BONGO_CAT_MODEL_SOURCE_TAURI :
        candidate->format == BONGO_CAT_IMPORT_MVER_PATCH
        ? BONGO_CAT_MODEL_SOURCE_MVER_PATCH : BONGO_CAT_MODEL_SOURCE_MVER;
    entry->capabilities = bongo_cat_import_candidate_capabilities(candidate);
    entry->adapter_schema = BONGO_CAT_MODEL_ADAPTER_SCHEMA;
    entry->adapter_generator = BONGO_CAT_MODEL_ADAPTER_GENERATOR;
    entry->managed = true;
}
