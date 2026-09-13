// The rendering oracle for CASDFLayer / CASDFElementLayer. Builds one on-screen window holding a grid
// of SDF cases, lets the real WindowServer composite it, reads the frame back with
// CGWindowListCreateImage and prints scanlines and an ASCII coverage map per case. Every number the
// contract quotes for the SDF rasteriser comes from this program's output.
//
// CGWindowListCreateImage is obsoleted in the 15.0 SDK but still exported; capturing one's own window
// needs no screen-recording permission, so it is reached through dlsym rather than a header.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <dlfcn.h>

typedef CGImageRef (*window_list_create_image_fn)(CGRect, uint32_t, CGWindowID, uint32_t);

static const CGFloat kCellW = 120;
static const CGFloat kCellH = 90;
static const CGFloat kWinW = 120;
static const CGFloat kWinH = 90;

static const char* g_selected;

typedef struct
{
    const char* name;
    CGRect cell;
} probe_case;

static probe_case g_cases[25];
static int g_case_count;

static CALayer* g_root;

static CGColorRef srgb(CGFloat r, CGFloat g, CGFloat b, CGFloat a)
{
    static CGColorSpaceRef space;
    if (space == NULL)
    {
        space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    }
    const CGFloat comps[4] = {r, g, b, a};
    return CGColorCreate(space, comps);
}

static CALayer* add_case(const char* name, void (^configure)(CALayer*))
{
    if (g_selected != NULL && strcmp(g_selected, name) != 0)
    {
        return nil;
    }

    const CGRect cell = CGRectMake(0, 0, kCellW, kCellH);
    const int index = g_case_count;

    CALayer* sdf = [[objc_getClass("CASDFLayer") alloc] init];
    sdf.bounds = CGRectMake(0, 0, kCellW, kCellH);
    sdf.anchorPoint = CGPointMake(0, 0);
    sdf.position = cell.origin;
    configure(sdf);
    [g_root addSublayer:sdf];

    g_cases[index] = (probe_case){name, cell};
    ++g_case_count;
    return sdf;
}

static CALayer* element(CGRect frame, CGFloat cornerRadius, NSString* operation)
{
    CALayer* e = [[objc_getClass("CASDFElementLayer") alloc] init];
    e.bounds = CGRectMake(0, 0, frame.size.width, frame.size.height);
    e.anchorPoint = CGPointMake(0, 0);
    e.position = frame.origin;
    e.cornerRadius = cornerRadius;
    if (operation != nil)
    {
        [e setValue:operation forKey:@"operation"];
    }
    return e;
}

static CGImageRef make_ramp_image(size_t w, size_t h)
{
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2);
    uint8_t* pixels = calloc(1, w * h);
    for (size_t y = 0; y < h; ++y)
    {
        for (size_t x = 0; x < w; ++x)
        {
            pixels[y * w + x] = (uint8_t)((x * 255) / (w - 1));
        }
    }
    CGContextRef ctx = CGBitmapContextCreate(pixels, w, h, 8, w, space, kCGImageAlphaNone);
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(space);
    free(pixels);
    return image;
}

static id fill_effect(CGColorRef color)
{
    id effect = [[objc_getClass("CASDFFillEffect") alloc] init];
    [effect setValue:(__bridge id)color forKey:@"color"];
    return effect;
}

typedef struct
{
    uint8_t* base;
    size_t stride;
    size_t width;
    size_t height;
    size_t scale;
} frame_buffer;

static uint32_t pixel_at(const frame_buffer* fb, size_t x, size_t y)
{
    if (x >= fb->width || y >= fb->height)
    {
        return 0;
    }
    return *(const uint32_t*)(fb->base + y * fb->stride + x * 4);
}

static void components(uint32_t p, int* r, int* g, int* b, int* a)
{
    *b = (int)(p & 0xff);
    *g = (int)((p >> 8) & 0xff);
    *r = (int)((p >> 16) & 0xff);
    *a = (int)((p >> 24) & 0xff);
}

static void print_case(const frame_buffer* fb, const probe_case* c)
{
    const size_t s = fb->scale;
    const size_t x0 = (size_t)c->cell.origin.x * s;
    const size_t y0 = (size_t)c->cell.origin.y * s;
    const size_t cw = (size_t)c->cell.size.width * s;
    const size_t ch = (size_t)c->cell.size.height * s;
    printf("CASE %s cell_points=(%g,%g,%g,%g) device=(%zu,%zu,%zu,%zu) scale=%zu\n", c->name, c->cell.origin.x, c->cell.origin.y,
           c->cell.size.width, c->cell.size.height, x0, y0, cw, ch, s);

    printf("  map (one char per %zu device px; '.'=black):\n", 2 * s);
    for (size_t y = 0; y < ch; y += 2 * s)
    {
        printf("    dy=%03zu |", y);
        for (size_t x = 0; x < cw; x += 2 * s)
        {
            int r, g, b, a;
            components(pixel_at(fb, x0 + x, y0 + y), &r, &g, &b, &a);
            const int lum = (r * 30 + g * 59 + b * 11) / 100;
            printf("%c", lum < 8 ? '.' : lum < 48 ? ':' : lum < 110 ? '-' : lum < 180 ? '+' : lum < 240 ? '*' : '#');
        }
        printf("|\n");
    }

    const size_t mid_y = ch / 2;
    printf("  scanline dy=%zu (point y=%g):", mid_y, (double)mid_y / (double)s);
    for (size_t x = 0; x < cw; ++x)
    {
        int r, g, b, a;
        components(pixel_at(fb, x0 + x, y0 + mid_y), &r, &g, &b, &a);
        if (x % 8 == 0)
        {
            printf("\n    dx=%03zu:", x);
        }
        printf(" %02x%02x%02x", r, g, b);
    }
    printf("\n");

    const size_t mid_x = cw / 2;
    printf("  column dx=%zu (point x=%g):", mid_x, (double)mid_x / (double)s);
    for (size_t y = 0; y < ch; ++y)
    {
        int r, g, b, a;
        components(pixel_at(fb, x0 + mid_x, y0 + y), &r, &g, &b, &a);
        if (y % 8 == 0)
        {
            printf("\n    dy=%03zu:", y);
        }
        printf(" %02x%02x%02x", r, g, b);
    }
    printf("\n\n");
}

static void dump_sdf_tree(CALayer* layer, int depth)
{
    for (int i = 0; i < depth; ++i)
    {
        printf("  ");
    }
    printf("%s b=(%g,%g,%g,%g) p=(%g,%g) a=(%g,%g) cr=%g", object_getClassName(layer), layer.bounds.origin.x, layer.bounds.origin.y,
           layer.bounds.size.width, layer.bounds.size.height, layer.position.x, layer.position.y, layer.anchorPoint.x,
           layer.anchorPoint.y, (double)layer.cornerRadius);
    if ([layer isKindOfClass:objc_getClass("CASDFElementLayer")])
    {
        printf(" mode=%s op=%s z0=%g z1=%g ovz=%g", [[layer valueForKey:@"mode"] UTF8String], [[layer valueForKey:@"operation"] UTF8String],
               [[layer valueForKey:@"contentsZeroValueDistance"] doubleValue], [[layer valueForKey:@"contentsOneValueDistance"] doubleValue],
               [[layer valueForKey:@"gradientOvalization"] doubleValue]);
    }
    if ([layer isKindOfClass:objc_getClass("CASDFLayer")])
    {
        id effect = [layer valueForKey:@"effect"];
        printf(" effect=%s smooth=%g gauss=%g offset=%g merge=%d", effect ? object_getClassName(effect) : "nil",
               [[layer valueForKey:@"smoothness"] doubleValue], [[layer valueForKey:@"gaussianRadius"] doubleValue],
               [[layer valueForKey:@"effectOffset"] doubleValue], [[layer valueForKey:@"mergeElements"] boolValue]);
    }
    printf("\n");
    for (CALayer* child in layer.sublayers)
    {
        dump_sdf_tree(child, depth + 1);
    }
}

@interface SdfRenderProbe : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@end

@implementation SdfRenderProbe

- (void)build
{
    CGColorRef red = srgb(1, 0, 0, 1);
    CGColorRef white = srgb(1, 1, 1, 1);
    CGColorRef half = srgb(1, 1, 1, 0.5);

    add_case("baseline_capsule", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("radius0_rect", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 0, nil)];
    });

    add_case("radius10", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 10, nil)];
    });

    add_case("no_effect", ^(CALayer* sdf) {
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("white_fill", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(white) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("alpha_half_fill", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(half) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("two_disjoint", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(10, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(70, 25, 40, 40), 20, nil)];
    });

    add_case("two_near_merge0", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@NO forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("two_near_merge1", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("two_smooth8", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf setValue:@8.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("subtraction", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
      [sdf addSublayer:element(CGRectMake(45, 30, 30, 30), 15, @"subtraction")];
    });

    add_case("mode_contents", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 25, nil);
      [e setValue:@"contents" forKey:@"mode"];
      [sdf addSublayer:e];
    });

    add_case("zero_dist_-5_one_5", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 25, nil);
      [e setValue:@(-5.0) forKey:@"contentsZeroValueDistance"];
      [e setValue:@(5.0) forKey:@"contentsOneValueDistance"];
      [sdf addSublayer:e];
    });

    add_case("effect_offset_8", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@8.0 forKey:@"effectOffset"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("effect_offset_-8", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@(-8.0) forKey:@"effectOffset"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("gaussian_6", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@6.0 forKey:@"gaussianRadius"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("output_effect", ^(CALayer* sdf) {
      [sdf setValue:[[objc_getClass("CASDFOutputEffect") alloc] init] forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("visualization", ^(CALayer* sdf) {
      [sdf setValue:[[objc_getClass("CASDFVisualizationEffect") alloc] init] forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("shadow_effect", ^(CALayer* sdf) {
      id effect = [[objc_getClass("CASDFShadowEffect") alloc] init];
      [effect setValue:(__bridge id)white forKey:@"color"];
      [effect setValue:@6.0 forKey:@"radius"];
      [effect setValue:[NSValue valueWithSize:NSMakeSize(0, 0)] forKey:@"offset"];
      [sdf setValue:effect forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("gradient_effect", ^(CALayer* sdf) {
      id effect = [[objc_getClass("CASDFGradientEffect") alloc] init];
      [effect setValue:@[(__bridge id)red, (__bridge id)white] forKey:@"colors"];
      [effect setValue:@[@(-20.0), @(0.0)] forKey:@"distances"];
      [sdf setValue:effect forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("element_bg_ignored", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 25, nil);
      e.backgroundColor = srgb(0, 0, 1, 1);
      [sdf addSublayer:e];
    });

    add_case("plain_sublayer_child", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* plain = [CALayer layer];
      plain.bounds = CGRectMake(0, 0, 80, 50);
      plain.anchorPoint = CGPointMake(0, 0);
      plain.position = CGPointMake(20, 20);
      plain.cornerRadius = 25;
      [sdf addSublayer:plain];
    });

    add_case("nested_in_plain", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* wrapper = [CALayer layer];
      wrapper.bounds = CGRectMake(0, 0, kCellW, kCellH);
      wrapper.anchorPoint = CGPointMake(0, 0);
      wrapper.position = CGPointMake(0, 0);
      [wrapper addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
      [sdf addSublayer:wrapper];
    });

    add_case("hidden_element", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 25, nil);
      e.hidden = YES;
      [sdf addSublayer:e];
    });

    add_case("gap10_merge0", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@NO forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(15, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(65, 25, 40, 40), 20, nil)];
    });

    add_case("gap10_merge1", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(15, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(65, 25, 40, 40), 20, nil)];
    });

    add_case("gap10_smooth4", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@4.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(15, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(65, 25, 40, 40), 20, nil)];
    });

    add_case("gap10_smooth16", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@16.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(15, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(65, 25, 40, 40), 20, nil)];
    });

    add_case("gap10_smooth16_merge1", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@16.0 forKey:@"smoothness"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(15, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(65, 25, 40, 40), 20, nil)];
    });

    add_case("sub_then_union", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(45, 30, 30, 30), 15, @"subtraction")];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("union_sub_union", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
      [sdf addSublayer:element(CGRectMake(45, 30, 30, 30), 15, @"subtraction")];
      [sdf addSublayer:element(CGRectMake(52, 37, 16, 16), 8, nil)];
    });

    add_case("radius_huge", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 100, nil)];
    });

    add_case("corner_continuous", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 20, nil);
      e.cornerCurve = kCACornerCurveContinuous;
      [sdf addSublayer:e];
    });

    add_case("element_scaled", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 40, 25), 12, nil);
      e.affineTransform = CGAffineTransformMakeScale(2, 2);
      [sdf addSublayer:e];
    });

    add_case("element_rotated", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 30), 15, nil);
      e.anchorPoint = CGPointMake(0.5, 0.5);
      e.position = CGPointMake(60, 45);
      e.affineTransform = CGAffineTransformMakeRotation(M_PI / 8);
      [sdf addSublayer:e];
    });

    add_case("element_outside_bounds", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(90, 20, 80, 50), 25, nil)];
    });

    add_case("sdf_maskstobounds", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      sdf.masksToBounds = YES;
      [sdf addSublayer:element(CGRectMake(90, 20, 80, 50), 25, nil)];
    });

    add_case("sdf_opacity_half", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      sdf.opacity = 0.5f;
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("effect_array", ^(CALayer* sdf) {
      id shadow = [[objc_getClass("CASDFShadowEffect") alloc] init];
      [shadow setValue:(__bridge id)srgb(0, 0, 1, 1) forKey:@"color"];
      [shadow setValue:@8.0 forKey:@"radius"];
      @try
      {
          [sdf setValue:@[shadow, fill_effect(red)] forKey:@"effect"];
      }
      @catch (NSException* e)
      {
          printf("NOTE effect_array threw %s\n", e.name.UTF8String);
      }
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("stacked_sdf_layers", ^(CALayer* sdf) {
      id shadow = [[objc_getClass("CASDFShadowEffect") alloc] init];
      [shadow setValue:(__bridge id)srgb(0, 0, 1, 1) forKey:@"color"];
      [shadow setValue:@8.0 forKey:@"radius"];
      [shadow setValue:[NSValue valueWithSize:NSMakeSize(0, 0)] forKey:@"offset"];
      [sdf setValue:shadow forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];

      CALayer* top = [[objc_getClass("CASDFLayer") alloc] init];
      top.bounds = sdf.bounds;
      top.anchorPoint = CGPointMake(0, 0);
      top.position = CGPointMake(0, 0);
      [top setValue:fill_effect(red) forKey:@"effect"];
      [top addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
      [sdf addSublayer:top];
    });

    add_case("gradient_ovalization", ^(CALayer* sdf) {
      id effect = [[objc_getClass("CASDFGradientEffect") alloc] init];
      [effect setValue:@[(__bridge id)red, (__bridge id)white] forKey:@"colors"];
      [effect setValue:@[@(-25.0), @(0.0)] forKey:@"distances"];
      [sdf setValue:effect forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 25, nil);
      [e setValue:@1.0 forKey:@"gradientOvalization"];
      [sdf addSublayer:e];
    });

    add_case("contents_image", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 0, nil);
      [e setValue:@"contents" forKey:@"mode"];
      e.contents = (__bridge id)make_ramp_image(80, 50);
      [sdf addSublayer:e];
    });

    add_case("contents_image_z-10_o10", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 0, nil);
      [e setValue:@"contents" forKey:@"mode"];
      [e setValue:@(-10.0) forKey:@"contentsZeroValueDistance"];
      [e setValue:@(10.0) forKey:@"contentsOneValueDistance"];
      e.contents = (__bridge id)make_ramp_image(80, 50);
      [sdf addSublayer:e];
    });

    add_case("gradient_distances_wide", ^(CALayer* sdf) {
      id effect = [[objc_getClass("CASDFGradientEffect") alloc] init];
      [effect setValue:@[(__bridge id)red, (__bridge id)white, (__bridge id)srgb(0, 0, 1, 1)] forKey:@"colors"];
      [effect setValue:@[@(-20.0), @(-10.0), @(0.0)] forKey:@"distances"];
      [sdf setValue:effect forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20, 20, 80, 50), 25, nil)];
    });

    add_case("subpixel_000", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20.0, 20, 60, 50), 0, nil)];
    });

    add_case("subpixel_025", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20.25, 20, 60, 50), 0, nil)];
    });

    add_case("subpixel_050", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20.5, 20, 60, 50), 0, nil)];
    });

    add_case("subpixel_075", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(20.75, 20, 60, 50), 0, nil)];
    });

    add_case("radius20_circular", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 20, nil);
      e.cornerCurve = kCACornerCurveCircular;
      [sdf addSublayer:e];
    });

    add_case("radius20_continuous", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 20, nil);
      e.cornerCurve = kCACornerCurveContinuous;
      [sdf addSublayer:e];
    });

    add_case("touch_sm00", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@0.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("touch_sm02", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@2.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("touch_sm04", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@4.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("touch_sm16", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@16.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("touch_sm32", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@32.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(20, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(60, 25, 40, 40), 20, nil)];
    });

    add_case("gap4_sm08", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@8.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(18, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(62, 25, 40, 40), 20, nil)];
    });

    add_case("overlap10_sm00", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf addSublayer:element(CGRectMake(25, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(55, 25, 40, 40), 20, nil)];
    });

    add_case("overlap10_sm08", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@8.0 forKey:@"smoothness"];
      [sdf addSublayer:element(CGRectMake(25, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(55, 25, 40, 40), 20, nil)];
    });

    add_case("grad_overlap_merge0", ^(CALayer* sdf) {
      id effect = [[objc_getClass("CASDFGradientEffect") alloc] init];
      [effect setValue:@[(__bridge id)red, (__bridge id)white] forKey:@"colors"];
      [effect setValue:@[@(-14.0), @(0.0)] forKey:@"distances"];
      [sdf setValue:effect forKey:@"effect"];
      [sdf setValue:@NO forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(25, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(55, 25, 40, 40), 20, nil)];
    });

    add_case("grad_overlap_merge1", ^(CALayer* sdf) {
      id effect = [[objc_getClass("CASDFGradientEffect") alloc] init];
      [effect setValue:@[(__bridge id)red, (__bridge id)white] forKey:@"colors"];
      [effect setValue:@[@(-14.0), @(0.0)] forKey:@"distances"];
      [sdf setValue:effect forKey:@"effect"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(25, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(55, 25, 40, 40), 20, nil)];
    });

    add_case("swiftui_glass_shape", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(white) forKey:@"effect"];
      [sdf setValue:@6.0 forKey:@"smoothness"];
      CALayer* e = element(CGRectMake(20, 23, 44, 44), 12, nil);
      e.cornerCurve = kCACornerCurveContinuous;
      [sdf addSublayer:e];
    });

    add_case("disjoint_merge1", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(10, 25, 40, 40), 20, nil)];
      [sdf addSublayer:element(CGRectMake(70, 25, 40, 40), 20, nil)];
    });

    add_case("difftall_merge1", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@YES forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(15, 15, 30, 20), 6, nil)];
      [sdf addSublayer:element(CGRectMake(70, 55, 35, 25), 10, nil)];
    });

    add_case("difftall_merge0", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      [sdf setValue:@NO forKey:@"mergeElements"];
      [sdf addSublayer:element(CGRectMake(15, 15, 30, 20), 6, nil)];
      [sdf addSublayer:element(CGRectMake(70, 55, 35, 25), 10, nil)];
    });

    add_case("element_opacity_half", ^(CALayer* sdf) {
      [sdf setValue:fill_effect(red) forKey:@"effect"];
      CALayer* e = element(CGRectMake(20, 20, 80, 50), 25, nil);
      e.opacity = 0.5f;
      [sdf addSublayer:e];
    });
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    (void)note;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 300, kWinW, kWinH)
                                              styleMask:NSWindowStyleMaskBorderless
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"sdfrender";
    self.window.backgroundColor = NSColor.blackColor;
    self.window.opaque = YES;

    NSView* view = self.window.contentView;
    view.wantsLayer = YES;
    view.layer.backgroundColor = srgb(0, 0, 0, 1);
    view.layer.geometryFlipped = YES;
    g_root = view.layer;

    [self build];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
      [self capture];
    });
}

- (void)capture
{
    printf("SDFRENDER host=%s backingScale=%g window=%ld size=%gx%g\n",
           NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String, (double)self.window.backingScaleFactor,
           (long)self.window.windowNumber, (double)kWinW, (double)kWinH);
    printf("--- layer tree ---\n");
    dump_sdf_tree(g_root, 0);
    printf("--- end tree ---\n");

    window_list_create_image_fn create_image = (window_list_create_image_fn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    if (create_image == NULL)
    {
        printf("SDFRENDER-FAIL no CGWindowListCreateImage\n");
        exit(2);
    }

    CGImageRef image = create_image(CGRectNull, (1 << 3) /*kCGWindowListOptionIncludingWindow*/,
                                    (CGWindowID)self.window.windowNumber, (1 << 0) /*kCGWindowImageBoundsIgnoreFraming*/);
    if (image == NULL)
    {
        printf("SDFRENDER-FAIL capture returned nil\n");
        exit(3);
    }

    const size_t w = CGImageGetWidth(image);
    const size_t h = CGImageGetHeight(image);
    printf("capture %zux%zu bpp=%zu\n", w, h, CGImageGetBitsPerPixel(image));

    const size_t stride = w * 4;
    uint8_t* pixels = calloc(1, stride * h);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(pixels, w, h, 8, stride, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextTranslateCTM(ctx, 0, (CGFloat)h);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), image);

    const frame_buffer fb = {pixels, stride, w, h, w / (size_t)kWinW};
    for (int i = 0; i < g_case_count; ++i)
    {
        print_case(&fb, &g_cases[i]);
    }

    NSData* png = nil;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:@"/tmp/sdfprobe/sdfrender.png" atomically:YES];
    printf("SDFRENDER-PNG /tmp/sdfprobe/sdfrender.png %lu bytes\n", (unsigned long)png.length);

    CGContextRelease(ctx);
    CGColorSpaceRelease(space);
    free(pixels);
    CGImageRelease(image);
    fflush(stdout);
    exit(0);
}

@end

int main(int argc, const char** argv)
{
    @autoreleasepool
    {
        if (argc > 1)
        {
            g_selected = argv[1];
        }
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        SdfRenderProbe* probe = [[SdfRenderProbe alloc] init];
        NSApp.delegate = probe;
        [NSApp run];
    }

    return 0;
}
