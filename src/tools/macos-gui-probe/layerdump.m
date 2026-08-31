// Ground truth for src/macos-emulator/gui/macos_layer_compositor.cpp: prints the live CALayer tree of a
// real AppKit window with every property the compositor models, so the emulator's geometry arithmetic can
// be checked against what CoreAnimation itself computes. Build and run per README.md.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

static void print_color(const char* label, CGColorRef color)
{
    if (color == NULL)
    {
        printf(" %s=nil", label);
        return;
    }

    const size_t n = CGColorGetNumberOfComponents(color);
    const CGFloat* c = CGColorGetComponents(color);
    printf(" %s=[n=%zu", label, n);
    for (size_t i = 0; i < n; ++i)
    {
        printf(" %.4f", (double)c[i]);
    }
    // Measured offsets the emulator reads instead of calling CoreGraphics: components at +0x48, count at
    // +0x38 (25G76).
    const uint64_t count_at_0x38 = *(const uint64_t*)((const uint8_t*)color + 0x38);
    const void* comps_at_0x48 = (const void*)((const uint8_t*)color + 0x48);
    printf("] raw{+0x38=%llu +0x48==CGColorGetComponents:%d}", (unsigned long long)count_at_0x38, comps_at_0x48 == (const void*)c);
}

static void dump(CALayer* layer, int depth)
{
    for (int i = 0; i < depth; ++i)
    {
        printf("  ");
    }

    const CGRect b = layer.bounds;
    const CGPoint p = layer.position;
    const CGPoint a = layer.anchorPoint;
    const CGAffineTransform t = layer.affineTransform;
    id contents = layer.contents;

    printf("%s %p b=(%.2f,%.2f,%.2f,%.2f) p=(%.2f,%.2f) a=(%.2f,%.2f) t=(%.3f,%.3f,%.3f,%.3f,%.2f,%.2f)"
           " op=%.3f hidden=%d masks=%d gflip=%d cr=%.2f bw=%.2f cs=%.2f z=%.2f grav=%s contents=%s",
           object_getClassName(layer), (__bridge void*)layer, b.origin.x, b.origin.y, b.size.width, b.size.height, p.x, p.y, a.x, a.y,
           t.a, t.b, t.c, t.d, t.tx, t.ty, (double)layer.opacity, (int)layer.hidden, (int)layer.masksToBounds, (int)layer.geometryFlipped,
           (double)layer.cornerRadius, (double)layer.borderWidth, (double)layer.contentsScale, (double)layer.zPosition,
           [layer.contentsGravity UTF8String], contents ? object_getClassName(contents) : "nil");
    print_color("bg", layer.backgroundColor);
    print_color("border", layer.borderColor);
    printf("\n");

    for (CALayer* child in layer.sublayers)
    {
        dump(child, depth + 1);
    }
}

@interface Probe : NSObject <NSApplicationDelegate>
@end

@implementation Probe
- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    (void)note;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 320, 200)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = @"layerdump";
    NSButton* button = [NSButton buttonWithTitle:@"7" target:nil action:nil];
    button.frame = NSMakeRect(40, 40, 80, 32);
    [window.contentView addSubview:button];
    [window makeKeyAndOrderFront:nil];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
      CALayer* layer = window.contentView.layer;
      while (layer.superlayer != nil)
      {
          layer = layer.superlayer;
      }

      printf("LAYERDUMP window frame=(%.1f,%.1f,%.1f,%.1f) backingScale=%.1f\n", window.frame.origin.x, window.frame.origin.y,
             window.frame.size.width, window.frame.size.height, (double)window.backingScaleFactor);
      dump(layer, 0);
      fflush(stdout);
      exit(0);
    });
}
@end

int main(void)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        Probe* probe = [[Probe alloc] init];
        NSApp.delegate = probe;
        [NSApp run];
    }

    return 0;
}
