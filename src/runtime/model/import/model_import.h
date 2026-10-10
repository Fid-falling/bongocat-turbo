/*
 * 第三方模型导入的公共契约头文件: 格式识别 -> 内容摘要 -> 复制归一化 -> 原子落盘。
 * 时序: discover 识别来源, prepare_package_metadata* 原地压缩候选并算摘要与展示名,
 * prepare_storage 复制成 Mver 布局, install_discovery 以临时目录加改名提交。
 * 对外承诺: 失败不在 models_root 留下半个包(残留只有 .import-*.tmp, 由会话创建时清理)。
 * 最容易改错: 候选下标只在元数据步骤之后才与 metadata 下标一一对应。
 */
#ifndef BONGO_CAT_MODEL_IMPORT_H
#define BONGO_CAT_MODEL_IMPORT_H

#include "bongo_cat/common.h"
#include "bongo_cat/model.h"

/* 单次 discovery 能承载的候选数, 也是 receipt 与包索引数组的容量; 失败名单只保留前
   几个文件名给界面提示, 不追求列全。 */
#define BONGO_CAT_IMPORT_CANDIDATE_CAP 16
#define BONGO_CAT_IMPORT_FAILURE_NAME_CAP 3
#define BONGO_CAT_SKIN_CONFIG_FILE "bongocat.skin.json"

typedef enum BongoCatImportFormat {
    BONGO_CAT_IMPORT_TAURI,
    BONGO_CAT_IMPORT_MVER,
    BONGO_CAT_IMPORT_MVER_PATCH
} BongoCatImportFormat;

/* 一个候选 = 一个可导入的模式变体。directory 与 setting 是清单所在层与清单文件名(真正
   被转换的入口), assets 是素材根, package_root 是归属包根(去重与布局归一化按它分组);
   patch_root、config、overrides 只有 MVER_PATCH 格式用到。 */
typedef struct BongoCatImportCandidate {
    char directory[BONGO_CAT_PATH_CAP];
    char setting[BONGO_CAT_PATH_CAP];
    char assets[BONGO_CAT_PATH_CAP];
    char package_root[BONGO_CAT_PATH_CAP];
    char patch_root[BONGO_CAT_PATH_CAP];
    char config[BONGO_CAT_PATH_CAP];
    char overrides[BONGO_CAT_PATH_CAP];
    BongoCatModelMode mode;
    BongoCatImportFormat format;
    bool gamepad_buttons;
} BongoCatImportCandidate;

/* candidates[0..count) 是本次识别的全部变体; prepare_package_metadata* 会原地压缩
   candidates 并改写 count(内容去重、剔除内置模板), 所以元数据只能在其之后按下标对齐。 */
typedef struct BongoCatImportDiscovery {
    BongoCatImportCandidate candidates[BONGO_CAT_IMPORT_CANDIDATE_CAP];
    /* Basename selected by the user. This becomes the visible model id. */
    char source_name[BONGO_CAT_ID_CAP];
    size_t count;
    int depth;
    bool ambiguous;
} BongoCatImportDiscovery;

/* installed[i] 为 false 表示该变体因"同内容已经装过"被去重跳过, 不是失败;
   只有 installed_count 才是本次新增的模型数。 */
typedef struct BongoCatImportReceipt {
    char ids[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP];
    bool installed[BONGO_CAT_IMPORT_CANDIDATE_CAP];
    size_t count;
    size_t installed_count;
} BongoCatImportReceipt;

typedef void (*BongoCatImportReceiptCallback)(void *userdata,
    const BongoCatImportReceipt *receipt);

/* 批量导入的统计: failure_names 只用于界面上的失败提示(条数上限见上面的宏),
   真正的错误细节在 BongoCatError 与日志里。 */
typedef struct BongoCatImportBatchStats {
    size_t succeeded_count;
    size_t failed_count;
    size_t failure_name_count;
    char failure_names[BONGO_CAT_IMPORT_FAILURE_NAME_CAP][BONGO_CAT_ID_CAP];
} BongoCatImportBatchStats;

/* content_digest 是 64 位十六进制 SHA-256(内容标识, 跨会话去重靠它); package_id 可能
   带 "~N" 变体后缀; family_id 只在同一批次有多个变体时生成, 表示它们是同一个模型的一组。 */
typedef struct BongoCatPackageMetadata {
    char package_id[BONGO_CAT_ID_CAP];
    char content_digest[65];
    char family_id[BONGO_CAT_ID_CAP];
    char display_name[BONGO_CAT_ID_CAP];
    char source_name[BONGO_CAT_ID_CAP];
    uint32_t capabilities;
} BongoCatPackageMetadata;

typedef struct BongoCatImportDigestCache BongoCatImportDigestCache;
typedef struct BongoCatImportSession BongoCatImportSession;

typedef BongoCatResult (*BongoCatImportVisitor)(void *userdata,
    const char *source, BongoCatImportDiscovery *discovery, BongoCatError *error);

typedef struct BongoCatApp BongoCatApp;

/* Shared discovery and package preparation. */
bool bongo_cat_import_discover(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
bool bongo_cat_import_adapter_metadata(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error);
bool bongo_cat_import_write_report(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error);
bool bongo_cat_import_prepare_adapter(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error);
bool bongo_cat_import_prepare_package(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatImportCandidate *installed, BongoCatError *error);
bool bongo_cat_import_prepare_storage(
    const BongoCatImportDiscovery *discovery, const char *target,
    BongoCatError *error);
bool bongo_cat_import_authored_package(const char *directory);

/* Content identity and package metadata. */
/* 内容标识三件套: output 是 65 字节缓冲区(64 位十六进制 + NUL); inspect 额外报出
   placeholder(内置模板占位, 元数据步骤会剔除该候选); cached 版本接受可选的会话级缓存。 */
bool bongo_cat_import_candidate_digest(const BongoCatImportCandidate *candidate,
    char output[65], BongoCatError *error);
bool bongo_cat_import_candidate_inspect(const BongoCatImportCandidate *candidate,
    char output[65], bool *placeholder, BongoCatError *error);
bool bongo_cat_import_candidate_inspect_cached(
    const BongoCatImportCandidate *candidate, char output[65],
    bool *placeholder, BongoCatImportDigestCache *cache,
    BongoCatError *error);
/* 摘要缓存按 (路径, 大小, 修改时间) 命中, 由调用方持有并释放; 传 NULL 表示不缓存
   (prepare_package_metadata 走的就是这条路径)。 */
BongoCatImportDigestCache *bongo_cat_import_digest_cache_create(void);
void bongo_cat_import_digest_cache_destroy(BongoCatImportDigestCache *cache);
bool bongo_cat_import_digest_file_cached(BongoCatImportDigestCache *cache,
    const char *path, uint64_t size, uint64_t modified, char output[65]);
bool bongo_cat_import_prepare_package_metadata(
    BongoCatImportDiscovery *discovery,
    BongoCatPackageMetadata *metadata, BongoCatError *error);
bool bongo_cat_import_prepare_package_metadata_cached(
    BongoCatImportDiscovery *discovery, BongoCatPackageMetadata *metadata,
    BongoCatImportDigestCache *cache, BongoCatError *error);
bool bongo_cat_import_package_id(char *output, size_t capacity,
    const char *name);
bool bongo_cat_import_variant_id(char *output, size_t capacity,
    const char *package_id, size_t variant_index);
bool bongo_cat_import_package_base_id(char *output, size_t capacity,
    const char *model_id);
uint32_t bongo_cat_import_candidate_capabilities(
    const BongoCatImportCandidate *candidate);

/* Runtime adapter and catalog integration. */
void bongo_cat_import_describe_nearby_entry(BongoCatModelEntry *entry,
    const BongoCatImportCandidate *candidate, const char *id,
    const char *identity, const char *source_hash, const char *source,
    const char *adapter);
void bongo_cat_import_apply_metadata(BongoCatApp *app, const char *model_id,
    const char *directory);
bool bongo_cat_import_render_options(const char *directory,
    BongoCatLive2DRenderOptions *options);

/* Recursive source scanning. */
BongoCatResult bongo_cat_import_scan(const char *root,
    BongoCatImportVisitor visitor, void *userdata, BongoCatError *error);
BongoCatResult bongo_cat_import_scan_diagnostic(const char *root,
    BongoCatImportVisitor visitor, void *userdata, BongoCatError *error);
/* budget_ns 是整次扫描的墙钟预算: 用完后扫描提前收尾但不报错, 语义是"结果可能不完整"
   而不是失败。 */
BongoCatResult bongo_cat_import_scan_budget(const char *root,
    BongoCatImportVisitor visitor, void *userdata, uint64_t budget_ns,
    BongoCatError *error);
BongoCatResult bongo_cat_import_source_directory(const char *source,
    char *directory, size_t capacity, BongoCatError *error);

/* Nearby and installed catalog discovery. */
BongoCatResult bongo_cat_import_nearby_root(BongoCatApp *app,
    const char *root, BongoCatError *error);
BongoCatResult bongo_cat_import_nearby_scan(BongoCatApp *app,
    const char *root, BongoCatError *error);
BongoCatResult bongo_cat_import_nearby(BongoCatApp *app,
    const char *root, BongoCatError *error);
BongoCatResult bongo_cat_import_installed_models(BongoCatApp *app,
    const char *root, BongoCatError *error);
BongoCatResult bongo_cat_import_installed_package(BongoCatApp *app,
    const char *root, const char *package_id, BongoCatError *error);

/* Installation sessions and progressive batch import. */
BongoCatResult bongo_cat_import_install(const char *source,
    const char *models_root, BongoCatImportReceipt *receipt,
    BongoCatError *error);
/* session 持有 models_root、摘要缓存与已安装包索引, 不是线程安全的; 每次 install 内部
   都会短暂获取全局存储锁, 因此必须在同一线程上串行使用。 */
BongoCatImportSession *bongo_cat_import_session_create(const char *models_root,
    BongoCatError *error);
void bongo_cat_import_session_destroy(BongoCatImportSession *session);
BongoCatResult bongo_cat_import_session_install(
    BongoCatImportSession *session, const char *source,
    BongoCatImportReceipt *receipt, BongoCatError *error);
BongoCatResult bongo_cat_import_session_install_progressive(
    BongoCatImportSession *session, const char *source,
    BongoCatImportReceiptCallback callback, void *userdata,
    BongoCatImportBatchStats *stats, BongoCatError *error);

#endif
