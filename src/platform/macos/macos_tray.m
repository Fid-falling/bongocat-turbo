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

- (void)unbind {
    [[item_ button] setTarget:nil];
    [[item_ button] setAction:nil];
    [item_ setMenu:menu_];
}
@end

static BongoCatTrayTarget *tray_target;

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
