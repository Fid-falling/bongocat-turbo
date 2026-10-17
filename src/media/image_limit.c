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
 * 优先级: 诊断用环境变量 > 设置页下发的档位 > 默认 (BONGO_CAT_LIVE2D_TEXTURE_LIMIT)。
 * 环境变量排最前, 是为了让"同一份二进制换上限复现问题"不用改配置、不写盘。
 */
#include "bongo_cat/image.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL_log.h>
#include <stdlib.h>

/* 设置页下发的上限; 0 = 还没下发, 用默认值 */
static int configured_limit;
/* 解析结果缓存: -1 = 还没解析。进程内必须是常量, 否则两张贴图可能吃到不同上限。 */
static int resolved_limit = -1;

void bongo_cat_image_set_model_texture_limit(int limit) {
    configured_limit = limit > 0 ? limit : 0;
    /* 已经解析过就作废: 用户改设置之后下一个模型负载要按新上限走 */
    resolved_limit = -1;
    /* 只在真的变了时记一行 (Release 下 APPLICATION 类的 INFO 会被日志策略滤掉,
       所以走 LIFECYCLE 这个自定义分类; 见 include/bongo_cat/log.h)。 */
    static int logged = -1;
    if (logged != configured_limit) {
        logged = configured_limit;
        SDL_LogMessage(BONGO_CAT_LOG_LIFECYCLE, SDL_LOG_PRIORITY_INFO,
            "[render] 模型图集工作尺寸上限 = %d (0 表示默认 %d)", configured_limit,
            BONGO_CAT_LIVE2D_TEXTURE_LIMIT);
    }
}

int bongo_cat_image_model_texture_limit(void) {
    if (resolved_limit >= 0) return resolved_limit;
    resolved_limit =
        configured_limit > 0 ? configured_limit : BONGO_CAT_LIVE2D_TEXTURE_LIMIT;
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
