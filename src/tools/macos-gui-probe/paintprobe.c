// One SkyLight window with real CoreGraphics drawing: the visible-content gate. The drawing runs
// Apple's own CoreGraphics against a bitmap context sogen substituted over the window's backing store,
// so the colored bars in the screenshot are guest-rendered pixels, not emulator paint.
//
// Build: clang -arch arm64 -o /tmp/paintprobe paintprobe.c -framework CoreGraphics \
//          -F/System/Library/PrivateFrameworks -framework SkyLight

#include <stdint.h>
#include <stdio.h>

#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGBitmapContext.h>

typedef int CGSConnectionID;
typedef uint32_t CGSWindowID;
typedef uint64_t CGSRegionRef;

typedef struct
{
    double x;
    double y;
    double width;
    double height;
} CGSRect;

typedef const void* CFTypeRef;

extern CGSConnectionID SLSMainConnectionID(void);
extern int CGSNewRegionWithRect(const CGSRect* rect, CGSRegionRef* region);
extern int SLSNewWindow(CGSConnectionID cid, int type, float x, float y, CGSRegionRef region, CGSWindowID* window);
extern int SLSSetWindowLevel(CGSConnectionID cid, CGSWindowID window, int level);
extern int SLSSetWindowOpacity(CGSConnectionID cid, CGSWindowID window, int opaque);
extern int SLSOrderWindow(CGSConnectionID cid, CGSWindowID window, int mode, CGSWindowID relative);
extern int SLSFlushWindowContentRegion(CGSConnectionID cid, CGSWindowID window, CGSRegionRef region);

extern CGContextRef CGWindowContextCreate(CGSConnectionID cid, CGSWindowID window, CFTypeRef options);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const CGSConnectionID cid = SLSMainConnectionID();
    printf("connection %d\n", cid);

    const CGSRect rect = {0.0, 0.0, 380.0, 480.0};

    CGSRegionRef region = 0;
    if (CGSNewRegionWithRect(&rect, &region) != 0)
    {
        printf("region failed\n");
        return 1;
    }

    CGSWindowID window = 0;
    if (SLSNewWindow(cid, 2, 20.0f, 20.0f, region, &window) != 0)
    {
        printf("window failed\n");
        return 1;
    }
    printf("window %u\n", window);

    SLSSetWindowOpacity(cid, window, 1);
    SLSSetWindowLevel(cid, window, 1);

    CGContextRef context = CGWindowContextCreate(cid, window, NULL);
    if (context == NULL)
    {
        printf("context failed\n");
        return 1;
    }
    printf("context %p\n", (void*)context);
    printf("bitmap data %p\n", CGBitmapContextGetData(context));

    // Eight hue steps across the window, then a white frame around the middle third: an image a black
    // rectangle cannot fake.
    for (int i = 0; i < 8; ++i)
    {
        const double hue = (double)i / 8.0;
        CGContextSetRGBFillColor(context, hue, 0.35 + 0.5 * (1.0 - hue), 1.0 - hue, 1.0);
        CGContextFillRect(context, CGRectMake(20.0 + (double)i * 40.0, 40.0, 36.0, 300.0));
    }

    CGContextSetRGBStrokeColor(context, 1.0, 1.0, 1.0, 1.0);
    CGContextSetLineWidth(context, 6.0);
    CGContextStrokeRect(context, CGRectMake(60.0, 380.0, 260.0, 60.0));

    SLSFlushWindowContentRegion(cid, window, region);
    SLSOrderWindow(cid, window, 1, 0);

    printf("painted\n");
    return 0;
}
