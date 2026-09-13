#import <AppKit/AppKit.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// SkyLight, private. SLSGetNextEventRecord copies up to 0xf8 bytes of the decoded record and returns 0,
// or 1000 when the connection's queue is empty; SLSServerIsConnected reports the byte SLSServerPort sets
// once the bring-up has answered, which is the gate CGSSnarfAndDispatchDatagrams reads before it pulls.
extern int SLSMainConnectionID(void);
extern int SLSGetNextEventRecord(int cid, void *record, unsigned long size);
extern int SLSServerIsConnected(void);

// The subject of the input gate: every stage AppKit puts between a CGSEventRecord and a button
// action prints itself, so a run says exactly how far an injected event travelled.
//
//   clang -arch arm64 -O2 -fobjc-arc -o inputprobe inputprobe.m -framework Foundation -framework AppKit \
//     -F/System/Library/PrivateFrameworks -framework SkyLight

// Draining the connection's own queue takes the events away from AppKit, so this is off unless asked
// for. It answers a different question: whether sogen's datagrams reach the guest's SkyLight at all,
// independently of whether AppKit is wired up to notice them.
static void *raw_poller(void *unused)
{
    (void)unused;
    const int cid = SLSMainConnectionID();
    printf("INPUTPROBE-CONNECTED %d cid=%d\n", SLSServerIsConnected(), cid);
    fflush(stdout);

    unsigned char record[0xf8];
    for (;;)
    {
        if (SLSGetNextEventRecord(cid, record, sizeof(record)) == 0)
        {
            unsigned int type = 0;
            double x = 0;
            double y = 0;
            memcpy(&type, record + 0x08, sizeof(type));
            memcpy(&x, record + 0x10, sizeof(x));
            memcpy(&y, record + 0x18, sizeof(y));
            printf("INPUTPROBE-RAW type=%u global=(%.1f,%.1f)\n", type, x, y);
            fflush(stdout);
        }

        usleep(20000);
    }

    return NULL;
}

@interface ProbeApplication : NSApplication
@end

@implementation ProbeApplication

- (void)sendEvent:(NSEvent *)event
{
    printf("INPUTPROBE-SENDEVENT type=%lu loc=(%.1f,%.1f) window=%ld clicks=%ld keycode=%u\n", (unsigned long)event.type,
           event.locationInWindow.x, event.locationInWindow.y, (long)event.windowNumber,
           (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseUp) ? (long)event.clickCount : 0L,
           (event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp) ? event.keyCode : 0);
    fflush(stdout);
    [super sendEvent:event];
}

@end

@interface ProbeButton : NSButton
@end

@implementation ProbeButton

- (void)mouseDown:(NSEvent *)event
{
    printf("INPUTPROBE-BUTTON-MOUSEDOWN loc=(%.1f,%.1f)\n", event.locationInWindow.x, event.locationInWindow.y);
    fflush(stdout);
    [super mouseDown:event];
}

- (void)keyDown:(NSEvent *)event
{
    printf("INPUTPROBE-BUTTON-KEYDOWN keycode=%u\n", event.keyCode);
    fflush(stdout);
    [super keyDown:event];
}

@end

@interface ProbeDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ProbeDelegate

- (void)fired:(id)sender
{
    (void)sender;
    printf("INPUTPROBE-ACTION\n");
    fflush(stdout);
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;

    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 300, 320, 232)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = @"inputprobe";

    ProbeButton *button = [[ProbeButton alloc] initWithFrame:NSMakeRect(110, 100, 100, 32)];
    button.title = @"Press";
    button.bezelStyle = NSBezelStyleRounded;
    button.target = self;
    button.action = @selector(fired:);
    [window.contentView addSubview:button];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskAny
                                          handler:^NSEvent *(NSEvent *event) {
                                            printf("INPUTPROBE-MONITOR type=%lu loc=(%.1f,%.1f) window=%ld\n", (unsigned long)event.type,
                                                   event.locationInWindow.x, event.locationInWindow.y, (long)event.windowNumber);
                                            fflush(stdout);
                                            return event;
                                          }];

    if (getenv("INPUTPROBE_RAW") != NULL)
    {
        pthread_t poller;
        pthread_create(&poller, NULL, raw_poller, NULL);
        pthread_detach(poller);
    }

    printf("INPUTPROBE-READY window=%ld frame=%s button=%s connected=%d\n", (long)window.windowNumber,
           [NSStringFromRect(window.frame) UTF8String], [NSStringFromRect(button.frame) UTF8String],
           SLSServerIsConnected());
    fflush(stdout);
}

@end

int main(void)
{
    static ProbeDelegate *delegate;
    @autoreleasepool
    {
        NSApplication *app = [ProbeApplication sharedApplication];
        delegate = [ProbeDelegate new];
        app.delegate = delegate;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        [app run];
    }
    return 0;
}
