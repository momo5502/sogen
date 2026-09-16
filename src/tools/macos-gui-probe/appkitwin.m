#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>

@interface AppKitWinDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppKitWinDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(300, 300, 320, 200)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @"appkitwin";

    NSButton *button = [NSButton buttonWithTitle:@"OK" target:nil action:nil];
    button.frame = NSMakeRect(120, 84, 80, 32);
    [window.contentView addSubview:button];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    printf("APPKITWIN-ORDERED-FRONT window=%ld\n", (long)window.windowNumber);
    fflush(stdout);

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
        BOOL onscreen = NO;
        for (NSDictionary *info in (__bridge NSArray *)list)
        {
            if ([info[(__bridge NSString *)kCGWindowNumber] unsignedLongValue] ==
                (unsigned long)window.windowNumber)
            {
                onscreen = YES;
                printf("APPKITWIN-WINDOWINFO bounds=%s owner=%s layer=%s\n",
                       [[info[(__bridge NSString *)kCGWindowBounds] description] UTF8String],
                       [[info[(__bridge NSString *)kCGWindowOwnerName] description] UTF8String],
                       [[info[(__bridge NSString *)kCGWindowLayer] description] UTF8String]);
            }
        }
        if (list)
            CFRelease(list);
        printf(onscreen ? "APPKITWIN-ONSCREEN\n" : "APPKITWIN-NOT-ONSCREEN\n");
        fflush(stdout);
    });

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        printf("APPKITWIN-DONE\n");
        fflush(stdout);
        exit(0);
    });
}

@end

int main(int argc, const char *argv[])
{
    static AppKitWinDelegate *delegate;
    @autoreleasepool
    {
        NSApplication *app = [NSApplication sharedApplication];
        delegate = [AppKitWinDelegate new];
        app.delegate = delegate;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        [app run];
    }
    return 0;
}
