#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

static void render(const char* what, CGPathRef p, BOOL evenOdd, int w, int h)
{
    CAShapeLayer* l = [CAShapeLayer layer];
    l.bounds = CGRectMake(0,0,w,h);
    l.anchorPoint = CGPointZero;
    l.position = CGPointZero;
    l.path = p;
    l.fillColor = CGColorCreateGenericRGB(1,1,1,1);
    l.fillRule = evenOdd ? kCAFillRuleEvenOdd : kCAFillRuleNonZero;
    l.contentsScale = 1.0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, w*4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    [l renderInContext:ctx];
    const unsigned char* px = CGBitmapContextGetData(ctx);
    printf("=== %s %dx%d %s ===\n", what, w, h, evenOdd ? "even-odd" : "nonzero");
    for (int y = 0; y < h; ++y) {
        printf("  ");
        for (int x = 0; x < w; ++x) printf("%3d ", px[(y*w+x)*4 + 3]);
        printf("\n");
    }
}

int main(void) {
    @autoreleasepool {
        CGMutablePathRef tri = CGPathCreateMutable();
        CGPathMoveToPoint(tri, NULL, 2, 2);
        CGPathAddLineToPoint(tri, NULL, 14, 2);
        CGPathAddLineToPoint(tri, NULL, 2, 14);
        CGPathCloseSubpath(tri);
        render("triangle", tri, NO, 16, 16);

        CGMutablePathRef ring = CGPathCreateMutable();
        CGPathAddRect(ring, NULL, CGRectMake(1,1,14,14));
        CGPathAddRect(ring, NULL, CGRectMake(5,5,6,6));
        render("two nested rects", ring, YES, 16, 16);
        render("two nested rects", ring, NO, 16, 16);

        CGMutablePathRef cur = CGPathCreateMutable();
        CGPathMoveToPoint(cur, NULL, 1, 8);
        CGPathAddCurveToPoint(cur, NULL, 1, 1, 15, 1, 15, 8);
        CGPathCloseSubpath(cur);
        render("cubic cap", cur, NO, 16, 16);
    }
    return 0;
}
