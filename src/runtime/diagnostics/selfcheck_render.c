/*
 * 自检第 2 组: 渲染后端 —— 这一组是整份自检的核心。
 *
 * 用户报告的故障 (AMD Radeon 780M + 200% DPI + 要求 Vulkan) 恰好落在"首帧呈现"这一段,
 * 而那段日志几乎为空。所以这里不是"读一个后端名字", 而是真的把两条链路各走一遍:
 *   - Vulkan: 独立隐藏窗口上 设备 -> 交换链 -> 提交 -> 呈现 1 帧 (见 vk_selfcheck.c);
 *   - OpenGL: 独立隐藏窗口 (当前后端是 OpenGL 时直接复用现有上下文) 读 GL 版本/厂商/
 *     渲染器/MSAA。
 * 两条路都只在自检需要时创建, 结束即销毁, 不干扰正在运行的宠物窗口。
 *
 * 结论的用法: Vulkan 首帧探测失败时 report->recommend_opengl 置位 —— 面板会多出一个
 * [切换并重启] 按钮, 那正是这台机器上唯一能立刻自救的动作。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <string.h>

typedef struct GlProbe {
    bool ok;
    char vendor[96];
    char renderer[128];
    char version[96];
    int samples;
    char error[160];
} GlProbe;

static void read_gl_strings(GlProbe *probe) {
    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    snprintf(probe->vendor, sizeof(probe->vendor), "%s",
        vendor ? (const char *)vendor : "unknown");
    snprintf(probe->renderer, sizeof(probe->renderer), "%s",
        renderer ? (const char *)renderer : "unknown");
    snprintf(probe->version, sizeof(probe->version), "%s",
        version ? (const char *)version : "unknown");
    int samples = 0;
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &samples);
    probe->samples = samples;
    probe->ok = renderer != NULL && version != NULL;
    if (!probe->ok)
        snprintf(probe->error, sizeof(probe->error),
            "当前上下文读不到 GL_RENDERER/GL_VERSION");
}

/* 复用宠物窗口的 GL 上下文 (当前后端是 OpenGL 时)。读之前必须切过去, 读完切回来 ——
   调用点是在设置窗口的一帧里, 那个上下文才是这帧接下来的绘制要用的。 */
static bool probe_opengl_existing(BongoCatApp *app, GlProbe *probe) {
    if (!app || !app->window || !app->gl_context) return false;
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        snprintf(probe->error, sizeof(probe->error),
            "无法激活宠物窗口的 OpenGL 上下文: %s", SDL_GetError());
        return true;
    }
    read_gl_strings(probe);
    if (previous_window && previous_context)
        SDL_GL_MakeCurrent(previous_window, previous_context);
    else
        SDL_GL_MakeCurrent(NULL, NULL);
    return true;
}

/* 没有现成 GL 上下文 (当前跑 Vulkan) 时, 用独立的隐藏窗口建一个: 只读版本串, 不做任何
   绘制。属性与宠物窗口同款 (3.3 兼容档), 否则读到的可能是一个和实际渲染完全不同
   的驱动实现。 */
static void probe_opengl_window(GlProbe *probe) {
    static const int samples[] = {4, 0};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
            SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, samples[i] > 0 ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, samples[i]);
        SDL_Window *window = SDL_CreateWindow("BongoCat OpenGL Self-Check",
            64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN |
            SDL_WINDOW_NOT_FOCUSABLE);
        if (!window) {
            snprintf(probe->error, sizeof(probe->error),
                "OpenGL 隐藏窗口创建失败: %s", SDL_GetError());
            continue;
        }
        SDL_GLContext context = SDL_GL_CreateContext(window);
        if (!context) {
            snprintf(probe->error, sizeof(probe->error),
                "OpenGL 上下文创建失败: %s", SDL_GetError());
            SDL_DestroyWindow(window);
            continue;
        }
        if (SDL_GL_MakeCurrent(window, context)) read_gl_strings(probe);
        else
            snprintf(probe->error, sizeof(probe->error),
                "OpenGL 上下文启用失败: %s", SDL_GetError());
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        if (probe->ok) return;
    }
}

static void probe_opengl(BongoCatApp *app, GlProbe *probe) {
    memset(probe, 0, sizeof(*probe));
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_OPENGL &&
        probe_opengl_existing(app, probe))
        return;
    probe_opengl_window(probe);
}

static void present_modes_text(unsigned modes, char *output, size_t capacity) {
    static const struct {
        unsigned bit;
        const char *name;
    } names[] = {
        {1u << 0, "FIFO"}, {1u << 1, "MAILBOX"}, {1u << 2, "IMMEDIATE"},
        {1u << 3, "FIFO_RELAXED"}};
    size_t used = 0;
    output[0] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!(modes & names[i].bit)) continue;
        int written = snprintf(output + used, capacity - used, "%s%s",
            used ? " | " : "", names[i].name);
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
    if (!used) snprintf(output, capacity, "无 (驱动未报告任何呈现模式)");
}

void bongo_cat_selfcheck_render(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    bongo_cat_selfcheck_section(builder, "渲染后端");
    const BongoCatRenderer active = bongo_cat_renderer_active();
    const BongoCatRenderer configured = app ? app->settings.model.renderer :
        BONGO_CAT_RENDERER_AUTO;
    if (configured == BONGO_CAT_RENDERER_VULKAN &&
        active != BONGO_CAT_RENDERER_VULKAN)
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN, "当前后端",
            "配置要求 Vulkan，但本次运行已回退到 %s（详见日志里的"
            "「%s 初始化失败」）",
            bongo_cat_renderer_name(active),
            bongo_cat_renderer_name(configured));
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "当前后端",
            "%s（配置=%s）", bongo_cat_renderer_name(active),
            bongo_cat_renderer_name(configured));
    /*
     * Vulkan 首帧探测。这一次调用同时给出设备信息与呈现结果 —— 失败时它已经把
     * "走到了哪一步、驱动给了什么返回码"写进 error 与各字段里 (见 vk_selfcheck.c)。
     */
    BongoCatVkSelfCheck vk = {0};
    const bool vk_ok = bongo_cat_vk_selfcheck(&vk);
    /*
     * 三条 Vulkan 结论**无论走到哪一步都必须出现** (见 selfcheck.h 的硬约束 3):
     * 设备不可用时不能只写"设备失败"就把后两条省略掉 —— 那样面板上会少两行, 用户
     * 会以为"没检查"。后两项在设备阶段失败时直接写明"未走到", 那本身就是结论。
     */
    if (!vk.compiled) {
        const bool wanted = configured == BONGO_CAT_RENDERER_VULKAN ||
            configured == BONGO_CAT_RENDERER_AUTO;
        bongo_cat_selfcheck_add(builder, wanted ?
            BONGO_CAT_SELFCHECK_FAIL : BONGO_CAT_SELFCHECK_WARN, "Vulkan 设备",
            "本构建未编入 Vulkan（需要 -DBONGO_CAT_ENABLE_VULKAN=ON），无法做首帧"
            "探测；OpenGL 不受影响");
    } else if (!vk.device[0]) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "Vulkan 设备",
            "%s", vk.error[0] ? vk.error : "Vulkan 设备创建失败（原因未记录）");
    } else {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "Vulkan 设备",
            "%s · 驱动 %s · apiVersion %s · 独立实例=%s", vk.device, vk.driver,
            vk.api, vk.shared ? "否（复用主窗口设备）" : "是");
    }
    if (!vk.device[0]) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
            "呈现模式与合成", "未走到（Vulkan 设备不可用）：%s",
            vk.error[0] ? vk.error : "设备创建失败");
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
            "Vulkan 首帧呈现", "未走到（Vulkan 设备不可用），无法创建交换链");
    } else {
        char modes[160];
        present_modes_text(vk.present_modes, modes, sizeof(modes));
        const bool wants_transparent = !SDL_getenv(
            "BONGO_CAT_TEST_DISABLE_PREFERENCES_TRANSPARENCY");
        const bool alpha_opaque = vk.composite_alpha == 1u; /* OPAQUE */
        bongo_cat_selfcheck_add(builder,
            (wants_transparent && alpha_opaque) ? BONGO_CAT_SELFCHECK_WARN :
                BONGO_CAT_SELFCHECK_PASS, "呈现模式与合成",
            "present mode: %s；compositeAlpha: 使用 0x%x（支持 0x%x）%s",
            modes, vk.composite_alpha, vk.composite_supported,
            (wants_transparent && alpha_opaque)
                ? "；驱动不支持逐像素透明合成，宠物背景会是不透明"
                : "");
        if (vk_ok)
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS,
                "Vulkan 首帧呈现",
                "在独立隐藏窗口上创建交换链 %dx%d 并成功呈现 1 帧"
                "（VkResult=%d，耗时 %u ms）",
                vk.width, vk.height, vk.present_result, vk.elapsed_ms);
        else
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
                "Vulkan 首帧呈现",
                "%s（已走到：交换链%s，present=%s）",
                vk.error[0] ? vk.error : "首帧没能在 5 秒内端出去",
                vk.swapchain_created ? "已创建" : "未创建",
                vk.presented ? "成功" : "失败");
    }
    if (!vk_ok) builder->report->recommend_opengl = true;
    GlProbe gl;
    probe_opengl(app, &gl);
    if (gl.ok)
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "OpenGL",
            "版本 %s · 厂商 %s · 渲染器 %s · MSAA %dx", gl.version, gl.vendor,
            gl.renderer, gl.samples);
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "OpenGL",
            "OpenGL 不可用：%s", gl.error[0] ? gl.error : "上下文创建失败");
}
