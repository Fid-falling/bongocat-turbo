#include "bongo_cat/platform.h"
#include "macos_internal.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/*
 * macOS 平台层的装配点。
 *
 * 与 Linux 版结构相似(单实例用锁文件 + flock), 但"把已有实例叫到前台"走的是分布式通知
 * (com.bongocat.desktop.show): macOS 上跨进程不能像 X11 那样直接操作别人的窗口。
 * macOS 专属的窗口语义有几点:
 *   - 激活策略设为 Accessory: 程序不出现在 Dock 里, 但仍然可以有窗口和状态栏图标 —— 这是
 *     桌宠应有的形态;
 *   - sharingType = ReadOnly: 允许录屏/截图抓到窗口, 否则默认会被系统排除在采集之外;
 *   - 点击穿透必须同时告诉 SDL(窗口形状)与 NSWindow(ignoresMouseEvents), 原因见下面那段
 *     长注释。
 */
static int instance_lock = -1;
static BongoCatPlatform *active_platform;
static NSObject *instance_observer;
static bool instance_show_pending;

/* 模态循环里驱动单帧回调的小目标对象: NSTimer 需要一个 target 来接收 tick, 而我们手里
   只有 C 函数指针, 所以用一个轻量 Objective-C 对象把两者接起来。 */
@interface BongoCatModalTarget : NSObject { BongoCatModalTick callback_; void *userdata_; }
- (id)initWithCallback:(BongoCatModalTick)callback userdata:(void *)userdata;
- (void)tick:(NSTimer *)timer;
@end
@implementation BongoCatModalTarget
- (id)initWithCallback:(BongoCatModalTick)callback userdata:(void *)userdata {
    self = [super init];
    if (self) { callback_ = callback; userdata_ = userdata; }
    return self;
}
- (void)tick:(NSTimer *)timer { (void)timer; callback_(userdata_); }
@end

static NSWindow *native_window(BongoCatPlatform *platform) {
    return (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
}

/* 采集相关的窗口属性: 激活策略 Accessory(不进 Dock)、sharingType 允许被采集。
   这里不涉及"只在采集软件里显示" —— macOS 需要虚拟显示器之类的额外机制, 见下面
   capture_only_supported 处的注释。 */
static void configure_capture_window(NSWindow *window) {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [window setSharingType:NSWindowSharingReadOnly];
}

/* 第二个实例请求"显示我"时调用。若平台还没初始化(active_platform 为空)就先记一个待处理
   标志, 等 init 结尾补一次 —— 通知可能比窗口创建更早到达。 */
static void show_instance(void) {
    if (!active_platform) { instance_show_pending = true; return; }
    NSWindow *window = native_window(active_platform); configure_capture_window(window);
    [window orderFrontRegardless];
    [NSApp activateIgnoringOtherApps:YES];
    instance_show_pending = false;
}

/* 分布式通知的接收者。回调可能在任意线程到达, 而 AppKit 的操作必须在主线程, 所以先切回
   主线程再处理。 */
@interface BongoCatInstanceObserver : NSObject
- (void)showWindow:(NSNotification *)notification;
@end
@implementation BongoCatInstanceObserver
- (void)showWindow:(NSNotification *)notification {
    if (![NSThread isMainThread]) {
        [self performSelectorOnMainThread:@selector(showWindow:)
            withObject:notification waitUntilDone:NO];
        return;
    }
    show_instance();
}
@end

/* 只注册一次即可。suspensionBehavior 用 DeliverImmediately: 程序可能正处于非活跃状态,
   默认策略会把通知挂起, 那样"再点一次图标"就没反应了。 */
static void observe_instance(void) {
    if (instance_observer) return;
    instance_observer = [[BongoCatInstanceObserver alloc] init];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:instance_observer
        selector:@selector(showWindow:) name:@"com.bongocat.desktop.show" object:nil
        suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
}
/* 初始化: 预留一个 SDL 用户事件给输入线程唤醒(拿不到就直接失败, 否则输入线程无法安全
   退出), 设好采集窗口属性, 再启动输入采集 —— 输入失败只记警告, 没有全局输入时程序仍然
   能跑。 */
BongoCatResult bongo_cat_platform_init(BongoCatPlatform *platform, SDL_Window *window,
    BongoCatInputState *input, BongoCatError *error) {
    memset(platform, 0, sizeof(*platform));
    platform->window = window;
    platform->input = input;
    platform->window_opacity = 1.0f;
    platform->wake_event_type = SDL_RegisterEvents(1);
    if (platform->wake_event_type == (Uint32)-1) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot reserve the macOS input wake event");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    active_platform = platform;
    configure_capture_window(native_window(platform));
    BongoCatError input_error = {0};
    if (!bongo_cat_macos_input_start(platform, &input_error))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", input_error.message);
    if (instance_show_pending) show_instance();
    return BONGO_CAT_OK;
}
/* 关闭: 停掉输入线程并清空 active_platform —— 此后 show_instance 会走"待处理"分支, 而
   不是去访问已经失效的窗口。 */
void bongo_cat_platform_shutdown(BongoCatPlatform *platform) {
    bongo_cat_macos_input_stop(platform);
    if (active_platform == platform) active_platform = NULL;
}
/* 点击穿透必须同时告诉两边: SDL(窗口形状)与 NSWindow(ignoresMouseEvents)。为什么两边都
   要说, 见下面原有的英文注释 —— 简单说: SDL 会在鼠标移动时按形状重新写
   ignoresMouseEvents, 只设 NSWindow 会被它覆盖回去。
   这里用一个 1x1 的表面充当"全透明 / 全不透明"的形状: SDL 要的是表面, 不是一个布尔标志。 */
/* SDL rewrites NSWindow.ignoresMouseEvents from the window shape whenever a mouse move reaches the
   window, and derives "ignore the mouse" from a transparent shape pixel. Telling SDL the same state
   through that shape keeps the two from disagreeing, which would otherwise hand the pet back to the
   window server on the next move. The native property is written afterwards: SDL's answer depends on
   where the pointer is, while this state has to hold wherever the pointer is. */
static void apply_click_through_shape(BongoCatPlatform *platform, bool enabled) {
    if (!(SDL_GetWindowFlags(platform->window) & SDL_WINDOW_TRANSPARENT)) return;
    SDL_Surface *shape = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_ARGB32);
    if (!shape) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Click-through window has no shape SDL can follow: %s", SDL_GetError());
        return;
    }
    if (!SDL_WriteSurfacePixel(shape, 0, 0, 0, 0, 0,
            enabled ? SDL_ALPHA_TRANSPARENT : SDL_ALPHA_OPAQUE) ||
        !SDL_SetWindowShape(platform->window, shape))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Click-through state is not described to SDL: %s", SDL_GetError());
    SDL_DestroySurface(shape);
}

/* 与 Linux 一样只有整窗穿透(没有逐像素判断), 所以 forced 与 pointer_transparent 在这里
   合并处理。 */
void bongo_cat_platform_set_click_through(BongoCatPlatform *platform,
    bool forced, bool pointer_transparent) {
    if (!platform || !platform->window) return;
    bool enabled = forced || pointer_transparent;
    apply_click_through_shape(platform, enabled);
    [native_window(platform) setIgnoresMouseEvents:enabled];
}
bool bongo_cat_platform_set_opacity(BongoCatPlatform *platform, float opacity) {
    if (!platform || !platform->window) return false;
    opacity = SDL_clamp(opacity, 0.0f, 1.0f);
    if (opacity == platform->window_opacity) return true;
    if (!SDL_SetWindowOpacity(platform->window, opacity)) return false;
    platform->window_opacity = opacity;
    return true;
}
float bongo_cat_platform_get_opacity(const BongoCatPlatform *platform) {
    return platform ? platform->window_opacity : 1.0f;
}
bool bongo_cat_platform_present(BongoCatPlatform *platform, int width, int height) {
    (void)width; (void)height;
    return platform && platform->window && SDL_GL_SwapWindow(platform->window);
}
bool bongo_cat_platform_frame_alpha(const BongoCatPlatform *platform,
    int width, int height, int x, int y, uint8_t *alpha) {
    (void)platform; (void)width; (void)height; (void)x; (void)y; (void)alpha;
    return false;
}
/* 显示 / 隐藏。重新显示时要重设一遍采集属性: 窗口重新上屏后系统可能把 sharingType 之类
   的属性恢复成默认值。 */
void bongo_cat_platform_set_visible(BongoCatPlatform *platform, bool visible) {
    if (!platform || !platform->window) return;
    visible ? SDL_ShowWindow(platform->window) : SDL_HideWindow(platform->window); if (visible) configure_capture_window(native_window(platform));
}
/* 只在采集软件里显示: macOS 需要虚拟显示器之类的额外机制, 暂不支持 ——
   设置界面会隐藏这一项。 */
bool bongo_cat_platform_capture_only_supported(void) { return false; }
bool bongo_cat_platform_set_capture_only(BongoCatPlatform *platform,
    bool enabled) {
    (void)platform; (void)enabled;
    return false;
}
bool bongo_cat_platform_pointer_local(BongoCatPlatform *platform, double screen_x,
    double screen_y, float *local_x, float *local_y) {
    int x, y, width, height;
    if (!platform || !local_x || !local_y ||
        !SDL_GetWindowPosition(platform->window, &x, &y) ||
        !SDL_GetWindowSize(platform->window, &width, &height)) return false;
    *local_x = (float)(screen_x - x); *local_y = (float)(screen_y - y);
    return *local_x >= 0 && *local_x < width && *local_y >= 0 && *local_y < height;
}
bool bongo_cat_platform_pointer_locked(BongoCatPlatform *platform) {
    (void)platform;
    return false;
}
bool bongo_cat_platform_relative_pointer(BongoCatPlatform *platform,
    double *x, double *y) {
    (void)platform; (void)x; (void)y;
    return false;
}
void bongo_cat_platform_relative_pointer_reset(BongoCatPlatform *platform) {
    (void)platform;
}
void bongo_cat_platform_relative_pointer_release(BongoCatPlatform *platform) {
    (void)platform;
}
/* 置顶靠窗口层级: NSFloatingWindowLevel 会浮在普通窗口之上, 取消时回到 Normal 层级。 */
void bongo_cat_platform_set_always_on_top(BongoCatPlatform *platform, bool enabled) {
    [native_window(platform) setLevel:enabled ? NSFloatingWindowLevel : NSNormalWindowLevel];
}
void bongo_cat_platform_raise_window(SDL_Window *window) {
    if (!window) return;
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    NSWindow *native = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    configure_capture_window(native);
    [NSApp activateIgnoringOtherApps:YES];
    [native makeKeyAndOrderFront:nil];
}

bool bongo_cat_platform_set_geometry(BongoCatPlatform *platform,
    int x, int y, int width, int height) {
    if (!platform || !platform->window) return false;
    int current_width, current_height;
    if ((!SDL_GetWindowSize(platform->window, &current_width, &current_height) ||
        current_width != width || current_height != height) &&
        !SDL_SetWindowSize(platform->window, width, height)) return false;
    int current_x, current_y;
    if (!SDL_GetWindowPosition(platform->window, &current_x, &current_y) ||
        current_x != x || current_y != y)
        SDL_SetWindowPosition(platform->window, x, y);
    return true;
}
/* 拖动: performWindowDragWithEvent 是 macOS 自带的窗口拖动, 它同样会进入模态循环, 所以
   和托盘菜单一样要临时挂一个 60Hz 定时器让猫继续出帧(拖动前后各补一次回调)。 */
void bongo_cat_platform_begin_drag(BongoCatPlatform *platform,
    BongoCatModalTick modal_tick, void *userdata) {
    NSWindow *window = native_window(platform);
    BongoCatModalTarget *target = modal_tick ? [[BongoCatModalTarget alloc]
        initWithCallback:modal_tick userdata:userdata] : nil;
    if (modal_tick) modal_tick(userdata);
    NSTimer *timer = target ? [NSTimer timerWithTimeInterval:1.0 / 60.0
        target:target selector:@selector(tick:) userInfo:nil repeats:YES] : nil;
    if (timer) [[NSRunLoop currentRunLoop] addTimer:timer
        forMode:NSRunLoopCommonModes];
    [window performWindowDragWithEvent:[NSApp currentEvent]];
    [timer invalidate];
    if (modal_tick) modal_tick(userdata);
    [target release];
}
/* 逐像素命中是否可用取决于输入监控授权: 没有授权就拿不到全局鼠标位置, 也就无法按像素
   判断鼠标是否落在猫身上。 */
bool bongo_cat_platform_dynamic_hit_supported(void) {
    return bongo_cat_macos_input_supported();
}

bool bongo_cat_platform_input_monitoring_authorized(void) {
    return bongo_cat_macos_input_monitoring_authorized();
}

bool bongo_cat_platform_input_monitoring_request(void) {
    return bongo_cat_macos_input_monitoring_request();
}

bool bongo_cat_platform_open_directory(const char *path) {
    if (!path || !path[0]) return false;
    @autoreleasepool {
        NSString *directory = [NSString stringWithUTF8String:path];
        return directory && [[NSWorkspace sharedWorkspace]
            openURL:[NSURL fileURLWithPath:directory isDirectory:YES]];
    }
}

/* 单实例: 锁文件 + flock(与 Linux 相同的理由 —— 进程退出时由内核自动释放)。拿不到锁就发
   一条分布式通知请已有实例显示自己, 然后返回 false。 */
bool bongo_cat_platform_single_instance_begin(void) {
    char path[96]; snprintf(path, sizeof(path),
        "/tmp/" BONGO_CAT_SLUG "-%lu.lock",
        (unsigned long)getuid());
    instance_lock = open(path, O_CREAT | O_RDWR, 0600);
    if (instance_lock < 0 || flock(instance_lock, LOCK_EX | LOCK_NB) == 0) {
        observe_instance(); return true;
    }
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:@"com.bongocat.desktop.show" object:nil
        userInfo:nil deliverImmediately:YES];
    close(instance_lock); instance_lock = -1; return false;
}
bool bongo_cat_platform_single_instance_take_wake(void) { return false; }
void bongo_cat_platform_single_instance_end(void) {
    if (instance_lock >= 0) close(instance_lock);
    instance_lock = -1;
    if (instance_observer) {
        [[NSDistributedNotificationCenter defaultCenter] removeObserver:instance_observer];
        [instance_observer release]; instance_observer = nil;
    }
}
/* macOS 侧没有内嵌资源(资源在 app bundle 里), 恒定失败, 由调用方走普通目录逻辑。 */
BongoCatResult bongo_cat_platform_embedded_assets(const char *target, BongoCatError *error) {
    (void)target; (void)error; return BONGO_CAT_ERROR_PLATFORM;
}
#endif
