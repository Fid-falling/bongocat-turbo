/*
 * 模型图集工作尺寸上限的运行时取值 (见 include/bongo_cat/image.h 的说明)。
 *
 * 为什么单独一个文件而不是塞进 media/image.c 或 live2d/cubism_model_load.cpp:
 * GL 那条路在宿主 (bongo_cat_core) 里, Vulkan 那条路在渲染组件 (bongo_live2d.dll) 里,
 * 而组件按纪律不链接宿主 core。上限必须是同一个数, 否则同一个模型在两个后端会拿到不同
 * 分辨率的图集, 画面就对不上 —— 宿主的这一份实现读设置档位与环境变量, 组件那边只把
 * "向宿主服务表要一个数"实现成同名函数 (见 component_host.c), 于是谁都不必把对方的实现
 * 拖进来, 只有一处判定逻辑。
 *
 * 两条收缩规则 (取更小的那个):
 *   1. 设置档位: 2048 / 4096 / 原生, 由设置页下发;
 *   2. 显示尺寸: 图集再大, 屏幕上也就宠物窗口那么大。采样密度超过显示像素的 2 倍之后
 *      肉眼已经分不出差别, 而代价 (解码 + 上传 + SDK 建渲染器) 随面积平方增长 ——
 *      实测一个 8192x4096 图集在 234x234 的窗口上, 光建渲染器就要 4.4 秒。
 *      所以还要按"最长边 x2, 下限 1024, 向上取 512 的整数倍"再收一次。
 *      窗口尺寸未知 (还没建窗) 时不收缩。
 *
 * 优先级: 诊断用环境变量 > 上面两条 > 默认 (BONGO_CAT_LIVE2D_TEXTURE_LIMIT)。
 * 环境变量排最前, 是为了让"同一份二进制换上限复现问题"不用改配置、不写盘 —— 它是排查
 * 贴图问题的唯一开关, 所以连显示尺寸的收缩也不允许盖掉它。
 */
#include "bongo_cat/image.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL_log.h>
#include <stdlib.h>

/* 设置页下发的上限; 0 = 还没下发, 用默认值 */
static int configured_limit;
/* 宠物窗口最长边的像素数; 0 = 未知 (不按显示尺寸收缩) */
static int display_pixels;
/* 解析结果缓存: -1 = 还没解析。进程内必须是常量, 否则两张贴图可能吃到不同上限。 */
static int resolved_limit = -1;

/* 显示尺寸换算成"值得的上限": 2 倍过采样, 最低 1024, 向上取 512 的倍数 —— 512 对齐让同一个
   窗口尺寸反复算出的上限稳定, 也让"档位"变成离散的几级: 放大时按级升, 不会每改一像素就重载。
   下限为什么是 1024 而不是 512: 实测同一个 8192 图集的模型在 234px 窗口上, 512 会明显发虚
   (头发与眼睛的边缘糊成一片), 1024 与 2048/原生肉眼无差别。 */
static int limit_for_display(int longest_side) {
    if (longest_side <= 0) return 0;
    long wanted = (long)longest_side * 2;
    if (wanted < 1024) wanted = 1024;
    if (wanted > BONGO_CAT_LIVE2D_TEXTURE_MAX_LIMIT)
        wanted = BONGO_CAT_LIVE2D_TEXTURE_MAX_LIMIT;
    return (int)(((wanted + 511) / 512) * 512);
}

/* 生效值变化时记一行。Release 下 APPLICATION 类的 INFO 会被日志策略滤掉, 所以走 LIFECYCLE
   这个自定义分类 (见 include/bongo_cat/log.h)。
   为什么要带上设置档位与窗口尺寸: 用户报"图集糊了"时, 先要分清是档位选的低、还是窗口太大
   之后按显示尺寸收窄了 —— 只报一个数字看不出是哪一种。 */
static void log_effective_limit(void) {
    static int logged = -1;
    int effective = bongo_cat_image_model_texture_limit();
    if (logged == effective) return;
    logged = effective;
    SDL_LogMessage(BONGO_CAT_LOG_LIFECYCLE, SDL_LOG_PRIORITY_INFO,
        "[render] 模型图集工作尺寸上限 = %d (设置档位 %d, 宠物窗口最长边 %dpx, "
        "默认 %d)", effective, configured_limit, display_pixels,
        BONGO_CAT_LIVE2D_TEXTURE_LIMIT);
}

void bongo_cat_image_set_model_texture_limit(int limit) {
    configured_limit = limit > 0 ? limit : 0;
    /* 已经解析过就作废: 用户改设置之后下一个模型负载要按新上限走 */
    resolved_limit = -1;
    log_effective_limit();
}

/* 窗口尺寸变化时更新。缓存作废是必须的: 否则用户把宠物放大之后, 下一次模型负载仍会
   按旧窗口算出的小上限去解码。 */
void bongo_cat_image_set_model_display_size(int longest_side_pixels) {
    int value = longest_side_pixels > 0 ? longest_side_pixels : 0;
    if (value == display_pixels) return;
    display_pixels = value;
    resolved_limit = -1;
    log_effective_limit();
}

int bongo_cat_image_model_texture_limit(void) {
    if (resolved_limit >= 0) return resolved_limit;
    resolved_limit =
        configured_limit > 0 ? configured_limit : BONGO_CAT_LIVE2D_TEXTURE_LIMIT;
    /* 先按显示尺寸收, 再看环境变量: 诊断时给的数字必须原样生效 */
    const int scaled = limit_for_display(display_pixels);
    if (scaled > 0 && scaled < resolved_limit) resolved_limit = scaled;
    const char *value = SDL_getenv("BONGO_CAT_LIVE2D_TEXTURE_LIMIT");
    if (value && value[0]) {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end && *end == '\0' && parsed >= BONGO_CAT_LIVE2D_TEXTURE_MIN_LIMIT &&
            parsed <= BONGO_CAT_LIVE2D_TEXTURE_MAX_LIMIT) {
            resolved_limit = (int)parsed;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "模型图集工作尺寸上限被环境变量改成 %d (设置档位 %d, 默认 %d)",
                resolved_limit, configured_limit, BONGO_CAT_LIVE2D_TEXTURE_LIMIT);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "BONGO_CAT_LIVE2D_TEXTURE_LIMIT=%s 无效 (允许 %d..%d), 保持 %d",
                value, BONGO_CAT_LIVE2D_TEXTURE_MIN_LIMIT,
                BONGO_CAT_LIVE2D_TEXTURE_MAX_LIMIT, resolved_limit);
        }
    }
    return resolved_limit;
}
