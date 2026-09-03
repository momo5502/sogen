// A calculator laid out as real SkyLight windows: one for the body, one per key. Every rectangle on the
// composed desktop is a window the guest asked the window server for, which is the point -- it exercises
// the window path without CoreGraphics, whose first context creation pulls in CoreFoundation's
// preferences round-trip and blocks on a daemon sogen does not run.
//
// Build: clang -arch arm64 -o calcdemo calcdemo.c -F/System/Library/PrivateFrameworks -framework SkyLight
// Run:   analyzer --os=macos --gui --screenshot calc.png --desktop-size 420x520 ./calcdemo

#include <stdint.h>
#include <stdio.h>

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

extern CGSConnectionID SLSMainConnectionID(void);
extern int CGSNewRegionWithRect(const CGSRect* rect, CGSRegionRef* region);
extern int SLSNewWindow(CGSConnectionID cid, int type, float x, float y, CGSRegionRef region, CGSWindowID* window);
extern int SLSSetWindowLevel(CGSConnectionID cid, CGSWindowID window, int level);
extern int SLSSetWindowOpacity(CGSConnectionID cid, CGSWindowID window, int opaque);
extern int SLSOrderWindow(CGSConnectionID cid, CGSWindowID window, int mode, CGSWindowID relative);
extern int SLSFlushWindowContentRegion(CGSConnectionID cid, CGSWindowID window, CGSRegionRef region);
extern int SLSGetWindowBounds(CGSConnectionID cid, CGSWindowID window, CGSRect* bounds);

static CGSWindowID make_panel(const CGSConnectionID cid, const float x, const float y, const double width, const double height,
                              const int level)
{
    const CGSRect rect = {0.0, 0.0, width, height};

    CGSRegionRef region = 0;
    if (CGSNewRegionWithRect(&rect, &region) != 0)
    {
        return 0;
    }

    CGSWindowID window = 0;
    if (SLSNewWindow(cid, 2, x, y, region, &window) != 0)
    {
        return 0;
    }

    SLSSetWindowOpacity(cid, window, 1);
    SLSSetWindowLevel(cid, window, level);
    SLSOrderWindow(cid, window, 1, 0);
    SLSFlushWindowContentRegion(cid, window, region);
    return window;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const CGSConnectionID cid = SLSMainConnectionID();
    printf("connection %d\n", cid);

    const CGSWindowID body = make_panel(cid, 20.0f, 20.0f, 380.0, 480.0, 1);
    printf("body window %u\n", body);

    const CGSWindowID display = make_panel(cid, 40.0f, 40.0f, 340.0, 90.0, 2);
    printf("display window %u\n", display);

    // Four rows of four keys, the layout of a pocket calculator. Each is its own window, so the composed
    // desktop shows the keypad as sixteen rectangles rather than as one painted bitmap.
    static const char* const labels[4][4] = {
        {"7", "8", "9", "/"},
        {"4", "5", "6", "*"},
        {"1", "2", "3", "-"},
        {"0", ".", "=", "+"},
    };

    unsigned keys = 0;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            const float x = 40.0f + (float)column * 86.0f;
            const float y = 150.0f + (float)row * 86.0f;

            const CGSWindowID key = make_panel(cid, x, y, 76.0, 76.0, 3);
            if (key != 0)
            {
                ++keys;
            }

            printf("key %-2s -> window %u at %.0f,%.0f\n", labels[row][column], key, (double)x, (double)y);
        }
    }

    CGSRect bounds = {0.0, 0.0, 0.0, 0.0};
    if (SLSGetWindowBounds(cid, body, &bounds) == 0)
    {
        printf("body bounds %.0fx%.0f at %.0f,%.0f\n", bounds.width, bounds.height, bounds.x, bounds.y);
    }

    printf("%u keys laid out\n", keys);
    return 0;
}
