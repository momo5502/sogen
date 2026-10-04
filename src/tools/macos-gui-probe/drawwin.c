// One SkyLight window with a drawing context: the Stage A gate for the XPC round-trip. Creating the
// context makes CoreGraphics talk to daemons over XPC, and sogen substitutes CGWindowContextCreate
// with a bitmap context over the window's backing store, so CGBitmapContextGetData must return that
// store. Prints both pointers and exits.
//
// Build: clang -arch arm64 -o /tmp/drawwin drawwin.c -framework CoreGraphics \
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
    printf("context %p\n", (void*)context);
    printf("bitmap data %p\n", CGBitmapContextGetData(context));

    SLSFlushWindowContentRegion(cid, window, region);
    SLSOrderWindow(cid, window, 1, 0);

    return context == NULL ? 1 : 0;
}
