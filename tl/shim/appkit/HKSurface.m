/* IOSurfaces as layer contents. A Mac app's compositor (Chromium's) hands each finished frame to Core Animation as an
   IOSurface set as a layer's contents, and re-signals the same surface with -setContentsChanged when it redraws into
   it. iOS's Core Animation is not known to show an IOSurface given that way (a Chromium window on the phone showed its
   frame colour and nothing it composited), so once a Mac app runs there, such contents become an image of the
   surface's pixels (copied, so the app may draw on into the surface). HK_SURFACE_COPY=1 does the same on a Mac, to
   test it. */
#import "HKShim.h"
#import <IOSurface/IOSurfaceRef.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

static IMP g_set_contents, g_contents_changed;
static char g_surface_key;
static int g_said;

/* The surface's pixels as an image, or NULL for a pixel format this does not know. */
static CGImageRef image_of_surface(IOSurfaceRef s) {
    OSType format = IOSurfaceGetPixelFormat(s);
    CGBitmapInfo info;
    if (format == 'BGRA') info = kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaPremultipliedFirst;
    else if (format == 'RGBA') info = kCGBitmapByteOrder32Big | (CGBitmapInfo)kCGImageAlphaPremultipliedLast;
    else return NULL;
    size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), row = IOSurfaceGetBytesPerRow(s);
    if (!w || !h) return NULL;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    CFDataRef data = CFDataCreate(NULL, IOSurfaceGetBaseAddress(s), (CFIndex)(row * h));
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(w, h, 8, 32, row, cs, info, provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    return img;
}

static void set_contents(CALayer *self, SEL _cmd, id contents) {
    if (contents && CFGetTypeID((__bridge CFTypeRef)contents) == IOSurfaceGetTypeID()) {
        IOSurfaceRef s = (__bridge IOSurfaceRef)contents;
        if (g_said++ < 3)
            hk_log("an IOSurface as layer contents (%zux%zu, '%.4s'): shown as an image", IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
                   (const char *)&(OSType){CFSwapInt32HostToBig(IOSurfaceGetPixelFormat(s))});
        objc_setAssociatedObject(self, &g_surface_key, contents, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        CGImageRef img = image_of_surface(s);
        if (img) {
            ((void (*)(id, SEL, id))g_set_contents)(self, _cmd, (__bridge id)img);
            CGImageRelease(img);
            return;
        }
    } else if (objc_getAssociatedObject(self, &g_surface_key)) {
        objc_setAssociatedObject(self, &g_surface_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    ((void (*)(id, SEL, id))g_set_contents)(self, _cmd, contents);
}

/* The app drew again into the surface it already gave: a fresh image of it. */
static void contents_changed(CALayer *self, SEL _cmd) {
    id surface = objc_getAssociatedObject(self, &g_surface_key);
    if (surface) {
        CGImageRef img = image_of_surface((__bridge IOSurfaceRef)surface);
        if (img) {
            ((void (*)(id, SEL, id))g_set_contents)(self, @selector(setContents:), (__bridge id)img);
            CGImageRelease(img);
        }
    }
    if (g_contents_changed) ((void (*)(id, SEL))g_contents_changed)(self, _cmd);
}

void hk_surfaces_as_images(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Method m = class_getInstanceMethod([CALayer class], @selector(setContents:));
        g_set_contents = method_setImplementation(m, (IMP)set_contents);
        SEL changed = NSSelectorFromString(@"setContentsChanged");
        Method c = class_getInstanceMethod([CALayer class], changed);
        if (c) g_contents_changed = method_setImplementation(c, (IMP)contents_changed);
        else class_addMethod([CALayer class], changed, (IMP)contents_changed, "v@:");
    });
}

/* Layer autoresizing (macOS's CALayer.autoresizingMask, which iOS's layers ignore). Chromium puts its composited frame
   in a flipped layer that is never sized, only told to follow its parent's size (width and height sizable); on iOS it
   stayed 0 by 0, and a flipped layer of no height turns its content up and out of the window. So, once a Mac app runs
   on iOS, a layer's mask is kept, and when a layer's size changes its sublayers with one are resized as macOS does it
   (each flexible margin or size takes an equal share of the change). */
static char g_mask_key;
static IMP g_set_mask, g_set_bounds, g_set_frame;

static void set_mask(CALayer *self, SEL _cmd, unsigned mask) {
    objc_setAssociatedObject(self, &g_mask_key, mask ? @(mask) : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (g_set_mask) ((void (*)(id, SEL, unsigned))g_set_mask)(self, _cmd, mask);
}

static unsigned get_mask(CALayer *self, SEL _cmd) { return [objc_getAssociatedObject(self, &g_mask_key) unsignedIntValue]; }

static void resize_sublayers(CALayer *layer, CGSize old) {
    CGSize now = layer.bounds.size;
    if (CGSizeEqualToSize(old, now)) return;
    NSArray<CALayer *> *subs = layer.sublayers;
    for (CALayer *c in subs) {
        unsigned m = [objc_getAssociatedObject(c, &g_mask_key) unsignedIntValue];
        if (!m) continue;
        CGRect f = c.frame;
        int n = !!(m & 1) + !!(m & 2) + !!(m & 4); /* min x margin, width, max x margin */
        if (n) {
            CGFloat d = (now.width - old.width) / n;
            if (m & 1) f.origin.x += d;
            if (m & 2) f.size.width += d;
        }
        n = !!(m & 8) + !!(m & 16) + !!(m & 32); /* min y margin, height, max y margin */
        if (n) {
            CGFloat d = (now.height - old.height) / n;
            if (m & 8) f.origin.y += d;
            if (m & 16) f.size.height += d;
        }
        c.frame = f;
    }
}

/* setFrame: may set the bounds itself: the sublayers are resized once, by the outer call. */
static __thread __unsafe_unretained CALayer *g_framing;

static void set_bounds(CALayer *self, SEL _cmd, CGRect b) {
    CGSize old = self.bounds.size;
    ((void (*)(id, SEL, CGRect))g_set_bounds)(self, _cmd, b);
    if (g_framing != self) resize_sublayers(self, old);
}

static void set_frame(CALayer *self, SEL _cmd, CGRect f) {
    CGSize old = self.bounds.size;
    CALayer *outer = g_framing;
    g_framing = self;
    ((void (*)(id, SEL, CGRect))g_set_frame)(self, _cmd, f);
    g_framing = outer;
    resize_sublayers(self, old);
}

void hk_layer_autoresizing(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cls = [CALayer class];
        Method m = class_getInstanceMethod(cls, @selector(setAutoresizingMask:));
        if (m) g_set_mask = method_setImplementation(m, (IMP)set_mask);
        else class_addMethod(cls, @selector(setAutoresizingMask:), (IMP)set_mask, "v@:I");
        Method g = class_getInstanceMethod(cls, @selector(autoresizingMask));
        if (g) method_setImplementation(g, (IMP)get_mask);
        else class_addMethod(cls, @selector(autoresizingMask), (IMP)get_mask, "I@:");
        g_set_bounds = method_setImplementation(class_getInstanceMethod(cls, @selector(setBounds:)), (IMP)set_bounds);
        g_set_frame = method_setImplementation(class_getInstanceMethod(cls, @selector(setFrame:)), (IMP)set_frame);
    });
}
