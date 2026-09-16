#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
static id nil_icon(id self, SEL _cmd) { (void)self; (void)_cmd; return nil; }
int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        Method m = class_getInstanceMethod(objc_getClass("NSAlert"), sel_registerName("icon"));
        printf("NSAlert icon method: %p\n", (void*)m);
        method_setImplementation(m, (IMP)nil_icon);

        NSAlert* a = [[NSAlert alloc] init];
        a.messageText = @"Click 1";
        a.informativeText = @"body";
        [a addButtonWithTitle:@"OK"];
        printf("alert icon now = %p\n", (__bridge void*)a.icon);
        NSWindow* w = a.window;
        printf("alert window frame=%.0fx%.0f\n", w.frame.size.width, w.frame.size.height);
        [w orderFront:nil];
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        [w display];
        [w.contentView displayIfNeeded];
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        printf("survived layout and display with a nil alert icon\n");
        fflush(stdout);
    }
    return 0;
}
