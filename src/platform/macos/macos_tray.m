#include "bongo_cat/platform.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>

/* SDL 3.2's pinned Cocoa tray layout; kept local to the tray adapter. */
typedef struct BongoCatSDLTrayMenu { NSMenu *menu; } BongoCatSDLTrayMenu;
typedef struct BongoCatSDLTray {
    NSStatusBar *status_bar;
    NSStatusItem *status_item;
    BongoCatSDLTrayMenu *menu;
} BongoCatSDLTray;

/*
 * 托盘(状态栏图标)交互的适配层。
 *
 * 上面两个结构体是 SDL 3.2 内部布局的本地副本 —— 靠它把 NSStatusItem 与 NSMenu 取出来,
 * 再挂上自己的 target/action, 因为 SDL 不暴露这两个对象。升级 SDL 时必须重新对照它的
 * 实现, 否则这里会按错误的内存布局读指针。
 * 左键抬起 = 普通点击(回调给上层); 其它情况(右键或按住)= 弹菜单。弹之前先起一个 60Hz
 * 定时器让猫继续出帧 —— NSMenu 的弹出是模态循环, 不主动泵帧画面就会停住; 菜单关闭后
 * 立刻 invalidate。
 */
@interface BongoCatTrayTarget : NSObject {
    NSStatusItem *item_;
    NSMenu *menu_;
    BongoCatTrayClick callback_;
    BongoCatModalTick modal_tick_;
    void *userdata_;
}
- (id)initWithItem:(NSStatusItem *)item menu:(NSMenu *)menu
    callback:(BongoCatTrayClick)callback modalTick:(BongoCatModalTick)modalTick
    userdata:(void *)userdata;
- (void)clicked:(id)sender;
- (void)tickModal:(NSTimer *)timer;
- (void)unbind;
@end

@implementation BongoCatTrayTarget
- (id)initWithItem:(NSStatusItem *)item menu:(NSMenu *)menu
    callback:(BongoCatTrayClick)callback modalTick:(BongoCatModalTick)modalTick
    userdata:(void *)userdata {
    self = [super init];
    if (self) {
        item_ = item;
        menu_ = menu;
        callback_ = callback;
        modal_tick_ = modalTick;
        userdata_ = userdata;
    }
    return self;
}

- (void)clicked:(id)sender {
    (void)sender;
    NSEvent *event = [NSApp currentEvent];
    if ([event type] == NSEventTypeLeftMouseUp) callback_(userdata_);
    else {
        if (modal_tick_) modal_tick_(userdata_);
        NSTimer *timer = modal_tick_ ?
            [NSTimer timerWithTimeInterval:1.0 / 60.0 target:self
                selector:@selector(tickModal:) userInfo:nil repeats:YES] : nil;
        if (timer)
            [[NSRunLoop currentRunLoop] addTimer:timer
                forMode:NSRunLoopCommonModes];
        [NSMenu popUpContextMenu:menu_ withEvent:event forView:[item_ button]];
        [timer invalidate];
    }
}

- (void)tickModal:(NSTimer *)timer {
    (void)timer;
    if (modal_tick_) modal_tick_(userdata_);
}

/* 解绑: 摘掉 target/action 并把菜单还原给状态项, 这样 SDL 之后自己处理点击时不会再打到
   已经释放的对象上。 */
- (void)unbind {
    [[item_ button] setTarget:nil];
    [[item_ button] setAction:nil];
    [item_ setMenu:menu_];
}
@end

static BongoCatTrayTarget *tray_target;

/* 重新绑定: 先拆掉旧 target(这里没有 ARC, 必须手动 release), 只有在"托盘、左键回调、
   状态项、菜单"都存在时才新建 —— 缺任何一个都只是清空旧绑定, 不做半套绑定。 */
void bongo_cat_platform_set_tray_callbacks(void *tray,
    BongoCatTrayClick left_click, BongoCatModalTick modal_tick,
    BongoCatTrayRestore restore, void *userdata) {
    (void)restore;
    BongoCatSDLTray *native = tray;
    if (tray_target) {
        [tray_target unbind];
        [tray_target release];
        tray_target = nil;
    }
    if (!native || !left_click || !native->status_item || !native->menu)
        return;
    tray_target = [[BongoCatTrayTarget alloc] initWithItem:native->status_item
        menu:native->menu->menu callback:left_click modalTick:modal_tick
        userdata:userdata];
    [native->status_item setMenu:nil];
    [[native->status_item button] setTarget:tray_target];
    [[native->status_item button] setAction:@selector(clicked:)];
    [[native->status_item button] sendActionOn:NSEventMaskLeftMouseUp |
        NSEventMaskRightMouseUp];
}
#endif
