// An on-screen window built from the SkyLight client API, with no AppKit. The reference for which
// exports sogen intercepts and for the order a program calls them in.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

typedef int CGSConnectionID;
typedef uint32_t CGSWindowID;
typedef uint64_t CGSRegionRef;

extern CGSConnectionID SLSMainConnectionID(void);
extern CGError CGSNewRegionWithRect(const CGRect* rect, CGSRegionRef* region);
extern CGError SLSNewWindow(CGSConnectionID cid, int type, float x, float y, CGSRegionRef region, CGSWindowID* window);
extern CGError SLSSetWindowLevel(CGSConnectionID cid, CGSWindowID window, int level);
extern CGError SLSSetWindowOpacity(CGSConnectionID cid, CGSWindowID window, bool opaque);
extern CGError SLSOrderWindow(CGSConnectionID cid, CGSWindowID window, int mode, CGSWindowID relative);
extern CGContextRef CGWindowContextCreate(CGSConnectionID cid, CGSWindowID window, CFDictionaryRef options);
extern CGError SLSFlushWindowContentRegion(CGSConnectionID cid, CGSWindowID window, CGSRegionRef region);

// Not in any SDK header: the private accessor that says which flavour of context CoreGraphics handed
// back. Type 3 is a recording context with no pixels behind it, which is the whole reason sogen
// substitutes one of its own.
extern int CGContextGetType(CGContextRef context);

int main(void)
{
    @autoreleasepool
    {
        const CGSConnectionID cid = SLSMainConnectionID();
        fprintf(stderr, "connection %d\n", cid);

        const CGRect bounds = CGRectMake(0, 0, 300, 180);
        CGSRegionRef region = 0;
        CGSNewRegionWithRect(&bounds, &region);

        CGSWindowID window = 0;
        SLSNewWindow(cid, 2, 200, 200, region, &window);
        fprintf(stderr, "window %u\n", window);

        SLSSetWindowOpacity(cid, window, true);
        SLSSetWindowLevel(cid, window, 3);

        CGContextRef context = CGWindowContextCreate(cid, window, NULL);
        fprintf(stderr, "context %p type %d bitmap data %p\n", (void*)context, (int)CGContextGetType(context),
                CGBitmapContextGetData(context));

        CGContextSetRGBFillColor(context, 1.0, 0.23, 0.19, 1.0);
        CGContextFillRect(context, bounds);
        CGContextFlush(context);

        SLSFlushWindowContentRegion(cid, window, region);
        SLSOrderWindow(cid, window, 1, 0);

        [NSThread sleepForTimeInterval:3.0];
    }

    return 0;
}
