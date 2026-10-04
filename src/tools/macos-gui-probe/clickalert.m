#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

// Answers "did my click do anything" with three independent signals, because they travel different
// paths through the emulator and the interesting result is which of them arrive.
//
//   1. stdout      -- no window server involved at all, so it says the event reached AppKit.
//   2. window title -- a window property, synced through the window path rather than redrawn.
//   3. an NSAlert   -- a brand new window, created and ordered in.
//
// A repaint of an existing view is a fourth path and the one known not to work: resolve_parked_frame()
// runs only from park_for_host_input(), which a guest with an armed run-loop timer never reaches. The
// button's own pressed-state highlight is that path, so it is expected to stay static even when the
// click is working -- which is exactly why this probe does not rely on it.
//
// Environment (set with the analyzer's --env NAME VALUE; the host's own environment is not passed on):
//
//   CLICKALERT_ALERT=1             show the NSAlert from the button's action
//   CLICKALERT_ALERT_BLANK_ICON=1  give the alert an icon of its own instead of the application icon
//   CLICKALERT_AUTOCLICK=1         take the action once straight from launch, for a headless run
//
//   clang -arch arm64 -O2 -fobjc-arc -o clickalert clickalert.m -framework Foundation -framework AppKit

@interface ClickDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *label;
@property(nonatomic, assign) NSInteger clicks;
@end

@implementation ClickDelegate

- (void)fired:(id)sender
{
    (void)sender;
    self.clicks += 1;

    printf("CLICKALERT-CLICK %ld\n", (long)self.clicks);
    fflush(stdout);

    self.window.title = [NSString stringWithFormat:@"clicked %ld", (long)self.clicks];
    self.label.stringValue = [NSString stringWithFormat:@"clicked %ld", (long)self.clicks];

    // NSAlert draws the application icon, and under sogen that comes back null and AppKit asserts in
    // NSISIconImageRepGetCGImage rather than degrading -- which kills the process before the other two
    // signals can be read. Off by default so the cheap signals stay usable; CLICKALERT_ALERT=1 asks for
    // it back, and CLICKALERT_ALERT_BLANK_ICON=1 keeps the assert out of the way until the icon path
    // works.
    if (getenv("CLICKALERT_ALERT") == NULL)
    {
        return;
    }

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:@"Click %ld", (long)self.clicks];
    alert.informativeText = @"The button's action fired inside the guest.";
    [alert addButtonWithTitle:@"OK"];

    // The default icon is the application icon, and that one is an NSISIconImageRep: AppKit asks
    // iconservicesagent to render it and asserts on a null result. An NSImage drawn here is a plain
    // bitmap rep with no Icon Services in it, so CLICKALERT_ALERT_BLANK_ICON=1 separates "the alert
    // window itself works" from "the icon lookup works".
    if (getenv("CLICKALERT_ALERT_BLANK_ICON") != NULL)
    {
        alert.icon = [NSImage imageWithSize:NSMakeSize(64, 64)
                                    flipped:NO
                             drawingHandler:^BOOL(NSRect rect) {
                                 [[NSColor systemBlueColor] setFill];
                                 NSRectFill(rect);
                                 return YES;
                             }];
    }

    printf("CLICKALERT-ALERT-BEGIN %ld\n", (long)self.clicks);
    fflush(stdout);

    const NSModalResponse response = [alert runModal];

    printf("CLICKALERT-ALERT-END %ld response=%ld\n", (long)self.clicks, (long)response);
    fflush(stdout);
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 360, 200)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"clickalert";

    self.label = [NSTextField labelWithString:@"not clicked yet"];
    self.label.frame = NSMakeRect(20, 140, 320, 24);
    self.label.alignment = NSTextAlignmentCenter;
    [self.window.contentView addSubview:self.label];

    // Deliberately large: a click that misses the control is indistinguishable from a click that never
    // arrived, and this probe exists to tell those two apart.
    NSButton *button = [[NSButton alloc] initWithFrame:NSMakeRect(40, 40, 280, 72)];
    button.title = @"Click me";
    button.bezelStyle = NSBezelStyleRounded;
    button.target = self;
    button.action = @selector(fired:);
    [self.window.contentView addSubview:button];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    printf("CLICKALERT-READY window=%ld frame=%s button=%s\n", (long)self.window.windowNumber,
           [NSStringFromRect(self.window.frame) UTF8String], [NSStringFromRect(button.frame) UTF8String]);
    fflush(stdout);

    // A headless run cannot click, so CLICKALERT_AUTOCLICK=1 takes the same path straight from launch.
    // It is the only way to exercise the alert under --screenshot. A run-loop-scheduled call is not: a
    // guest whose run loop is parked never reaches it.
    if (getenv("CLICKALERT_AUTOCLICK") != NULL)
    {
        [self fired:nil];
    }
}

@end

int main(void)
{
    static ClickDelegate *delegate;
    @autoreleasepool
    {
        NSApplication *app = [NSApplication sharedApplication];
        delegate = [ClickDelegate new];
        app.delegate = delegate;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        [app run];
    }
    return 0;
}
