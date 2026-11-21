/* Drawing into a focused image and reading it back, as programs that render text with AppKit into GL textures do
   (cocos2d-x's CCImage: lockFocus on an NSImage, [[NSAffineTransform transform] set], drawInRect:, then
   -[NSBitmapImageRep initWithFocusedViewRect:] and bitmapData).

   - NSAffineTransform's -set and -concat (AppKit additions to a Foundation class) act on Henge's current
     NSGraphicsContext. -set makes the matrix the device's (a focused image's pixels), as AppKit does, so the
     identity undoes the screen scale lockFocus applied.
   - -[NSBitmapImageRep initWithFocusedViewRect:] (HKDraw.m answers nil) copies the rectangle, in the current
     coordinate system, out of the focused bitmap: RGBA, 8 bits, premultiplied, rows packed, AppKit's layout.

   Both are installed over whatever the class has at load (macOS's UIFoundation has a -set that finds no context
   here); they belong in HKDraw.m. */
#import "HKShim.h"
#import <objc/message.h>
#import <objc/runtime.h>

static CGContextRef current_cg(void) {
    NSGraphicsContext *g = [NSGraphicsContext currentContext];
    return g ? g.CGContext : NULL;
}

typedef struct {
    CGFloat m11, m12, m21, m22, tX, tY;
} HKAffineStruct;

static CGAffineTransform affine_of(id transform) {
    HKAffineStruct s = {1, 0, 0, 1, 0, 0};
    if ([transform respondsToSelector:@selector(transformStruct)])
        s = ((HKAffineStruct(*)(id, SEL))objc_msgSend)(transform, @selector(transformStruct));
    return CGAffineTransformMake(s.m11, s.m12, s.m21, s.m22, s.tX, s.tY);
}

/* Methods written here (for ARC's ownership rules of their families) and installed on the classes at load. */
@interface NSObject (HKFocusRep)
- (instancetype)initWithCGImage:(CGImageRef)image;
@end

@interface HKFocusSupport : NSObject
@end
@implementation HKFocusSupport
- (void)set {
    CGContextRef ctx = current_cg();
    if (!ctx) return;
    CGContextConcatCTM(ctx, CGAffineTransformInvert(CGContextGetCTM(ctx)));
    CGContextConcatCTM(ctx, affine_of(self));
}

- (void)concat {
    CGContextRef ctx = current_cg();
    if (ctx) CGContextConcatCTM(ctx, affine_of(self));
}

- (instancetype)initWithFocusedViewRect:(NSRect)rect {
    CGContextRef ctx = current_cg();
    if (!ctx || !CGBitmapContextGetData(ctx)) {
        hk_log("initWithFocusedViewRect: no focused bitmap");
        return nil;
    }
    CGRect dev = CGRectIntegral(CGRectApplyAffineTransform(rect, CGContextGetCTM(ctx)));
    size_t w = (size_t)dev.size.width, h = (size_t)dev.size.height;
    if (!w || !h) return nil;
    CGImageRef whole = CGBitmapContextCreateImage(ctx);
    if (!whole) return nil;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef out = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!out) {
        CGImageRelease(whole);
        return nil;
    }
    CGContextSetBlendMode(out, kCGBlendModeCopy);
    CGContextDrawImage(out, CGRectMake(-dev.origin.x, -dev.origin.y, CGBitmapContextGetWidth(ctx), CGBitmapContextGetHeight(ctx)), whole);
    CGImageRelease(whole);
    CGImageRef img = CGBitmapContextCreateImage(out);
    CGContextRelease(out);
    self = [self initWithCGImage:img];
    CGImageRelease(img);
    return self;
}

static void install(Class target, SEL sel) {
    Method m = class_getInstanceMethod([HKFocusSupport class], sel);
    if (target && m) class_replaceMethod(target, sel, method_getImplementation(m), method_getTypeEncoding(m));
}

+ (void)load {
    install(objc_getClass("NSAffineTransform"), @selector(set));
    install(objc_getClass("NSAffineTransform"), @selector(concat));
    install(objc_getClass("NSBitmapImageRep"), @selector(initWithFocusedViewRect:));
}
@end
