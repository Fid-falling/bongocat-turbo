#include "bongo_cat/platform.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

/*
 * macOS 的窗口外观与开机自启。
 *
 * 自动启动用 LaunchAgent(~/Library/LaunchAgents 下的 plist + RunAtLoad), 参数带
 * --autostart, 让程序知道这次是自启。@autoreleasepool 是必需的: 这个函数可能被非主线程
 * 调用, 没有池子就会持续泄漏 Objective-C 对象。
 * 关闭时"文件本来就不存在"也算成功 —— 目标状态已经达成。
 */
BongoCatResult bongo_cat_platform_set_autostart(bool enabled,
    BongoCatError *error) {
    @autoreleasepool {
        NSString *directory = [NSHomeDirectory()
            stringByAppendingPathComponent:@"Library/LaunchAgents"];
        NSString *path = [directory
            stringByAppendingPathComponent:@"com.bongocat.desktop.plist"];
        NSFileManager *files = [NSFileManager defaultManager];
        if (!enabled) {
            if (![files fileExistsAtPath:path] ||
                [files removeItemAtPath:path error:nil]) return BONGO_CAT_OK;
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot remove macOS launch agent");
            return BONGO_CAT_ERROR_IO;
        }
        NSString *executable = [[NSBundle mainBundle] executablePath];
        if (!executable || ![files createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:nil]) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot create macOS launch agent directory");
            return BONGO_CAT_ERROR_IO;
        }
        NSDictionary *plist = @{ @"Label": @"com.bongocat.desktop",
            @"ProgramArguments": @[executable, @"--autostart"],
            @"RunAtLoad": @YES };
        if ([plist writeToFile:path atomically:YES]) return BONGO_CAT_OK;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot write macOS launch agent");
        return BONGO_CAT_ERROR_IO;
    }
}

/* 设置窗口的外观改造: 加上 FullSizeContentView、隐藏标题并把标题栏设成透明, 于是它看起来
   没有标题栏(与主窗口的无边框风格一致), 但保留关闭/最小化/缩放三个按钮 —— 设置窗口是
   普通窗口, 用户需要这些按钮。
   tabbingMode 设为 Disallowed: 否则 macOS 会把多个窗口合并成带标签页的窗口, 设置窗口会
   被并进主窗口的标签组; movableByWindowBackground 关掉是为了避免拖动内容区时误移窗口。 */
void bongo_cat_platform_configure_preferences_window(SDL_Window *sdl_window) {
    if (!sdl_window) return;
    NSWindow *window = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdl_window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!window) return;
    NSWindowStyleMask style = [window styleMask];
    style |= NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
        NSWindowStyleMaskFullSizeContentView;
    [window setStyleMask:style];
    [window setTitle:@""];
    [window setTitleVisibility:NSWindowTitleHidden];
    [window setTitlebarAppearsTransparent:YES];
    [window setToolbar:nil];
    [window setTabbingMode:NSWindowTabbingModeDisallowed];
    [window setMovableByWindowBackground:NO];
    [[window standardWindowButton:NSWindowCloseButton] setHidden:NO];
    [[window standardWindowButton:NSWindowMiniaturizeButton] setHidden:NO];
    [[window standardWindowButton:NSWindowZoomButton] setHidden:NO];
}
#endif
