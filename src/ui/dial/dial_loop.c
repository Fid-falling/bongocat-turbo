/*
 * 轮盘的模态循环, 从 dial_window.c 拆出来只为守住单文件 500 行 (cmake/CheckLines.cmake)。
 * 窗口怎么建在 dial_window.c, 这里只负责"每一帧做什么"以及"失败怎么办"。
 *
 * 两条失败处理是本文件的核心:
 *   1. 建窗失败 (dial_window_create 返回 false) —— 在 Vulkan 模式下整窗换 OpenGL 重来
 *      一次。窗口的图形 API 标志在创建时就定死了, 所以回退必然是重建;
 *   2. 连续 N 帧画不出来 (DIAL_PAINT_FAILURE_LIMIT) —— 同上。单帧失败只计数, 因为
 *      拖窗口尺寸导致的交换链过期是正常现象。
 * 用户是"右键想看菜单"才走到这里的: 后端的问题不该让菜单彻底消失。
 */
#include "dial_internal.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/log.h"
#include "bongo_cat/memory.h"
#include <stdlib.h>
#ifdef _WIN32
#include "windows_layered.h"
#endif

/* 菜单开合/悬停动画的步进。dirty 必须在这里标: 漏标的表现是菜单卡住不动。 */
static void animate(Dial *d, uint64_t now) {
    float elapsed = fminf(.1f,(float)(now-d->animation_at)/1000);
    d->animation_at = now;
    float t = fminf(1,(float)(now-d->opened_at)/700);
    float opening = .85f+.15f*(1-powf(1-t,4));
    if (opening != d->opening) { d->opening = opening; d->dirty = true; }
    float response = 1-expf(-17*elapsed);
    for (int i = 0; i < d->count; ++i) {
        if (d->child_focus && i != d->active) { d->lift[i] = 0; continue; }
        float target = d->active == i ? 1.0f : 0.0f;
        float next = d->lift[i]+(target-d->lift[i])*response;
        if (fabsf(next-target) < .002f) next = target;
        if (next != d->lift[i]) { d->lift[i] = next; d->dirty = true; }
    }
    if (dial_child_count(d) && now-d->changed_at <=
        (uint64_t)(DIAL_REVEAL_DURATION_MS +
            (dial_child_count(d)-1)*DIAL_REVEAL_DELAY_MS + 16))
        d->dirty = true;
    if (now-d->opened_at <= (uint64_t)(DIAL_REVEAL_DURATION_MS +
        (d->count-1)*DIAL_REVEAL_DELAY_MS + 16)) d->dirty = true;
}

/* 释放一个没能跑起来的 Vulkan 菜单窗口: 交换链/表面先于窗口销毁, 命令只要自洽。 */
static void abandon_vulkan_window(Dial *d) {
    if (d->vk) { bongo_cat_vk_destroy(d->vk); d->vk = NULL; }
    if (d->window_id && SDL_GetWindowFromID(d->window_id))
        SDL_DestroyWindow(d->window);
    d->window = NULL;
    d->window_id = 0;
}

/*
 * 真正跑一次模态菜单。retry_with_opengl 为真表示"Vulkan 这条窗口用不了, 请用 GL 再
 * 来一次" —— 调用方 (bongo_cat_platform_context_menu) 负责重跑。
 */
static BongoCatMenuAction menu_run(BongoCatPlatform *platform,
    const BongoCatMenuLabels *labels, bool force_opengl, bool *retry_with_opengl) {
    Dial *d = calloc(1,sizeof(*d));
    if (!d) return BONGO_CAT_MENU_NONE;
    d->owner = platform->window; d->labels = labels; d->dark = labels->dark_theme;
    /* 设置项的快照: 菜单是模态的, 打开期间设置不会变, 所以取一次就够(见 dial_internal.h)。 */
    d->gl_transparency = platform->gl_transparency;
    d->force_opengl = force_opengl;
    d->previous_window = SDL_GL_GetCurrentWindow();
    d->previous_context = SDL_GL_GetCurrentContext();
    d->active = d->child = d->pressed = -1;
    dial_items(d);
    bool ready = dial_window_create(d);
    if (!ready) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"Radial menu initialization failed: %s",SDL_GetError());
        /*
         * 防御性回退: 菜单窗口这次是按 Vulkan 建的、现在建不起来了 —— 但宠物窗口本身
         * 可能好好的 (Vulkan 只是这块窗口/这条交换链有问题)。用 GL 再来一次, 总比让
         * 用户右键之后什么都看不到强。只重试一次 (force_opengl 已经为真就不再重试)。
         *
         * 判据用 vk_attempted 而不是 d->vk: 上下文创建失败时 d->vk 仍是 NULL, 只看它
         * 会把"试过但失败"当成"没走 Vulkan", 回退就永远不会触发。
         */
        if (d->vk_attempted && !d->force_opengl) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Radial menu Vulkan initialization failed; retrying with OpenGL");
            abandon_vulkan_window(d);
            free(d);
            if (retry_with_opengl) *retry_with_opengl = true;
            return BONGO_CAT_MENU_NONE;
        }
    }
    while (ready && !d->done) {
        uint64_t start = SDL_GetTicks();
        ++d->frames;
        SDL_PumpEvents();
        if (!SDL_GetWindowFromID(d->window_id)) { d->done = true; break; }
        d->event_count = 0;
        SDL_FilterEvents(dial_collect_event,d);
        for (int i = 0; i < d->event_count && !d->done; ++i) {
            dial_event(d,&d->events[i]);
            if (d->done) SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "Radial menu dismissed: event=0x%x action=%d elapsed_ms=%llu",
                (unsigned)d->events[i].type,(int)d->result,
                (unsigned long long)(SDL_GetTicks()-d->opened_at));
        }
        if (d->done) break;
        if (labels->preview_tick) {
            if (!d->vk && d->previous_window && d->previous_context)
                SDL_GL_MakeCurrent(d->previous_window,d->previous_context);
            labels->preview_tick(labels->preview_userdata);
        }
        if (!d->vk && !SDL_GL_MakeCurrent(d->window,d->context)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,"Radial menu context activation failed: %s",SDL_GetError());
            break;
        }
        dial_covers_tick(d);
        animate(d,SDL_GetTicks());
        if (d->dirty) {
            d->dirty = false;
            if (!dial_paint_frame(d)) {
                /*
                 * 一帧失败不再立刻退出: 交换链过期/重建常常只影响一两帧。连续失败到
                 * 上限才认定"这个窗口端不出去了", 并把失败次数、失败码 (由 vk_frame.c
                 * 打出) 与这条日志一起留在现场。
                 */
                ++d->paint_failures;
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                    "Radial menu rendering failed (连续第 %d 次): %s "
                    "vulkan=%d presents=%d frames=%d",
                    d->paint_failures, SDL_GetError(), d->vk ? 1 : 0,
                    d->presents, d->frames);
                if (d->paint_failures < DIAL_PAINT_FAILURE_LIMIT) continue;
                /* 连续失败: Vulkan 窗口改走 GL (由调用方重跑), GL 窗口只能放弃。 */
                if (d->vk && !d->force_opengl && retry_with_opengl) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "Radial menu gave up on Vulkan after %d consecutive frame "
                        "failures (presented=%llu failures=%llu); retrying with OpenGL",
                        d->paint_failures,
                        (unsigned long long)bongo_cat_vk_presented_frames(d->vk),
                        (unsigned long long)bongo_cat_vk_present_failures(d->vk));
                    *retry_with_opengl = true;
                }
                break;
            }
            d->paint_failures = 0;
        }
        uint64_t elapsed = SDL_GetTicks()-start;
        if (elapsed < 16) SDL_Delay((Uint32)(16-elapsed));
    }
    /*
     * 帧率诊断。frames 是模态循环的迭代率, presents 才是真正回读整窗并提交给 DWM 的次数
     * —— 代理分层呈现下这两个数会明显分离(静止的菜单 presents 不再增长), 这正是帧率问题
     * 的第一手证据, 出问题时先看这一行。
     */
    if (ready) {
        uint64_t total = SDL_GetTicks()-d->opened_at;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "Radial menu loop: frames=%d presents=%d elapsed_ms=%llu fps=%.1f",
            d->frames,d->presents,(unsigned long long)total,
            total ? 1000.0*(double)d->frames/(double)total : 0.0);
        /*
         * "画了却一次都没提交"必须显式报出来。以前这条只有 frames/presents 两个数字,
         * 用户回传的日志里出现 presents=0 时无法区分"计数漏了"和"真的没提交" ——
         * 现在两种情况分别有各自的日志 (dial_paint.c 的 Vulkan 分支也会 ++presents,
         * 以及这里的 WARN)。
         */
        if (d->frames > 0 && d->presents == 0)
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Radial menu never presented a frame: frames=%d presents=0 "
                "vulkan=%d paint_failures=%d (Vulkan 的 acquire/present 失败码见上面"
                "的 [vulkan] 行)",
                d->frames, d->vk ? 1 : 0, d->paint_failures);
    }
    if (d->vk || !d->context || (SDL_GetWindowFromID(d->window_id) &&
        SDL_GL_MakeCurrent(d->window,d->context))) dial_paint_free(d);
    else {
        /* A destroyed window cannot safely own GL deletion calls. Its context
           releases GPU allocations; release only CPU allocations here. */
        if (d->paint.atlas.permanent.alloc) nk_font_atlas_clear(&d->paint.atlas);
        for (int i = 0; i < 4; ++i) free(d->paint.ranges[i]);
        free(d->paint.vertices);
    }
    /* 交换链/表面必须在窗口销毁之前释放。 */
    if (d->vk) bongo_cat_vk_destroy(d->vk);
#ifdef _WIN32
    /* 同理, 呈现器也要在窗口销毁之前拆: 代理窗口是菜单窗口的 owned window, 会跟着
       owner 一起消失, 之后就没人能恢复真实窗口的样式与属性了。 */
    if (d->layered) bongo_cat_windows_layered_destroy(d->layered);
#endif
    if (d->context) SDL_GL_DestroyContext(d->context);
    if (d->window_id && SDL_GetWindowFromID(d->window_id)) SDL_DestroyWindow(d->window);
    if (d->previous_window && d->previous_context)
        SDL_GL_MakeCurrent(d->previous_window,d->previous_context);
    if (labels->restore) labels->restore(labels->preview_userdata,d->result);
    BongoCatMenuAction result = d->result;
    free(d);
    /* Trim only after GL/CPU teardown and preview restoration have finished. */
    bongo_cat_platform_trim_memory();
    return result;
}

BongoCatMenuAction bongo_cat_platform_context_menu(BongoCatPlatform *platform,
    const BongoCatMenuLabels *labels) {
    if (!platform || !platform->window || !labels) return BONGO_CAT_MENU_NONE;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,"Radial menu requested: owner=%u",
        (unsigned)SDL_GetWindowID(platform->window));
    bool retry_with_opengl = false;
    BongoCatMenuAction result = menu_run(platform, labels, false, &retry_with_opengl);
    /*
     * Vulkan 这条窗口用不了: 换成 GL 重开一次。失败就以 NONE 返回 (调用方按"菜单被
     * 取消"处理, 不会卡住)。只重试一次, 避免两个后端都坏时来回重试。
     */
    if (retry_with_opengl) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Radial menu falling back to OpenGL after a Vulkan failure");
        result = menu_run(platform, labels, true, NULL);
    }
    return result;
}
