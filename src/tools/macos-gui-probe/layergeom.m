// Ground truth for the layer→root mapping in src/macos-emulator/gui/macos_layer_compositor.cpp.
// Builds synthetic CALayer trees and asks CoreAnimation itself where the child's bounds corners land in
// the root's coordinate space. The printed numbers are pasted into layer_compositor_test.cpp, so the
// emulator's arithmetic is checked against CoreAnimation rather than against a re-derivation of it.
// Build: clang -arch arm64 -O1 -fobjc-arc -o layergeom layergeom.m -framework Foundation -framework QuartzCore
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

static void report(const char* name, CALayer* root, CALayer* child)
{
    const CGRect b = child.bounds;
    const CGPoint corners[4] = {
        {CGRectGetMinX(b), CGRectGetMinY(b)},
        {CGRectGetMaxX(b), CGRectGetMinY(b)},
        {CGRectGetMinX(b), CGRectGetMaxY(b)},
        {CGRectGetMaxX(b), CGRectGetMaxY(b)},
    };

    printf("%s:", name);
    for (int i = 0; i < 4; ++i)
    {
        const CGPoint p = [child convertPoint:corners[i] toLayer:root];
        printf(" (%.4f,%.4f)", p.x, p.y);
    }

    printf("\n");
}

static CALayer* make(CGRect bounds, CGPoint position, CGPoint anchor)
{
    CALayer* layer = [CALayer layer];
    layer.bounds = bounds;
    layer.position = position;
    layer.anchorPoint = anchor;
    return layer;
}

int main(void)
{
    @autoreleasepool
    {
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:child];
            report("anchor00", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0.5, 0.5));
            [root addSublayer:child];
            report("anchor_center", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(-11.5, -4.5, 111, 26.5), CGPointMake(44, 8.75), CGPointMake(0.5, 0.5));
            [root addSublayer:child];
            report("bounds_origin", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            child.affineTransform = CGAffineTransformMakeScale(2.0, 3.0);
            [root addSublayer:child];
            report("scale_anchor00", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0.5, 0.5));
            child.affineTransform = CGAffineTransformMakeScale(2.0, 3.0);
            [root addSublayer:child];
            report("scale_center", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(200, 100), CGPointMake(0.5, 0.5));
            child.affineTransform = CGAffineTransformMakeRotation(M_PI / 6.0);
            [root addSublayer:child];
            report("rotate30_center", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            child.affineTransform = CGAffineTransformMake(1.5, 0.25, -0.5, 2.0, 7.0, -3.0);
            [root addSublayer:child];
            report("full_affine", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            root.geometryFlipped = YES;
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:child];
            report("gflip_parent", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            root.geometryFlipped = YES;
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0.5, 0.5));
            child.affineTransform = CGAffineTransformMakeScale(2.0, 3.0);
            [root addSublayer:child];
            report("gflip_scale_center", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* mid = make(CGRectMake(0, 0, 200, 150), CGPointMake(50, 40), CGPointMake(0, 0));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:mid];
            [mid addSublayer:child];
            report("nested", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* mid = make(CGRectMake(0, 0, 200, 150), CGPointMake(50, 40), CGPointMake(0, 0));
            mid.geometryFlipped = YES;
            mid.affineTransform = CGAffineTransformMakeScale(1.0, 2.0);
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:mid];
            [mid addSublayer:child];
            report("nested_gflip_scale", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* mid = make(CGRectMake(0, 0, 200, 150), CGPointMake(50, 40), CGPointMake(0, 0));
            mid.sublayerTransform = CATransform3DMakeScale(2.0, 0.5, 1.0);
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:mid];
            [mid addSublayer:child];
            report("sublayer_transform", root, child);
        }
        {
            CALayer* root = make(CGRectMake(10, 20, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:child];
            report("root_bounds_origin", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 0, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            root.geometryFlipped = YES;
            CALayer* mid = make(CGRectMake(0, 0, 200, 150), CGPointMake(50, 40), CGPointMake(0, 0));
            mid.geometryFlipped = YES;
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:mid];
            [mid addSublayer:child];
            report("double_gflip", root, child);
        }
        {
            CALayer* root = make(CGRectMake(0, 20, 400, 300), CGPointMake(200, 150), CGPointMake(0.5, 0.5));
            root.geometryFlipped = YES;
            CALayer* child = make(CGRectMake(0, 0, 100, 50), CGPointMake(30, 20), CGPointMake(0, 0));
            [root addSublayer:child];
            report("gflip_bounds_origin", root, child);
        }
    }

    return 0;
}
