// Ground truth for the signed-distance-field layer classes SwiftUI paints button bezels with. Prints
// the class hierarchy, property list, ivar list and freshly-constructed defaults for every QuartzCore
// CASDF* class and for SwiftUI's own SDF CALayer subclasses, which only exist once SwiftUI is loaded.
// Build and run per README.md.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <dlfcn.h>

static const char* kQuartzCoreClasses[] = {
    "CASDFLayer", "CASDFElementLayer", "CABackdropLayer", "CAShapeLayer", "CAPortalLayer",
};

static const char* kSwiftUIClasses[] = {
    "SwiftUI.SDFLayer",
    "_TtC7SwiftUIP33_05C1F5BE6EE7940FED05100EA822F7B314SDFPortalLayer",
    "_TtC7SwiftUIP33_E19F490D25D5E0EC8A24903AF958E34115ColorShapeLayer",
    "_TtC7SwiftUIP33_E19F490D25D5E0EC8A24903AF958E34115PaintShapeLayer",
    "_SwiftUISDFLayerDelegate",
};

static const char* kEffectClasses[] = {
    "CASDFEffect",         "CASDFFillEffect",           "CASDFGradientEffect",
    "CASDFGradientContourEffect", "CASDFShadowEffect",  "CASDFGlassHighlightEffect",
    "CASDFGlassDisplacementEffect", "CASDFKeyFillHighlightEffect",
    "CASDFOutputEffect",   "CASDFVisualizationEffect",  "CASDFGenerator",
    "CASDFGeneratorRequest",
};

static void print_hierarchy(Class cls)
{
    printf("  hierarchy:");
    for (Class c = cls; c != Nil; c = class_getSuperclass(c))
    {
        printf(" %s", class_getName(c));
    }
    printf("\n");
}

static void print_properties(Class cls, BOOL instance)
{
    unsigned int count = 0;
    objc_property_t* props = instance ? class_copyPropertyList(cls, &count) : class_copyPropertyList(object_getClass(cls), &count);
    printf("  %s properties (%u):\n", instance ? "instance" : "class", count);
    for (unsigned int i = 0; i < count; ++i)
    {
        printf("    %-34s %s\n", property_getName(props[i]), property_getAttributes(props[i]));
    }
    free(props);
}

static void print_ivars(Class cls)
{
    unsigned int count = 0;
    Ivar* ivars = class_copyIvarList(cls, &count);
    printf("  ivars (%u):\n", count);
    for (unsigned int i = 0; i < count; ++i)
    {
        printf("    %-34s %-24s off=%td\n", ivar_getName(ivars[i]), ivar_getTypeEncoding(ivars[i]) ?: "?", ivar_getOffset(ivars[i]));
    }
    free(ivars);
}

static void print_methods(Class cls)
{
    unsigned int count = 0;
    Method* methods = class_copyMethodList(cls, &count);
    printf("  instance methods (%u):\n", count);
    for (unsigned int i = 0; i < count; ++i)
    {
        printf("    %-44s %s\n", sel_getName(method_getName(methods[i])), method_getTypeEncoding(methods[i]) ?: "?");
    }
    free(methods);

    count = 0;
    methods = class_copyMethodList(object_getClass(cls), &count);
    printf("  class methods (%u):\n", count);
    for (unsigned int i = 0; i < count; ++i)
    {
        printf("    %-44s %s\n", sel_getName(method_getName(methods[i])), method_getTypeEncoding(methods[i]) ?: "?");
    }
    free(methods);
}

static void print_value(id object, const char* name, const char* attributes)
{
    SEL sel = sel_getUid(name);
    if (![object respondsToSelector:sel])
    {
        printf("    %-34s <no getter>\n", name);
        return;
    }

    const char* type = strchr(attributes, 'T');
    if (type == NULL)
    {
        return;
    }
    ++type;

    NSMethodSignature* signature = [object methodSignatureForSelector:sel];
    if (signature == nil)
    {
        printf("    %-34s <no signature>\n", name);
        return;
    }

    NSInvocation* invocation = [NSInvocation invocationWithMethodSignature:signature];
    invocation.selector = sel;
    @try
    {
        [invocation invokeWithTarget:object];
    }
    @catch (NSException* e)
    {
        printf("    %-34s <threw %s>\n", name, e.name.UTF8String);
        return;
    }

    const char* ret = signature.methodReturnType;
    printf("    %-34s ", name);
    if (strcmp(ret, "@") == 0)
    {
        void* raw = NULL;
        [invocation getReturnValue:&raw];
        id value = (__bridge id)raw;
        printf("(id %s) %s\n", value ? object_getClassName(value) : "nil", value ? [[value description] UTF8String] : "");
    }
    else if (strcmp(ret, "^{CGColor=}") == 0 || strcmp(ret, "^{CGPath=}") == 0 || ret[0] == '^')
    {
        void* raw = NULL;
        [invocation getReturnValue:&raw];
        printf("(ptr %s) %p", ret, raw);
        if (raw && strcmp(ret, "^{CGColor=}") == 0)
        {
            CGColorRef color = (CGColorRef)raw;
            const size_t n = CGColorGetNumberOfComponents(color);
            const CGFloat* c = CGColorGetComponents(color);
            printf(" rgba[");
            for (size_t i = 0; i < n; ++i)
            {
                printf("%s%.4f", i ? " " : "", (double)c[i]);
            }
            printf("]");
        }
        printf("\n");
    }
    else if (strcmp(ret, "d") == 0)
    {
        double value = 0;
        [invocation getReturnValue:&value];
        printf("(double) %g\n", value);
    }
    else if (strcmp(ret, "f") == 0)
    {
        float value = 0;
        [invocation getReturnValue:&value];
        printf("(float) %g\n", (double)value);
    }
    else if (strcmp(ret, "B") == 0 || strcmp(ret, "c") == 0)
    {
        signed char value = 0;
        [invocation getReturnValue:&value];
        printf("(bool) %d\n", (int)value);
    }
    else if (strcmp(ret, "q") == 0 || strcmp(ret, "l") == 0 || strcmp(ret, "i") == 0)
    {
        long long value = 0;
        [invocation getReturnValue:&value];
        printf("(int) %lld\n", value);
    }
    else if (strcmp(ret, "Q") == 0 || strcmp(ret, "L") == 0 || strcmp(ret, "I") == 0)
    {
        unsigned long long value = 0;
        [invocation getReturnValue:&value];
        printf("(uint) %llu\n", value);
    }
    else if (strncmp(ret, "{CGRect", 7) == 0)
    {
        CGRect value = CGRectZero;
        [invocation getReturnValue:&value];
        printf("(CGRect) (%g,%g,%g,%g)\n", value.origin.x, value.origin.y, value.size.width, value.size.height);
    }
    else if (strncmp(ret, "{CGPoint", 8) == 0)
    {
        CGPoint value = CGPointZero;
        [invocation getReturnValue:&value];
        printf("(CGPoint) (%g,%g)\n", value.x, value.y);
    }
    else if (strncmp(ret, "{CGSize", 7) == 0)
    {
        CGSize value = CGSizeZero;
        [invocation getReturnValue:&value];
        printf("(CGSize) (%g,%g)\n", value.width, value.height);
    }
    else
    {
        printf("<unhandled return %s>\n", ret);
    }
}

static void print_defaults(Class cls)
{
    id object = nil;
    @try
    {
        object = [[cls alloc] init];
    }
    @catch (NSException* e)
    {
        printf("  defaults: <init threw %s>\n", e.name.UTF8String);
        return;
    }

    if (object == nil)
    {
        printf("  defaults: <init returned nil>\n");
        return;
    }

    printf("  defaults for a fresh %s %p:\n", class_getName(cls), (__bridge void*)object);
    for (Class c = cls; c != Nil && c != [CALayer class] && c != [NSObject class]; c = class_getSuperclass(c))
    {
        unsigned int count = 0;
        objc_property_t* props = class_copyPropertyList(c, &count);
        for (unsigned int i = 0; i < count; ++i)
        {
            print_value(object, property_getName(props[i]), property_getAttributes(props[i]));
        }
        free(props);
    }

    if ([cls isSubclassOfClass:[CALayer class]])
    {
        CALayer* layer = (CALayer*)object;
        printf("    (CALayer) bounds=(%g,%g,%g,%g) position=(%g,%g) cornerRadius=%g opacity=%g\n", layer.bounds.origin.x,
               layer.bounds.origin.y, layer.bounds.size.width, layer.bounds.size.height, layer.position.x, layer.position.y,
               (double)layer.cornerRadius, (double)layer.opacity);
    }
}

static void dump_class(const char* name, BOOL with_methods)
{
    Class cls = objc_getClass(name);
    printf("=== %s ===\n", name);
    if (cls == Nil)
    {
        printf("  <not registered>\n\n");
        return;
    }

    print_hierarchy(cls);
    print_properties(cls, YES);
    print_properties(cls, NO);
    print_ivars(cls);
    if (with_methods)
    {
        print_methods(cls);
    }
    print_defaults(cls);
    printf("\n");
}

static void dump_matching(const char* needle)
{
    unsigned int count = 0;
    Class* classes = objc_copyClassList(&count);
    printf("=== classes matching \"%s\" ===\n", needle);
    for (unsigned int i = 0; i < count; ++i)
    {
        const char* name = class_getName(classes[i]);
        if (strcasestr(name, needle) == NULL)
        {
            continue;
        }

        printf("  %-52s image=%s super=%s\n", name, class_getImageName(classes[i]) ?: "?",
               class_getSuperclass(classes[i]) ? class_getName(class_getSuperclass(classes[i])) : "-");
    }
    free(classes);
    printf("\n");
}

int main(int argc, const char** argv)
{
    @autoreleasepool
    {
        const BOOL with_methods = (argc > 1 && strcmp(argv[1], "-m") == 0);

        printf("### stage 1: QuartzCore only, SwiftUI not loaded\n\n");
        for (size_t i = 0; i < sizeof(kSwiftUIClasses) / sizeof(*kSwiftUIClasses); ++i)
        {
            printf("preload objc_getClass(\"%s\") = %p\n", kSwiftUIClasses[i], (__bridge void*)objc_getClass(kSwiftUIClasses[i]));
        }
        printf("\n");

        for (size_t i = 0; i < sizeof(kQuartzCoreClasses) / sizeof(*kQuartzCoreClasses); ++i)
        {
            dump_class(kQuartzCoreClasses[i], with_methods);
        }

        dump_matching("sdf");

        printf("### stage 2: after dlopen(SwiftUI)\n\n");
        void* swiftui = dlopen("/System/Library/Frameworks/SwiftUI.framework/SwiftUI", RTLD_NOW);
        printf("dlopen(SwiftUI) = %p err=%s\n\n", swiftui, swiftui ? "-" : dlerror());

        for (size_t i = 0; i < sizeof(kSwiftUIClasses) / sizeof(*kSwiftUIClasses); ++i)
        {
            dump_class(kSwiftUIClasses[i], with_methods);
        }

        printf("### stage 3: CASDFEffect family\n\n");
        for (size_t i = 0; i < sizeof(kEffectClasses) / sizeof(*kEffectClasses); ++i)
        {
            dump_class(kEffectClasses[i], with_methods);
        }

        dump_matching("sdf");
        dump_matching("shapelayer");
        fflush(stdout);
    }

    return 0;
}
