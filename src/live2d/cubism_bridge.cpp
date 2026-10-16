#include "bongo_cat/file.h"
#include "bongo_cat/render.h"
#include "bongo_cat/model.h"
#if defined(CSM_TARGET_WIN_GL) || defined(CSM_TARGET_LINUX_GL) || defined(CSM_TARGET_MAC_GL)
#include <GL/glew.h>
#endif
#include "cubism_runtime.hpp"

#include <CubismFramework.hpp>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>
#include <cstdio>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif
using Csm::CubismFramework;

namespace {
class Allocator final : public Csm::ICubismAllocator {
public:
    void *Allocate(const Csm::csmSizeType size) override { return std::malloc(size); }
    void Deallocate(void *memory) override { std::free(memory); }
    void *AllocateAligned(const Csm::csmSizeType size, const Csm::csmUint32 alignment) override {
#ifdef _WIN32
        return _aligned_malloc(size, alignment);
#else
        void *memory = nullptr;
        return posix_memalign(&memory, alignment, size) == 0 ? memory : nullptr;
#endif
    }
    void DeallocateAligned(void *memory) override {
#ifdef _WIN32
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }
};

/*
 * 框架是进程级单例 (全局分配器 + Id 管理器), 但运行时对象可以有多个。所以用引用计数:
 * 第一个 create 时 StartUp, 最后一个 destroy 时才 CleanUp —— 中间任何时候提前收摊都会把
 * 还活着的模型一起带走。resource_root 是进程级的一份: 它只用来兜底找相对路径的资源。
 */
Allocator allocator;
CubismFramework::Option framework_option;
std::string resource_root;
int runtime_count;

void log_message(const char *message) {
    if (message) std::fprintf(stderr, "[Cubism] %s\n", message);
}

/*
 * exe 所在目录。SDK 一次启动要读几百上千个着色器文件, 每次都去问一遍系统没必要 ——
 * 进程运行期间这个值不会变, 所以只取一次 (SDL3 的返回值由 SDL 自己持有, 不要释放)。
 * 注意它带结尾的分隔符, 拼接相对路径时不要再补。
 */
const std::string &executable_directory() {
    static const std::string value = [] {
        const char *path = SDL_GetBasePath();
        return std::string(path ? path : "");
    }();
    return value;
}

/* 资源查找顺序: 已解析的 assets 根目录 -> exe 目录 -> 当前工作目录。
 *
 * SDK 传进来的都是相对路径 ("FrameworkShaders/VertShaderSrc.vert" 之类), 而发布形态
 * 可能是"用户只复制了一个 exe 到桌面/任意目录": 那时 exe 旁边和工作目录里都没有着色器,
 * 唯一可靠的一份在从 pak 解出来的 assets 根目录里 (见 cmake/CubismAssets.cmake 与
 * src/runtime/lifecycle/assets.c), 所以它必须排第一。
 *
 * 工作目录留作最后的兜底, 只为开发期直接从源码树/构建目录跑时仍能找到 FrameworkShaders;
 * 发布版从任意目录启动都不应该依赖它 —— 反过来把它排在最前, 还会让启动目录里另一份
 * 版本不匹配的着色器把正确的那份盖掉。
 */
Csm::csmByte *load_file(const std::string path, Csm::csmSizeInt *size) {
    if (size) *size = 0;
    FILE *file = nullptr;
    if (!resource_root.empty())
        file = bongo_cat_file_open((resource_root + "/" + path).c_str(), "rb");
    const std::string &base = executable_directory();
    if (!file && !base.empty())
        file = bongo_cat_file_open((base + path).c_str(), "rb");
    if (!file) file = bongo_cat_file_open(path.c_str(), "rb");
    if (!file) {
        /* 只记一次: 缺着色器时 SDK 会对每套着色器各要一遍 (近千次), 每次一条日志
         * 会把日志刷满, 而"在哪几个根目录下找过"才是排查时要看的信息。 */
        static bool reported = false;
        if (!reported) {
            reported = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[live2d] 找不到资源 %s: 已查找 assets 根=%s / exe 目录=%s / 工作目录",
                path.c_str(), resource_root.empty() ? "<未设置>" : resource_root.c_str(),
                base.empty() ? "<未知>" : base.c_str());
        }
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    long length = std::ftell(file);
    std::rewind(file);
    if (length <= 0) { std::fclose(file); return nullptr; }
    auto *bytes = static_cast<Csm::csmByte *>(std::malloc((size_t)length));
    if (!bytes || std::fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        std::free(bytes);
        bytes = nullptr;
    } else if (size) *size = (Csm::csmSizeInt)length;
    std::fclose(file);
    return bytes;
}

void release_file(Csm::csmByte *bytes) { std::free(bytes); }

bool start_framework(BongoCatError *error) {
    if (runtime_count++) return true;
#if defined(CSM_TARGET_WIN_GL) || defined(CSM_TARGET_LINUX_GL) || defined(CSM_TARGET_MAC_GL)
    /*
     * Vulkan 后端下没有 OpenGL 上下文: glewInit 必然失败, 而这条路径根本不会用到
     * GL。所以先看当前后端, 选中 Vulkan 时跳过整段 GL 初始化与函数检查, 直接进入
     * CubismFramework::StartUp。
     */
    const bool vulkan_backend =
        bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN;
    if (!vulkan_backend) {
    glewExperimental = GL_TRUE;
    GLenum glew_result = glewInit();
    glGetError();
    if (glew_result != GLEW_OK) {
        runtime_count = 0;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "GLEW initialization failed: %s",
            reinterpret_cast<const char *>(glewGetErrorString(glew_result)));
        return false;
    }
    if (!glCreateShader || !glShaderSource || !glCompileShader ||
        !glGetShaderiv || !glCreateProgram || !glGenFramebuffers ||
        !glGenBuffers || !glBindBuffer || !glBufferData || !glDeleteBuffers ||
        !glGenVertexArrays || !glBindVertexArray || !glDeleteVertexArrays ||
        !glEnableVertexAttribArray || !glVertexAttribPointer) {
        runtime_count = 0;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Required OpenGL 3.3 functions are unavailable");
        return false;
    }
    } /* !vulkan_backend */
#endif
    framework_option = CubismFramework::Option{};
    framework_option.LogFunction = log_message;
    framework_option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;
    framework_option.LoadFileFunction = load_file;
    framework_option.ReleaseBytesFunction = release_file;
    if (!CubismFramework::StartUp(&allocator, &framework_option)) {
        runtime_count = 0;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM, "Cubism Framework startup failed");
        return false;
    }
    CubismFramework::Initialize();
    return true;
}

/*
 * 收摊。只 Dispose, **不** CleanUp —— 这是本文件里唯一一处"反直觉但必须如此"的地方。
 *
 * CubismFramework::CleanUp() 会把框架内部的 s_allocator 置成 NULL
 * (SDK 的 CubismFramework.cpp:100), 而 SDK 里还有一批**静态对象**要到 DLL 分离 (或进程退出)
 * 才析构, 它们的析构函数要经 s_allocator 释放自己持有的内存。两件事撞在一起就是空指针解
 * 引用: 实测崩在 bongo_live2d.dll+0x1e85b 的 `mov rcx,[s_allocator] / mov rax,[rcx]` 上,
 * Vulkan 路径下退出码 0xC0000005 (GL 路径正常)。
 *
 * 为什么只有 Vulkan 中招: GL 后端的 CubismRenderer_OpenGLES2::DoStaticRelease() 会在
 * Dispose() 内部就把静态着色器单例删掉(CubismShader_OpenGLES2::DeleteInstance()), 于是没有
 * 静态对象活到分离阶段; 而 CubismRenderer_Vulkan::DoStaticRelease() 是**空实现**
 * (SDK 的 CubismRenderer_Vulkan.cpp:904), 那些静态资源一直留到 DLL 分离 —— 那时分配器已经
 * 被 CleanUp 清空了。
 *
 * 不调 CleanUp 的代价只是"框架的分配器与选项指针保持有效", 而它们指向的就是本文件里的静态
 * 对象(allocator / framework_option), 生命周期本来就覆盖整个 DLL —— 这正是我们想要的效果:
 * 让最后一个静态析构仍然拿得到一个有效的分配器。
 *
 * Dispose() 之后补一次 Initialize(): Dispose 删掉了 Id 管理器, 但 SDK 只由 CleanUp 负责把那个
 * 指针置空, 跳过 CleanUp 会留下悬空指针。重新 Initialize 会建一个新的, 于是框架在分离阶段
 * 处于"完整可用"而不是"半拆"状态 —— 之后若要再建实例也能直接复用, 不会读到悬空指针。
 */
void stop_framework() {
    if (--runtime_count > 0) return;
    CubismFramework::Dispose();
    CubismFramework::Initialize();
    runtime_count = 0;
}

} // namespace

static BongoCatLive2D *create_runtime(const char *asset_root,
    BongoCatError *error) {
    resource_root = asset_root ? asset_root : "";
    if (!start_framework(error)) return nullptr;
    BongoCatLive2D *runtime = new(std::nothrow) BongoCatLive2D{};
    if (!runtime) {
        stop_framework();
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Cannot allocate Cubism runtime");
    }
    return runtime;
}

extern "C" BongoCatLive2D *bongo_cat_live2d_create(const char *asset_root,
    BongoCatError *error) {
    return create_runtime(asset_root, error);
}

/* 释放顺序不能动: 退休队列 -> 当前模型 -> 运行时对象 -> 停框架。模型必须在框架还活着的
 * 时候删 —— 它们析构时会调 SDK 的分配器和 GL 释放路径, 先 CleanUp 就是 use-after-free。 */
extern "C" void bongo_cat_live2d_destroy(BongoCatLive2D *runtime) {
    if (!runtime) return;
    if (runtime->retired_count) SDL_Log("[runtime] Live2D resource handoff: "
        "stage=retirement-flush queue=%u current_context=%p",
        runtime->retired_count, (void *)SDL_GL_GetCurrentContext());
    for (unsigned i = 0; i < runtime->retired_count; ++i)
        delete runtime->retired[i].model;
    delete runtime->model;
    delete runtime;
    stop_framework();
}

extern "C" bool bongo_cat_live2d_ready(const BongoCatLive2D *runtime) {
    return runtime && runtime->model;
}

extern "C" bool bongo_cat_live2d_canvas_size(const BongoCatLive2D *runtime,
    int *width, int *height) {
    return runtime && runtime->model &&
        runtime->model->canvas_size(width, height);
}

extern "C" bool bongo_cat_live2d_frame(const BongoCatLive2D *runtime,
    BongoCatLive2DFrame *frame) {
    return runtime && runtime->model && runtime->model->frame(frame);
}

extern "C" bool bongo_cat_live2d_viewport(const BongoCatLive2D *runtime,
    int *x, int *y, int *width, int *height) {
    return runtime && runtime->model &&
        runtime->model->viewport(x, y, width, height);
}

/* resize 会把新尺寸推给 SDK 的重绘目标, reshape 只更新画布与视口; 两者都先把尺寸记在运行时
 * 对象上, 因为加载新模型时要按这个尺寸 reshape (见 cubism_bridge_load.cpp)。 */
extern "C" void bongo_cat_live2d_resize(BongoCatLive2D *runtime, int width, int height) {
    if (!runtime) return;
    if (width > 0 && height > 0) {
        runtime->width = width;
        runtime->height = height;
    }
    if (runtime->model) runtime->model->resize(width, height);
}
extern "C" void bongo_cat_live2d_reshape(BongoCatLive2D *runtime, int width, int height) {
    if (!runtime) return;
    if (width > 0 && height > 0) {
        runtime->width = width;
        runtime->height = height;
    }
    if (runtime->model) runtime->model->reshape(width, height);
}
/* 返回值是脏标记。特别注意 retired_count > 0 时也必须报"有变化": 退休队列要靠 draw() 推进,
 * 如果新模型是静态的、上层据此跳过重绘, 旧模型的贴图就永远释放不掉。 */
extern "C" bool bongo_cat_live2d_update(BongoCatLive2D *runtime, float elapsed) {
    if (!runtime) return false;
    bool changed = runtime->model && runtime->model->update(elapsed);
    // Finish deferred releases even when the replacement model is static.
    return changed || runtime->retired_count > 0;
}
extern "C" void bongo_cat_live2d_draw(BongoCatLive2D *runtime) {
    if (!runtime) return;
    unsigned keep = 0, released = 0;
    for (unsigned i = 0; i < runtime->retired_count; ++i) {
        BongoCatRetiredModel item = runtime->retired[i];
        if (item.frames_remaining) item.frames_remaining--;
        if (!item.frames_remaining) {
            delete item.model;
            released++;
        } else runtime->retired[keep++] = item;
    }
    runtime->retired_count = keep;
    if (released) SDL_Log("[runtime] Live2D resource handoff: "
        "stage=retirement-complete released=%u queue=%u current_context=%p "
        "gl_error=0x%x", released, keep, (void *)SDL_GL_GetCurrentContext(),
        (unsigned)glGetError());
    if (runtime->model) runtime->model->draw();
}
extern "C" void bongo_cat_live2d_set_mirror(BongoCatLive2D *runtime, bool mirror) {
    if (runtime && runtime->model) runtime->model->set_mirror(mirror); }
extern "C" void bongo_cat_live2d_set_vertical_flip(BongoCatLive2D *runtime,
    bool flipped) {
    if (runtime && runtime->model) runtime->model->set_vertical_flip(flipped); }
extern "C" void bongo_cat_live2d_set_render_options(BongoCatLive2D *runtime,
    const BongoCatLive2DRenderOptions *options) {
    if (runtime && runtime->model && options)
        runtime->model->set_render_options(*options); }
extern "C" void bongo_cat_live2d_set_dragging(BongoCatLive2D *runtime,
    float x, float y) {
    if (runtime && runtime->model) runtime->model->set_dragging(x, y); }
extern "C" void bongo_cat_live2d_set_centered_dragging(BongoCatLive2D *runtime,
    float x, float y) {
    if (runtime && runtime->model) runtime->model->set_dragging(x, y, true); }
extern "C" void bongo_cat_live2d_prepare_viewer_audit(BongoCatLive2D *runtime) {
    if (runtime && runtime->model) runtime->model->prepare_viewer_audit();
}
extern "C" bool bongo_cat_live2d_prepare_cover_capture(
    BongoCatLive2D *runtime) {
    return runtime && runtime->model &&
        runtime->model->prepare_cover_capture();
}
extern "C" bool bongo_cat_live2d_set_parameter(BongoCatLive2D *runtime, const char *id, float value) {
    return runtime && runtime->model && runtime->model->set_parameter(id, value);
}
extern "C" bool bongo_cat_live2d_parameter(BongoCatLive2D *runtime, const char *id,
    BongoCatParameterRange *range) {
    return runtime && runtime->model && range && runtime->model->parameter(id,
        &range->minimum, &range->maximum, &range->value);
}
extern "C" bool bongo_cat_live2d_start_motion(BongoCatLive2D *runtime, const char *group, int index) {
    return runtime && runtime->model && runtime->model->start_motion(group, index);
}
extern "C" bool bongo_cat_live2d_restore_motion_state(BongoCatLive2D *runtime,
    const char *group, int index) {
    return runtime && runtime->model &&
        runtime->model->restore_motion_state(group, index);
}
extern "C" bool bongo_cat_live2d_preview_motion(BongoCatLive2D *runtime, const char *group, int index) {
    return runtime && runtime->model && runtime->model->preview_motion(group, index); }
extern "C" bool bongo_cat_live2d_restore_motion_preview(BongoCatLive2D *runtime) { return runtime && runtime->model && runtime->model->restore_motion_preview(); }
extern "C" bool bongo_cat_live2d_commit_motion_preview(BongoCatLive2D *runtime, const char *group, int index) {
    return runtime && runtime->model && runtime->model->commit_motion_preview(group, index); }
extern "C" bool bongo_cat_live2d_motion_selected(const BongoCatLive2D *runtime, const char *group, int index) { return runtime && runtime->model && runtime->model->motion_selected(group, index); }
extern "C" bool bongo_cat_live2d_motion_persistent(const BongoCatLive2D *runtime,
    const char *group, int index) {
    return runtime && runtime->model &&
        runtime->model->motion_persistent(group, index); }
extern "C" bool bongo_cat_live2d_motion_visible(const BongoCatLive2D *runtime, const char *group, int index) {
    return runtime && runtime->model && runtime->model->motion_visible(group, index); }
extern "C" bool bongo_cat_live2d_motion_same_toggle(
    const BongoCatLive2D *runtime, const char *left_group, int left_index,
    const char *right_group, int right_index) {
    return runtime && runtime->model && runtime->model->motion_same_toggle(
        left_group, left_index, right_group, right_index); }
extern "C" bool bongo_cat_live2d_set_expression(BongoCatLive2D *runtime, int index) {
    return runtime && runtime->model && runtime->model->set_expression(index); }
extern "C" int bongo_cat_live2d_expression(const BongoCatLive2D *runtime) {
    return runtime && runtime->model ? runtime->model->expression() : -1; }
