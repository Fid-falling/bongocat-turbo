#include "bongo_cat/config.h"
#include "bongo_cat/utf8.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * 默认值与合法性钳制的唯一来源。配置可能来自手改或旧版本, 所以读进来之后
 * 一律要过一遍 bongo_cat_settings_validate; 之后渲染/输入侧就可以直接相信
 * 结构里的数值都在允许范围内, 不必到处再判一次。
 */
/*
 * 最近一次校验纠正过的项, 交给运行时打日志 (核心层不依赖 SDL, 自己打不了)。
 * "渲染精度被改回允许范围"必须让用户在日志里看见: 手改配置文件的人不会知道
 * 程序悄悄换掉了他写的那一档。
 */
static char validation_note[192];

const char *bongo_cat_settings_validation_note(void) { return validation_note; }

void bongo_cat_settings_note(const char *text) {
    if (!text) return;
    snprintf(validation_note, sizeof(validation_note), "%s", text);
}

void bongo_cat_settings_clear_validation_note(void) { validation_note[0] = '\0'; }

static float clampf_or(float value, float low, float high, float fallback) {
    if (!isfinite(value)) return fallback;
    return value < low ? low : value > high ? high : value;
}

static bool shortcut_equal(const char *left, const char *right) {
    if (!left || !right || !left[0] || !right[0]) return false;
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++))
            return false;
    }
    return *left == *right;
}

/* 定长文本字段的收尾: 保证 NUL 结尾、把尾部残留字节清零(否则写回文件时会
   带出上一次的垃圾), 非法 UTF-8 直接清空整段而不是留下半个字符。 */
static bool normalize_text(char *text, size_t capacity) {
    text[capacity - 1] = '\0';
    if (!bongo_cat_utf8_valid(text)) {
        memset(text, 0, capacity);
        return false;
    }
    size_t length = strlen(text);
    memset(text + length + 1, 0, capacity - length - 1);
    return true;
}

/*
 * 快捷键冲突检查: 全局命令(显示/隐藏、打开设置、镜像、穿透、置顶)之间必须
 * 互斥; 模型动作绑定的键彼此可以重复, 但与全局命令冲突时以全局为准。
 * exclude 用于"正在改的就是这一项"时忽略它自己。大小写不敏感。
 */
bool bongo_cat_settings_shortcut_conflicts(const BongoCatSettings *config,
    const char *shortcut, const char *exclude) {
    if (!config || !shortcut || !shortcut[0]) return false;
    const char *global[] = {config->shortcuts.toggle_pet_visibility,
        config->shortcuts.visible_preferences, config->shortcuts.mirror,
        config->shortcuts.pass_through, config->shortcuts.always_on_top};
    for (size_t i = 0; i < sizeof(global) / sizeof(global[0]); ++i)
        if (global[i] != exclude && shortcut_equal(global[i], shortcut))
            return true;
    size_t behavior_count = config->behavior_shortcut_count;
    if (behavior_count > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        behavior_count = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    /* Model behaviors may share a key; global commands remain exclusive. */
    for (size_t i = 0; i < behavior_count; ++i)
        if (config->behavior_shortcuts[i].shortcut == exclude) return false;
    for (size_t i = 0; i < behavior_count; ++i) {
        const char *bound = config->behavior_shortcuts[i].shortcut;
        if (shortcut_equal(bound, shortcut)) return true;
    }
    return false;
}

/*
 * 冲突的解决方式是"后者让位": 重复的全局键被清空, 模型动作里与全局键相同的
 * 绑定也被清空。这里只做清理不提示, 由 UI 负责告诉用户。
 * 两两比较是按数组顺序做的, 所以最终保留的是先出现的那一项。
 */
static void validate_shortcuts(BongoCatSettings *config) {
    char *global[] = {config->shortcuts.toggle_pet_visibility,
        config->shortcuts.visible_preferences, config->shortcuts.mirror,
        config->shortcuts.pass_through, config->shortcuts.always_on_top};
    for (size_t i = 0; i < sizeof(global) / sizeof(global[0]); ++i) {
        normalize_text(global[i], BONGO_CAT_SHORTCUT_CAP);
        for (size_t j = 0; j < i; ++j)
            if (shortcut_equal(global[i], global[j]))
                memset(global[i], 0, BONGO_CAT_SHORTCUT_CAP);
    }
    for (size_t i = 0; i < config->behavior_shortcut_count; ++i) {
        char *shortcut = config->behavior_shortcuts[i].shortcut;
        bool duplicate = false;
        for (size_t j = 0; j < sizeof(global) / sizeof(global[0]); ++j)
            duplicate = duplicate || shortcut_equal(shortcut, global[j]);
        if (duplicate) shortcut[0] = '\0';
    }
}

/*
 * 三个 compact_* 做的是同一件事: 把可能被外部改坏的定长数组就地整理成规范
 * 形态 —— 截断超长字段、丢弃非法或空条目、按 id 去重(后出现的字段覆盖先出现
 * 的)、最后清零剩余槽位并把 count 收敛到真实条目数。
 * 不变量: 前 count 项一定有效, 其余槽位一定是 0。写回文件前必须经过它。
 */
static void compact_behavior_overrides(BongoCatSettings *config) {
    size_t input_count = config->behavior_shortcut_count;
    if (input_count > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        input_count = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    size_t output_count = 0;
    for (size_t i = 0; i < input_count; ++i) {
        BongoCatBehaviorShortcut entry = config->behavior_shortcuts[i];
        entry.id[sizeof(entry.id) - 1] = '\0';
        entry.shortcut[sizeof(entry.shortcut) - 1] = '\0';
        entry.label[sizeof(entry.label) - 1] = '\0';
        if (!bongo_cat_utf8_valid(entry.id) ||
            !bongo_cat_utf8_valid(entry.shortcut) ||
            !bongo_cat_utf8_valid(entry.label)) continue;
        if (entry.shortcut[0]) entry.shortcut_disabled = false;
        if (!entry.id[0] || (!entry.shortcut[0] && !entry.label[0] && !entry.shortcut_disabled)) continue;
        BongoCatBehaviorShortcut canonical = {0};
        canonical.shortcut_disabled = entry.shortcut_disabled;
        snprintf(canonical.id, sizeof(canonical.id), "%s", entry.id);
        snprintf(canonical.shortcut, sizeof(canonical.shortcut), "%s",
            entry.shortcut);
        snprintf(canonical.label, sizeof(canonical.label), "%s", entry.label);
        size_t existing = output_count;
        for (size_t j = 0; j < output_count; ++j)
            if (!strcmp(config->behavior_shortcuts[j].id, entry.id)) {
                existing = j;
                break;
            }
        if (existing < output_count) {
            if (canonical.shortcut[0] || canonical.shortcut_disabled) {
                config->behavior_shortcuts[existing].shortcut_disabled = canonical.shortcut_disabled;
                memset(config->behavior_shortcuts[existing].shortcut, 0,
                    sizeof(config->behavior_shortcuts[existing].shortcut));
                snprintf(
                    config->behavior_shortcuts[existing].shortcut,
                    sizeof(config->behavior_shortcuts[existing].shortcut),
                    "%s", canonical.shortcut);
            }
            if (canonical.label[0]) {
                memset(config->behavior_shortcuts[existing].label, 0,
                    sizeof(config->behavior_shortcuts[existing].label));
                snprintf(
                    config->behavior_shortcuts[existing].label,
                    sizeof(config->behavior_shortcuts[existing].label),
                    "%s", canonical.label);
            }
            continue;
        }
        config->behavior_shortcuts[output_count++] = canonical;
    }
    memset(&config->behavior_shortcuts[output_count], 0,
        (BONGO_CAT_BEHAVIOR_BINDING_CAP - output_count) *
        sizeof(config->behavior_shortcuts[0]));
    config->behavior_shortcut_count = output_count;
}

static void compact_model_overrides(BongoCatSettings *config) {
    size_t input_count = config->model_label_count;
    if (input_count > BONGO_CAT_MODEL_CAP) input_count = BONGO_CAT_MODEL_CAP;
    size_t output_count = 0;
    for (size_t i = 0; i < input_count; ++i) {
        BongoCatModelLabel entry = config->model_labels[i];
        entry.id[sizeof(entry.id) - 1] = '\0';
        entry.label[sizeof(entry.label) - 1] = '\0';
        if (!entry.id[0] || !entry.label[0] ||
            !bongo_cat_utf8_valid(entry.id) ||
            !bongo_cat_utf8_valid(entry.label)) continue;
        BongoCatModelLabel canonical = {0};
        snprintf(canonical.id, sizeof(canonical.id), "%s", entry.id);
        snprintf(canonical.label, sizeof(canonical.label), "%s", entry.label);
        size_t existing = output_count;
        for (size_t j = 0; j < output_count; ++j)
            if (!strcmp(config->model_labels[j].id, entry.id)) {
                existing = j;
                break;
            }
        if (existing < output_count) {
            memset(config->model_labels[existing].label, 0,
                sizeof(config->model_labels[existing].label));
            snprintf(config->model_labels[existing].label,
                sizeof(config->model_labels[existing].label), "%s",
                canonical.label);
            continue;
        }
        config->model_labels[output_count++] = canonical;
    }
    memset(&config->model_labels[output_count], 0,
        (BONGO_CAT_MODEL_CAP - output_count) *
        sizeof(config->model_labels[0]));
    config->model_label_count = output_count;
}

static void compact_removed_models(BongoCatSettings *config) {
    size_t input_count = config->removed_model_count;
    if (input_count > BONGO_CAT_MODEL_CAP) input_count = BONGO_CAT_MODEL_CAP;
    size_t output_count = 0;
    for (size_t i = 0; i < input_count; ++i) {
        BongoCatRemovedModel entry = config->removed_models[i];
        entry.id[sizeof(entry.id) - 1] = '\0';
        if (!entry.id[0] || !bongo_cat_utf8_valid(entry.id)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < output_count; ++j)
            if (!strcmp(config->removed_models[j].id, entry.id)) {
                duplicate = true;
                break;
            }
        if (!duplicate) {
            BongoCatRemovedModel canonical = {0};
            snprintf(canonical.id, sizeof(canonical.id), "%s", entry.id);
            config->removed_models[output_count++] = canonical;
        }
    }
    memset(&config->removed_models[output_count], 0,
        (BONGO_CAT_MODEL_CAP - output_count) *
        sizeof(config->removed_models[0]));
    config->removed_model_count = output_count;
}

static void compact_hidden_models(BongoCatSettings *config) {
    size_t input_count = config->hidden_model_count;
    if (input_count > BONGO_CAT_MODEL_CAP) input_count = BONGO_CAT_MODEL_CAP;
    size_t output_count = 0;
    for (size_t i = 0; i < input_count; ++i) {
        BongoCatRemovedModel entry = config->hidden_models[i];
        entry.id[sizeof(entry.id) - 1] = '\0';
        if (!entry.id[0] || !bongo_cat_utf8_valid(entry.id)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < output_count; ++j)
            if (!strcmp(config->hidden_models[j].id, entry.id)) {
                duplicate = true;
                break;
            }
        if (!duplicate) {
            BongoCatRemovedModel canonical = {0};
            snprintf(canonical.id, sizeof(canonical.id), "%s", entry.id);
            config->hidden_models[output_count++] = canonical;
        }
    }
    memset(&config->hidden_models[output_count], 0,
        (BONGO_CAT_MODEL_CAP - output_count) *
        sizeof(config->hidden_models[0]));
    config->hidden_model_count = output_count;
}

static void compact_random_disabled(BongoCatSettings *config) {
    size_t input_count = config->random_disabled_count;
    if (input_count > BONGO_CAT_RANDOM_DISABLED_CAP)
        input_count = BONGO_CAT_RANDOM_DISABLED_CAP;
    size_t output_count = 0;
    for (size_t i = 0; i < input_count; ++i) {
        char temp[BONGO_CAT_BEHAVIOR_ID_CAP];
        memcpy(temp, config->random_disabled[i], BONGO_CAT_BEHAVIOR_ID_CAP);
        temp[BONGO_CAT_BEHAVIOR_ID_CAP - 1] = '\0';
        if (!temp[0] || !bongo_cat_utf8_valid(temp)) continue;
        char *existing = NULL;
        for (size_t j = 0; j < output_count; ++j)
            if (!strcmp(config->random_disabled[j], temp)) {
                existing = config->random_disabled[j];
                break;
            }
        if (!existing) {
            memset(config->random_disabled[output_count], 0,
                BONGO_CAT_BEHAVIOR_ID_CAP);
            snprintf(config->random_disabled[output_count],
                BONGO_CAT_BEHAVIOR_ID_CAP, "%s", temp);
            output_count++;
        }
    }
    for (size_t i = output_count; i < BONGO_CAT_RANDOM_DISABLED_CAP; ++i)
        memset(config->random_disabled[i], 0, BONGO_CAT_BEHAVIOR_ID_CAP);
    config->random_disabled_count = output_count;
}

/* 全新配置的初值: 首次运行、没有配置文件时由调用方用它铺底。
   由于读取是可选读(缺键保留原值), 这里给出的初值同时就是那些键的默认值,
   改动它会直接改变老配置的行为。 */
void bongo_cat_settings_defaults(BongoCatSettings *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->model.mouse_centered = true;
    config->model.max_fps = BONGO_CAT_DEFAULT_MAX_FPS;
    /*
     * 垂直同步默认留"未设置"而不是直接 true: 老版本里它不是一个独立设置, 而是由最大
     * 帧率推出来的 (限帧 30/60 -> 开, "无限" -> 关, 见 frame_clock.c)。sentinel 让
     * 校验阶段能按这条老规则收口, 于是**新装的默认值和老用户升级后的行为完全一致**
     * (默认 60 帧 => 开同步), 只有用户真的动过这个开关才会覆盖它。
     */
    config->model.vsync = BONGO_CAT_VSYNC_DEFAULT;
    /* 多键显示默认打开: 同侧多键本来就"能连上", 只是旧实现每侧只留最后一个键, 用户
       看不到自己按了什么。关掉它才需要理由, 所以默认值是 1。 */
    config->model.multi_key_display = 1;
    config->model.multi_key_limit = BONGO_CAT_MULTI_KEY_LIMIT_DEFAULT;
    /* 默认合并: 一个气泡里 "A + S" 比并排 N 个气泡更接近"用户按了什么"的直觉读法 */
    config->model.bubble_merge = 1;
    /* 渲染精度默认 1.0 = 按窗口尺寸直接渲染, 与 1.17.0 逐像素一致: 更高的默认值等于
       替用户付显卡/显存的代价, 只有用户自己要求时才该升级。 */
    config->model.render_scale = BONGO_CAT_RENDER_SCALE_DEFAULT;
    /* 默认走 OpenGL: Vulkan 现在是显式选项 (见 render_backend.c 的 resolve) */
    config->model.renderer = BONGO_CAT_RENDERER_OPENGL;
    config->window.always_on_top = true;
    config->window.keep_in_screen = false;
    config->window.edge_snap = false;
    config->window.edge_snap_threshold = BONGO_CAT_DEFAULT_EDGE_SNAP_PX;
    config->window.slide_animation = true;
    config->window.slide_slowdown_percent = BONGO_CAT_DEFAULT_SLIDE_SLOWDOWN_PERCENT;
    /* 默认关闭: "永不停止的滑行"是刻意选的玩法, 不能替用户打开 (它会让宠物一直动)。 */
    config->window.slide_no_decay = false;
    /* 默认关闭透明背景 (与 1.17.0 一致): 开启后走代理分层呈现, 录屏软件按标题采集
       会拿到全透明画面, 不能替用户默认打开。 */
    config->window.gl_transparency = 0;
    config->window.obs_background_color = BONGO_CAT_OBS_BACKGROUND_GREEN;
    config->window.corner_radius_percent = BONGO_CAT_DEFAULT_WINDOW_CORNER_PERCENT;
    config->window.hide_fade_seconds = BONGO_CAT_DEFAULT_HIDE_FADE_SECONDS;
    config->window.random_expression_interval_seconds =
        BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS;
    config->window.random_motion_interval_seconds =
        BONGO_CAT_DEFAULT_RANDOM_MOTION_SECONDS;
    config->window.random_sound_interval_seconds =
        BONGO_CAT_DEFAULT_RANDOM_SOUND_SECONDS;
    config->window.sequential_model_interval_seconds =
        BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS;
    config->app.tray_visible = true;
    /* 缺 Core 的一次性引导还没发生过, 用户也没说过"别再提示"。两条都必须是 false:
       有一个 true 就会让全新安装的机器再也看不到导入引导。 */
    config->app.live2d_core_prompt_shown = false;
    config->app.live2d_core_prompt_dismissed = false;
    /* 默认以管理员身份运行 (与 BongoCat-X 一致, 但 turbo 的默认值是开): 启动时
       请求提权。因此每次启动 (含开机自启) 都会弹一次 UAC, 用户可在设置里关掉。 */
    config->app.run_as_admin = true;
    config->app.theme = BONGO_CAT_THEME_AUTO;
    config->app.language = BONGO_CAT_LANG_EN_US;
    config->cache_root[0] = '\0';
    memcpy(config->extensions_json, "{}", sizeof("{}"));
}

/*
 * 幂等的收口函数: 读配置、改配置、写配置之前都会被调用。它只做钳制与整理,
 * 不改变语义, 所以调用多少次结果都一样, 调用方不需要判断"是否已经校验过"。
 * 末尾再跑一次 compact_behavior_overrides, 是因为 validate_shortcuts 可能
 * 刚把某些 shortcut 清空, 清空之后又会多出空条目。
 */
void bongo_cat_settings_validate(BongoCatSettings *config) {
    if (!config) return;
    int max_fps = config->model.max_fps;
    if (max_fps != BONGO_CAT_MAX_FPS_UNLIMITED)
        max_fps = max_fps > 0 && max_fps <= 30 ? 30 : BONGO_CAT_DEFAULT_MAX_FPS;
    config->model.max_fps = max_fps;
    /*
     * 垂直同步收口: 未设置(-1)就按老规则从最大帧率推 (此时 max_fps 已经钳过, 用的是
     * 最终值), 其余非法值一律当"开" —— 见 config.h 的三态注释。收口之后结构体里只会
     * 剩 0/1, 于是保存写盘的是具体开关状态: 这份配置一旦被本版本写过, 即使之后又在
     * 设置里把帧率改成"无限", 也不会再退回"同步跟着帧率变"的老语义 (那时用户已经能
     * 在界面上直接看到这个开关了)。
     */
    if (config->model.vsync != 0 && config->model.vsync != 1)
        config->model.vsync = config->model.vsync == BONGO_CAT_VSYNC_DEFAULT ?
            BONGO_CAT_VSYNC_FOR_MAX_FPS(max_fps) : 1;
    if ((unsigned)config->model.renderer >= BONGO_CAT_RENDERER_COUNT)
        config->model.renderer = BONGO_CAT_RENDERER_OPENGL;
    config->model.multi_key_display = config->model.multi_key_display != 0;
    config->model.bubble_merge = config->model.bubble_merge != 0;
    if (config->model.multi_key_limit < 1 ||
        config->model.multi_key_limit > BONGO_CAT_MULTI_KEY_LIMIT_MAX)
        config->model.multi_key_limit = BONGO_CAT_MULTI_KEY_LIMIT_DEFAULT;
    /*
     * 渲染精度: clamp 到 [0.1, 4.0] 并按 0.1 取整。
     * 越界 (手改文件写 0/-1/9) 与"不在步进上"(1.234) 都在这里纠正: 前者会让离屏目标
     * 变成 0 像素或吃掉几个 GB 显存, 后者会让人以为"我设的值没生效"(设置页一次拨 0.1)。
     * 非数字在 read_float 那一步就拒读并保留默认值, 走到这里的 NaN 只是兜底。
     * 纠正要留一条 WARN: 用户改过文件却看到设置被改回边界值时, 这是唯一线索。
     */
    {
        const float requested = config->model.render_scale;
        float corrected = clampf_or(requested, BONGO_CAT_RENDER_SCALE_MIN,
            BONGO_CAT_RENDER_SCALE_MAX, BONGO_CAT_RENDER_SCALE_DEFAULT);
        corrected = roundf(corrected * 10.0f) / 10.0f;
        config->model.render_scale = corrected;
        if (!(fabsf(corrected - requested) <= 0.001f))
            snprintf(validation_note, sizeof(validation_note),
                "[render] 渲染精度 %.3f 不在允许范围内, 已纠正为 %.1f "
                "(范围 %.1f–%.1f, 步进 0.1)",
                (double)requested, (double)corrected,
                (double)BONGO_CAT_RENDER_SCALE_MIN,
                (double)BONGO_CAT_RENDER_SCALE_MAX);
    }
    config->window.hide_delay_seconds = clampf_or(
        config->window.hide_delay_seconds, 0.0f, 60.0f, 0.0f);
    config->window.hide_fade_seconds = clampf_or(
        config->window.hide_fade_seconds, 0.0f,
        BONGO_CAT_MAX_HIDE_FADE_SECONDS, BONGO_CAT_DEFAULT_HIDE_FADE_SECONDS);
    config->window.corner_radius_percent = clampf_or(
        config->window.corner_radius_percent, 0.0f, 50.0f,
        BONGO_CAT_DEFAULT_WINDOW_CORNER_PERCENT);
    if (config->window.edge_snap_threshold < 0 ||
        config->window.edge_snap_threshold > BONGO_CAT_MAX_EDGE_SNAP_PX)
        config->window.edge_snap_threshold = BONGO_CAT_DEFAULT_EDGE_SNAP_PX;
    if (config->window.slide_slowdown_percent < 0 ||
        config->window.slide_slowdown_percent > BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT)
        config->window.slide_slowdown_percent = BONGO_CAT_DEFAULT_SLIDE_SLOWDOWN_PERCENT;
    config->window.random_expression_interval_seconds = clampf_or(
        config->window.random_expression_interval_seconds, 1.0f, 3600.0f,
        BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS);
    config->window.random_motion_interval_seconds = clampf_or(
        config->window.random_motion_interval_seconds, 1.0f, 3600.0f,
        BONGO_CAT_DEFAULT_RANDOM_MOTION_SECONDS);
    config->window.random_sound_interval_seconds = clampf_or(
        config->window.random_sound_interval_seconds, 1.0f, 3600.0f,
        BONGO_CAT_DEFAULT_RANDOM_SOUND_SECONDS);
    config->window.sequential_model_interval_seconds = clampf_or(
        config->window.sequential_model_interval_seconds, 1.0f, 3600.0f,
        BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS);
    if ((unsigned)config->window.obs_background_color >=
        BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT)
        config->window.obs_background_color = BONGO_CAT_OBS_BACKGROUND_GREEN;
    /* 0/1 布尔开关的钳制: 手改文件填 0/1 以外的值一律按关闭处理。 */
    config->window.gl_transparency = !!config->window.gl_transparency;
    if ((unsigned)config->app.theme > BONGO_CAT_THEME_DARK)
        config->app.theme = BONGO_CAT_THEME_AUTO;
    if ((unsigned)config->app.language >= BONGO_CAT_LANG_COUNT)
        config->app.language = BONGO_CAT_LANG_EN_US;
    compact_behavior_overrides(config);
    compact_model_overrides(config);
    compact_removed_models(config);
    compact_hidden_models(config);
    compact_random_disabled(config);
    validate_shortcuts(config);
    compact_behavior_overrides(config);
}
