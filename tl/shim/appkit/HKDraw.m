/* Henge's AppKit, drawing: color spaces and colors, fonts, images and bitmap representations, graphics contexts and
   Bézier paths, over CoreGraphics, CoreText and ImageIO.

   Fonts are CoreText's objects, as on the Mac where NSFont is toll-free bridged with CTFontRef: NSFont's class
   methods answer CTFonts, and whatever class CoreFoundation gives those in this process (__NSCFType when nothing
   bridges them, UICTFont where UIKit is loaded) is given NSFont's and NSFontDescriptor's instance methods, each
   checking the CF type of its receiver since __NSCFType is shared by every unbridged CF type. */
#import "HKShim.h"
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <objc/runtime.h>
#include <os/lock.h>
#include <stdatomic.h>

static CGContextRef current_cg(void) { return [NSGraphicsContext currentContext].CGContext; }

/* ================================================================ color spaces */

@implementation NSColorSpace {
    CGColorSpaceRef _space;
}

static NSColorSpace *space_named(CFStringRef name) {
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    static NSMutableDictionary *cache;
    NSString *key = (__bridge NSString *)name;
    os_unfair_lock_lock(&lock);
    NSColorSpace *s = cache[key];
    os_unfair_lock_unlock(&lock);
    if (s) return s;
    CGColorSpaceRef cg = CGColorSpaceCreateWithName(name);
    s = [[NSColorSpace alloc] initWithCGColorSpace:cg];
    CGColorSpaceRelease(cg);
    if (!s) return nil;
    os_unfair_lock_lock(&lock);
    if (!cache) cache = [NSMutableDictionary dictionary];
    if (cache[key]) s = cache[key];
    else cache[key] = s;
    os_unfair_lock_unlock(&lock);
    return s;
}

+ (NSColorSpace *)sRGBColorSpace { return space_named(kCGColorSpaceSRGB); }
+ (NSColorSpace *)extendedSRGBColorSpace { return space_named(kCGColorSpaceExtendedSRGB); }
+ (NSColorSpace *)displayP3ColorSpace { return space_named(kCGColorSpaceDisplayP3); }
+ (NSColorSpace *)adobeRGB1998ColorSpace { return space_named(kCGColorSpaceAdobeRGB1998); }
+ (NSColorSpace *)genericRGBColorSpace {
#if TARGET_OS_OSX
    return space_named(kCGColorSpaceGenericRGB);
#else
    return space_named(kCGColorSpaceSRGB);
#endif
}
+ (NSColorSpace *)genericGrayColorSpace { return space_named(kCGColorSpaceGenericGrayGamma2_2); }
+ (NSColorSpace *)genericGamma22GrayColorSpace { return space_named(kCGColorSpaceGenericGrayGamma2_2); }
+ (NSColorSpace *)extendedGenericGamma22GrayColorSpace { return space_named(kCGColorSpaceExtendedGray); }
+ (NSColorSpace *)genericCMYKColorSpace { return space_named(kCGColorSpaceGenericCMYK); }
+ (NSColorSpace *)deviceRGBColorSpace {
    static NSColorSpace *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        CGColorSpaceRef cg = CGColorSpaceCreateDeviceRGB();
        s = [[NSColorSpace alloc] initWithCGColorSpace:cg];
        CGColorSpaceRelease(cg);
    });
    return s;
}
+ (NSColorSpace *)deviceGrayColorSpace {
    static NSColorSpace *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        CGColorSpaceRef cg = CGColorSpaceCreateDeviceGray();
        s = [[NSColorSpace alloc] initWithCGColorSpace:cg];
        CGColorSpaceRelease(cg);
    });
    return s;
}
+ (NSColorSpace *)deviceCMYKColorSpace {
    static NSColorSpace *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        CGColorSpaceRef cg = CGColorSpaceCreateDeviceCMYK();
        s = [[NSColorSpace alloc] initWithCGColorSpace:cg];
        CGColorSpaceRelease(cg);
    });
    return s;
}
+ (NSColorSpace *)colorSpaceForCGColorSpace:(CGColorSpaceRef)space { return [[self alloc] initWithCGColorSpace:space]; }
+ (NSArray<NSColorSpace *> *)availableColorSpacesWithModel:(NSInteger)model {
    if (model == 0) return @[ [self genericGamma22GrayColorSpace], [self deviceGrayColorSpace] ];
    if (model == 1) return @[ [self sRGBColorSpace], [self displayP3ColorSpace], [self genericRGBColorSpace] ];
    return @[];
}

- (instancetype)initWithCGColorSpace:(CGColorSpaceRef)space {
    if (!space || !(self = [super init])) return nil;
    _space = CGColorSpaceRetain(space);
    return self;
}
- (instancetype)initWithICCProfileData:(NSData *)data {
    CGColorSpaceRef cg = data ? CGColorSpaceCreateWithICCData((__bridge CFDataRef)data) : NULL;
    self = [self initWithCGColorSpace:cg];
    CGColorSpaceRelease(cg);
    return self;
}
- (void)dealloc { CGColorSpaceRelease(_space); }
- (CGColorSpaceRef)CGColorSpace { return _space; }
- (NSData *)ICCProfileData { return CFBridgingRelease(CGColorSpaceCopyICCData(_space)); }
- (NSInteger)colorSpaceModel {
    CGColorSpaceModel m = CGColorSpaceGetModel(_space);
    return m >= kCGColorSpaceModelMonochrome && m <= kCGColorSpaceModelPattern ? (NSInteger)m : -1;
}
- (NSInteger)numberOfColorComponents { return (NSInteger)CGColorSpaceGetNumberOfComponents(_space); }
- (BOOL)isWideGamut { return CGColorSpaceIsWideGamutRGB(_space); }
- (NSString *)localizedName {
    NSString *n = CFBridgingRelease(CGColorSpaceCopyName(_space));
    return n ?: @"Device";
}
- (BOOL)isEqual:(id)other {
    return self == other || ([other isKindOfClass:[NSColorSpace class]] && CFEqual(_space, ((NSColorSpace *)other)->_space));
}
- (NSUInteger)hash { return CFHash(_space); }
- (NSString *)description { return [NSString stringWithFormat:@"<NSColorSpace %@>", self.localizedName]; }
@end

/* ================================================================ colors */

@implementation NSColor {
    CGColorRef _cg;
    NSString *_catalog, *_name;
}

static BOOL g_ignores_alpha = YES;

/* Takes ownership of cg. */
static NSColor *wrap(CGColorRef cg) {
    if (!cg) return nil;
    NSColor *c = [[NSColor alloc] init];
    CGColorRelease(c->_cg);
    c->_cg = cg;
    return c;
}

static NSColor *in_space(NSColorSpace *space, const CGFloat *components) {
    return wrap(CGColorCreate(space.CGColorSpace, components));
}

static NSColor *rgb_in(NSColorSpace *space, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGFloat c[4] = {r, g, b, a};
    return in_space(space, c);
}

static NSColor *gray_in(NSColorSpace *space, CGFloat w, CGFloat a) {
    CGFloat c[2] = {w, a};
    return in_space(space, c);
}

/* Any color as sRGB components (gray and other models converted). */
static void rgba_of(CGColorRef cg, CGFloat out[4]) {
    out[0] = out[1] = out[2] = 0;
    out[3] = cg ? CGColorGetAlpha(cg) : 0;
    if (!cg) return;
    size_t n = CGColorGetNumberOfComponents(cg);
    const CGFloat *c = CGColorGetComponents(cg);
    CGColorSpaceModel m = CGColorSpaceGetModel(CGColorGetColorSpace(cg));
    if (!c) return;
    if (m == kCGColorSpaceModelRGB && n >= 4) {
        out[0] = c[0], out[1] = c[1], out[2] = c[2];
        return;
    }
    if (m == kCGColorSpaceModelMonochrome && n >= 2) {
        out[0] = out[1] = out[2] = c[0];
        return;
    }
    CGColorRef s = CGColorCreateCopyByMatchingToColorSpace([NSColorSpace sRGBColorSpace].CGColorSpace,
                                                           kCGRenderingIntentDefault, cg, NULL);
    if (s && CGColorGetNumberOfComponents(s) >= 4) {
        const CGFloat *sc = CGColorGetComponents(s);
        out[0] = sc[0], out[1] = sc[1], out[2] = sc[2];
    }
    if (s) CGColorRelease(s);
}

static void hsb_to_rgb(CGFloat h, CGFloat s, CGFloat v, CGFloat out[3]) {
    h = fmod(h, 1.0);
    if (h < 0) h += 1;
    CGFloat f = h * 6, p = v * (1 - s);
    int i = (int)floor(f) % 6;
    CGFloat q = v * (1 - s * (f - floor(f))), t = v * (1 - s * (1 - (f - floor(f))));
    CGFloat tab[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    memcpy(out, tab[i], sizeof(CGFloat) * 3);
}

static void rgb_to_hsb(const CGFloat c[3], CGFloat *h, CGFloat *s, CGFloat *v) {
    CGFloat mx = fmax(c[0], fmax(c[1], c[2])), mn = fmin(c[0], fmin(c[1], c[2])), d = mx - mn, hue = 0;
    if (d > 0) {
        if (mx == c[0]) hue = fmod((c[1] - c[2]) / d, 6);
        else if (mx == c[1]) hue = (c[2] - c[0]) / d + 2;
        else hue = (c[0] - c[1]) / d + 4;
        hue /= 6;
        if (hue < 0) hue += 1;
    }
    if (h) *h = hue;
    if (s) *s = mx > 0 ? d / mx : 0;
    if (v) *v = mx;
}

- (instancetype)init {
    if ((self = [super init])) {
        CGFloat black[2] = {0, 1};
        _cg = CGColorCreate([NSColorSpace deviceGrayColorSpace].CGColorSpace, black);
    }
    return self;
}
- (void)dealloc { CGColorRelease(_cg); }

+ (BOOL)ignoresAlpha { return g_ignores_alpha; }
+ (void)setIgnoresAlpha:(BOOL)flag { g_ignores_alpha = flag; }
+ (BOOL)supportsSecureCoding { return YES; }

+ (NSColor *)colorWithCGColor:(CGColorRef)color { return color ? wrap(CGColorRetain(color)) : nil; }
+ (NSColor *)colorWithSRGBRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a {
    return rgb_in([NSColorSpace sRGBColorSpace], r, g, b, a);
}
+ (NSColor *)colorWithRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a {
    return rgb_in([NSColorSpace extendedSRGBColorSpace], r, g, b, a);
}
+ (NSColor *)colorWithDisplayP3Red:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a {
    return rgb_in([NSColorSpace displayP3ColorSpace], r, g, b, a);
}
+ (NSColor *)colorWithDeviceRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a {
    return rgb_in([NSColorSpace deviceRGBColorSpace], r, g, b, a);
}
+ (NSColor *)colorWithCalibratedRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a {
    return rgb_in([NSColorSpace genericRGBColorSpace], r, g, b, a);
}
+ (NSColor *)colorWithWhite:(CGFloat)w alpha:(CGFloat)a { return gray_in([NSColorSpace extendedGenericGamma22GrayColorSpace], w, a); }
+ (NSColor *)colorWithDeviceWhite:(CGFloat)w alpha:(CGFloat)a { return gray_in([NSColorSpace deviceGrayColorSpace], w, a); }
+ (NSColor *)colorWithCalibratedWhite:(CGFloat)w alpha:(CGFloat)a { return gray_in([NSColorSpace genericGrayColorSpace], w, a); }
+ (NSColor *)colorWithGenericGamma22White:(CGFloat)w alpha:(CGFloat)a {
    return gray_in([NSColorSpace genericGamma22GrayColorSpace], w, a);
}
+ (NSColor *)colorWithHue:(CGFloat)h saturation:(CGFloat)s brightness:(CGFloat)v alpha:(CGFloat)a {
    CGFloat c[3];
    hsb_to_rgb(h, s, v, c);
    return rgb_in([NSColorSpace sRGBColorSpace], c[0], c[1], c[2], a);
}
+ (NSColor *)colorWithDeviceHue:(CGFloat)h saturation:(CGFloat)s brightness:(CGFloat)v alpha:(CGFloat)a {
    CGFloat c[3];
    hsb_to_rgb(h, s, v, c);
    return rgb_in([NSColorSpace deviceRGBColorSpace], c[0], c[1], c[2], a);
}
+ (NSColor *)colorWithCalibratedHue:(CGFloat)h saturation:(CGFloat)s brightness:(CGFloat)v alpha:(CGFloat)a {
    CGFloat c[3];
    hsb_to_rgb(h, s, v, c);
    return rgb_in([NSColorSpace genericRGBColorSpace], c[0], c[1], c[2], a);
}
+ (NSColor *)colorWithColorSpace:(NSColorSpace *)space components:(const CGFloat *)components count:(NSInteger)count {
    if (!space || !components) return nil;
    NSInteger n = space.numberOfColorComponents;
    if (count >= n + 1) return in_space(space, components);
    CGFloat c[16] = {0};
    for (NSInteger i = 0; i < n && i < count && i < 15; i++) c[i] = components[i];
    c[n < 15 ? n : 15] = 1;
    return in_space(space, c);
}
+ (NSColor *)colorWithName:(NSString *)name dynamicProvider:(NSColor * (^)(NSAppearance *))provider {
    return provider ? provider([NSAppearance currentDrawingAppearance]) : nil;
}
+ (NSColor *)colorFromPasteboard:(id)pasteboard { return nil; }

/* System (catalog) colors: the light appearance, the blue accent. */
static NSColor *system_color(NSString *name, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    static NSMutableDictionary *cache;
    os_unfair_lock_lock(&lock);
    NSColor *c = cache[name];
    os_unfair_lock_unlock(&lock);
    if (c) return c;
    c = rgb_in([NSColorSpace sRGBColorSpace], r, g, b, a);
    c->_catalog = @"System";
    c->_name = name;
    os_unfair_lock_lock(&lock);
    if (!cache) cache = [NSMutableDictionary dictionary];
    if (cache[name]) c = cache[name];
    else cache[name] = c;
    os_unfair_lock_unlock(&lock);
    return c;
}
#define SYS(sel, r, g, b, a) \
    +(NSColor *)sel { return system_color(@ #sel, r, g, b, a); }
SYS(blackColor, 0, 0, 0, 1) SYS(whiteColor, 1, 1, 1, 1) SYS(clearColor, 0, 0, 0, 0) SYS(grayColor, 0.5, 0.5, 0.5, 1)
SYS(darkGrayColor, 1 / 3.0, 1 / 3.0, 1 / 3.0, 1) SYS(lightGrayColor, 2 / 3.0, 2 / 3.0, 2 / 3.0, 1)
SYS(redColor, 1, 0, 0, 1) SYS(greenColor, 0, 1, 0, 1) SYS(blueColor, 0, 0, 1, 1) SYS(cyanColor, 0, 1, 1, 1)
SYS(yellowColor, 1, 1, 0, 1) SYS(magentaColor, 1, 0, 1, 1) SYS(orangeColor, 1, 0.5, 0, 1) SYS(purpleColor, 0.5, 0, 0.5, 1)
SYS(brownColor, 0.6, 0.4, 0.2, 1)
SYS(systemBlueColor, 0, 0.5333, 1, 1) SYS(systemRedColor, 1, 0.2196, 0.2353, 1) SYS(systemGreenColor, 0.2039, 0.7804, 0.349, 1)
SYS(systemOrangeColor, 1, 0.5529, 0.1569, 1) SYS(systemYellowColor, 1, 0.8, 0, 1) SYS(systemPurpleColor, 0.7961, 0.1882, 0.8784, 1)
SYS(systemPinkColor, 1, 0.1765, 0.3333, 1) SYS(systemGrayColor, 0.5569, 0.5569, 0.5765, 1)
SYS(systemBrownColor, 0.6745, 0.498, 0.3686, 1) SYS(systemTealColor, 0, 0.7647, 0.8157, 1)
SYS(systemIndigoColor, 0.3804, 0.3333, 0.9608, 1) SYS(systemMintColor, 0, 0.7843, 0.702, 1) SYS(systemCyanColor, 0, 0.7529, 0.9098, 1)
SYS(controlAccentColor, 0, 0.5333, 1, 1) SYS(keyboardFocusIndicatorColor, 0, 0.5333, 1, 0.5)
SYS(controlBackgroundColor, 1, 1, 1, 1) SYS(windowBackgroundColor, 1, 1, 1, 1) SYS(underPageBackgroundColor, 0.9647, 0.9647, 0.9647, 1)
SYS(textColor, 0, 0, 0, 1) SYS(textBackgroundColor, 1, 1, 1, 1) SYS(selectedTextColor, 0, 0, 0, 1)
SYS(selectedTextBackgroundColor, 0.702, 0.8431, 1, 1) SYS(unemphasizedSelectedTextBackgroundColor, 0.8627, 0.8627, 0.8627, 1)
SYS(unemphasizedSelectedTextColor, 0, 0, 0, 1) SYS(selectedContentBackgroundColor, 0, 0.3922, 0.8824, 1)
SYS(unemphasizedSelectedContentBackgroundColor, 0.8627, 0.8627, 0.8627, 1)
SYS(labelColor, 0, 0, 0, 0.8471) SYS(secondaryLabelColor, 0, 0, 0, 0.498) SYS(tertiaryLabelColor, 0, 0, 0, 0.2588)
SYS(quaternaryLabelColor, 0, 0, 0, 0.098) SYS(quinaryLabelColor, 0, 0, 0, 0.0471)
SYS(systemFillColor, 0, 0, 0, 0.098) SYS(secondarySystemFillColor, 0, 0, 0, 0.0784) SYS(tertiarySystemFillColor, 0, 0, 0, 0.0471)
SYS(quaternarySystemFillColor, 0, 0, 0, 0.0275) SYS(quinarySystemFillColor, 0, 0, 0, 0.0078)
SYS(linkColor, 0, 0.4078, 0.8549, 1) SYS(placeholderTextColor, 0, 0, 0, 0.498) SYS(separatorColor, 0, 0, 0, 0.1)
SYS(gridColor, 0.902, 0.902, 0.902, 1) SYS(headerTextColor, 0, 0, 0, 0.8471) SYS(windowFrameTextColor, 0, 0, 0, 0.8471)
SYS(windowFrameColor, 0.6667, 0.6667, 0.6667, 1) SYS(findHighlightColor, 1, 1, 0, 1)
SYS(controlColor, 1, 1, 1, 1) SYS(controlTextColor, 0, 0, 0, 0.8471) SYS(disabledControlTextColor, 0, 0, 0, 0.2471)
SYS(selectedControlColor, 0.702, 0.8431, 1, 1) SYS(selectedControlTextColor, 0, 0, 0, 0.8471)
SYS(alternateSelectedControlColor, 0, 0.3922, 0.8824, 1) SYS(alternateSelectedControlTextColor, 1, 1, 1, 1)
SYS(secondarySelectedControlColor, 0.8627, 0.8627, 0.8627, 1) SYS(controlHighlightColor, 1, 1, 1, 1)
SYS(controlLightHighlightColor, 1, 1, 1, 1) SYS(controlShadowColor, 0, 0, 0, 0.2667) SYS(controlDarkShadowColor, 0, 0, 0, 1)
SYS(highlightColor, 1, 1, 1, 1) SYS(shadowColor, 0, 0, 0, 1) SYS(scrollBarColor, 0.6667, 0.6667, 0.6667, 1)
SYS(knobColor, 0.6, 0.6, 0.7333, 1) SYS(selectedKnobColor, 0.4, 0.4, 0.6, 1)
SYS(selectedMenuItemColor, 0, 0.3922, 0.8824, 1) SYS(selectedMenuItemTextColor, 1, 1, 1, 1)
SYS(scrubberTexturedBackgroundColor, 0, 0, 0, 0) SYS(controlAlternatingRowColor, 0.9569, 0.9608, 0.9608, 1)
#undef SYS
+ (NSArray<NSColor *> *)alternatingContentBackgroundColors {
    return @[ [self controlBackgroundColor], [self controlAlternatingRowColor] ];
}
+ (NSArray<NSColor *> *)controlAlternatingRowBackgroundColors { return [self alternatingContentBackgroundColors]; }
+ (NSColor *)colorWithCatalogName:(NSString *)catalog colorName:(NSString *)name {
    if (![catalog isEqualToString:@"System"] || !name.length) return nil;
    SEL sel = NSSelectorFromString(name);
    Method m = class_getClassMethod(self, sel);
    if (!m || method_getNumberOfArguments(m) != 2 || [name hasPrefix:@"colorWith"]) return nil;
    return ((NSColor * (*)(id, SEL)) method_getImplementation(m))(self, sel);
}

- (CGColorRef)CGColor { return _cg; }
- (NSInteger)type { return _catalog ? 2 : 0; }
- (NSColor *)colorUsingType:(NSInteger)type {
    if (type == 0) return _catalog ? wrap(CGColorRetain(_cg)) : self;
    return type == 2 && _catalog ? self : nil;
}
- (NSString *)catalogNameComponent { return _catalog; }
- (NSString *)colorNameComponent { return _name; }
- (NSString *)localizedCatalogNameComponent { return _catalog; }
- (NSString *)localizedColorNameComponent { return _name; }
- (NSColorSpace *)colorSpace {
    CGColorSpaceRef cs = CGColorGetColorSpace(_cg);
    NSColorSpace *srgb = [NSColorSpace sRGBColorSpace];
    return CFEqual(cs, srgb.CGColorSpace) ? srgb : [[NSColorSpace alloc] initWithCGColorSpace:cs];
}
- (NSColorSpaceName)colorSpaceName {
    if (_catalog) return @"NSNamedColorSpace";
    CGColorSpaceRef cs = CGColorGetColorSpace(_cg);
    switch (CGColorSpaceGetModel(cs)) {
    case kCGColorSpaceModelMonochrome:
        return CFEqual(cs, [NSColorSpace deviceGrayColorSpace].CGColorSpace) ? @"NSDeviceWhiteColorSpace" : @"NSCalibratedWhiteColorSpace";
    case kCGColorSpaceModelRGB:
        if (CFEqual(cs, [NSColorSpace deviceRGBColorSpace].CGColorSpace)) return @"NSDeviceRGBColorSpace";
        return CFEqual(cs, [NSColorSpace genericRGBColorSpace].CGColorSpace) ? @"NSCalibratedRGBColorSpace" : @"NSCustomColorSpace";
    case kCGColorSpaceModelCMYK: return @"NSDeviceCMYKColorSpace";
    case kCGColorSpaceModelPattern: return @"NSPatternColorSpace";
    default: return @"NSCustomColorSpace";
    }
}
- (NSColor *)colorUsingColorSpace:(NSColorSpace *)space {
    if (!space) return nil;
    if (CFEqual(CGColorGetColorSpace(_cg), space.CGColorSpace)) return self;
    return wrap(CGColorCreateCopyByMatchingToColorSpace(space.CGColorSpace, kCGRenderingIntentDefault, _cg, NULL));
}
- (NSColor *)colorUsingColorSpaceName:(NSColorSpaceName)name {
    if (!name || [name isEqualToString:@"NSCustomColorSpace"]) return self;
    if ([name isEqualToString:@"NSNamedColorSpace"]) return _catalog ? self : nil;
    NSColorSpace *s = [name isEqualToString:@"NSDeviceRGBColorSpace"]       ? [NSColorSpace deviceRGBColorSpace]
                      : [name isEqualToString:@"NSCalibratedRGBColorSpace"] ? [NSColorSpace genericRGBColorSpace]
                      : [name isEqualToString:@"NSDeviceWhiteColorSpace"]   ? [NSColorSpace deviceGrayColorSpace]
                      : [name isEqualToString:@"NSCalibratedWhiteColorSpace"] ? [NSColorSpace genericGrayColorSpace]
                      : [name isEqualToString:@"NSDeviceCMYKColorSpace"]    ? [NSColorSpace deviceCMYKColorSpace]
                                                                            : nil;
    return s ? [self colorUsingColorSpace:s] : nil;
}
- (NSColor *)colorUsingColorSpaceName:(NSColorSpaceName)name device:(NSDictionary *)device {
    return [self colorUsingColorSpaceName:name];
}
- (NSInteger)numberOfComponents { return (NSInteger)CGColorGetNumberOfComponents(_cg); }
- (void)getComponents:(CGFloat *)components {
    const CGFloat *c = CGColorGetComponents(_cg);
    if (components && c) memcpy(components, c, sizeof(CGFloat) * CGColorGetNumberOfComponents(_cg));
}
- (void)getRed:(CGFloat *)r green:(CGFloat *)g blue:(CGFloat *)b alpha:(CGFloat *)a {
    CGFloat c[4];
    rgba_of(_cg, c);
    if (r) *r = c[0];
    if (g) *g = c[1];
    if (b) *b = c[2];
    if (a) *a = c[3];
}
- (CGFloat)redComponent { CGFloat c[4]; rgba_of(_cg, c); return c[0]; }
- (CGFloat)greenComponent { CGFloat c[4]; rgba_of(_cg, c); return c[1]; }
- (CGFloat)blueComponent { CGFloat c[4]; rgba_of(_cg, c); return c[2]; }
- (CGFloat)alphaComponent { return CGColorGetAlpha(_cg); }
- (CGFloat)whiteComponent {
    if (CGColorSpaceGetModel(CGColorGetColorSpace(_cg)) == kCGColorSpaceModelMonochrome) return CGColorGetComponents(_cg)[0];
    CGFloat c[4];
    rgba_of(_cg, c);
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
}
- (void)getWhite:(CGFloat *)white alpha:(CGFloat *)alpha {
    if (white) *white = self.whiteComponent;
    if (alpha) *alpha = CGColorGetAlpha(_cg);
}
- (void)getHue:(CGFloat *)h saturation:(CGFloat *)s brightness:(CGFloat *)v alpha:(CGFloat *)a {
    CGFloat c[4];
    rgba_of(_cg, c);
    rgb_to_hsb(c, h, s, v);
    if (a) *a = c[3];
}
- (CGFloat)hueComponent { CGFloat h; [self getHue:&h saturation:NULL brightness:NULL alpha:NULL]; return h; }
- (CGFloat)saturationComponent { CGFloat s; [self getHue:NULL saturation:&s brightness:NULL alpha:NULL]; return s; }
- (CGFloat)brightnessComponent { CGFloat v; [self getHue:NULL saturation:NULL brightness:&v alpha:NULL]; return v; }
- (NSColor *)colorWithAlphaComponent:(CGFloat)alpha { return wrap(CGColorCreateCopyWithAlpha(_cg, alpha)); }
- (NSColor *)blendedColorWithFraction:(CGFloat)fraction ofColor:(NSColor *)color {
    if (!color) return nil;
    CGFloat a[4], b[4];
    rgba_of(_cg, a);
    rgba_of(color.CGColor, b);
    fraction = fmax(0, fmin(1, fraction));
    for (int i = 0; i < 4; i++) a[i] += (b[i] - a[i]) * fraction;
    return rgb_in([NSColorSpace sRGBColorSpace], a[0], a[1], a[2], a[3]);
}
- (NSColor *)highlightWithLevel:(CGFloat)level { return [self blendedColorWithFraction:level ofColor:[NSColor whiteColor]]; }
- (NSColor *)shadowWithLevel:(CGFloat)level { return [self blendedColorWithFraction:level ofColor:[NSColor blackColor]]; }
- (NSColor *)colorWithSystemEffect:(NSInteger)effect { return self; }
- (void)set {
    [self setFill];
    [self setStroke];
}
- (void)setFill {
    CGContextRef c = current_cg();
    if (c) CGContextSetFillColorWithColor(c, _cg);
}
- (void)setStroke {
    CGContextRef c = current_cg();
    if (c) CGContextSetStrokeColorWithColor(c, _cg);
}
- (void)drawSwatchInRect:(NSRect)rect {
    CGContextRef c = current_cg();
    if (!c) return;
    CGContextSaveGState(c);
    CGContextSetFillColorWithColor(c, _cg);
    CGContextFillRect(c, rect);
    CGContextRestoreGState(c);
}
- (id)copyWithZone:(NSZone *)zone { return self; }
- (BOOL)isEqual:(id)other {
    return self == other || ([other isKindOfClass:[NSColor class]] && CGColorEqualToColor(_cg, ((NSColor *)other)->_cg));
}
- (NSUInteger)hash {
    CGFloat c[4];
    rgba_of(_cg, c);
    return (NSUInteger)(c[0] * 255) << 24 ^ (NSUInteger)(c[1] * 255) << 16 ^ (NSUInteger)(c[2] * 255) << 8 ^ (NSUInteger)(c[3] * 255);
}
- (NSString *)description {
    if (_catalog) return [NSString stringWithFormat:@"%@ %@", _catalog, _name];
    CGFloat c[4];
    rgba_of(_cg, c);
    return [NSString stringWithFormat:@"%@ %g %g %g %g", self.colorSpaceName, c[0], c[1], c[2], c[3]];
}
- (void)encodeWithCoder:(NSCoder *)coder {}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
@end

/* ================================================================ fonts */

__attribute__((objc_runtime_name("HKFontDescriptor"))) @interface NSFontDescriptor : HKShimObject
@end

static BOOL is_ct_font(id o) { return o && CFGetTypeID((__bridge CFTypeRef)o) == CTFontGetTypeID(); }
static BOOL is_ct_desc(id o) { return o && CFGetTypeID((__bridge CFTypeRef)o) == CTFontDescriptorGetTypeID(); }

/* AppKit caches the fonts its class methods hand out, and apps rely on it: Chromium keeps only an unretained
   CTFontRef to [NSFont systemFontOfSize:] once ARC has let go. So every font made here stays alive. */
static NSFont *own_font(CTFontRef f) {
    if (!f) return nil;
    static NSMutableSet *kept;
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    id font = hk_font_adopt(CFBridgingRelease(f));
    os_unfair_lock_lock(&lock);
    if (!kept) kept = [NSMutableSet new];
    id same = [kept member:font];
    if (same) font = same;
    else [kept addObject:font];
    os_unfair_lock_unlock(&lock);
    return font;
}
static NSFontDescriptor *own_desc(CTFontDescriptorRef d) { return d ? hk_font_adopt(CFBridgingRelease(d)) : nil; }

/* NSFont's and NSFontDescriptor's instance methods, copied onto the class CTFonts and CTFontDescriptors have. self is
   then a CTFont or a CTFontDescriptor, or (under __NSCFType) any other unbridged CF object, which is refused as
   NSObject would. */
@interface HKFontMethods : NSObject
@end
@implementation HKFontMethods
#if TARGET_OS_OSX
/* Mac test runners only: macOS's UIFoundation measures text the AppKit way and asks AppKit's fonts for these. iOS's
   is UIKit's, and its fonts are UIKit's own, which have what it needs. */
- (id)_fontForCurrentAppearance { return self; }
#endif
#define FONT_ONLY(...) \
    if (!is_ct_font(self)) { \
        [self doesNotRecognizeSelector:_cmd]; \
        return __VA_ARGS__; \
    } \
    CTFontRef f = (__bridge CTFontRef)(id)self; \
    (void)f;
#define DESC_ONLY(...) \
    if (!is_ct_desc(self)) { \
        [self doesNotRecognizeSelector:_cmd]; \
        return __VA_ARGS__; \
    } \
    CTFontDescriptorRef d = (__bridge CTFontDescriptorRef)(id)self; \
    (void)d;

- (id)copyWithZone:(NSZone *)zone {
    if (!is_ct_font(self) && !is_ct_desc(self)) [self doesNotRecognizeSelector:_cmd];
    return self;
}
- (CGFloat)pointSize {
    if (is_ct_font(self)) return CTFontGetSize((__bridge CTFontRef)(id)self);
    if (is_ct_desc(self)) {
        NSNumber *n = CFBridgingRelease(CTFontDescriptorCopyAttribute((__bridge CTFontDescriptorRef)(id)self, kCTFontSizeAttribute));
        return n.doubleValue;
    }
    [self doesNotRecognizeSelector:_cmd];
    return 0;
}

/* NSFont */
- (NSString *)fontName { FONT_ONLY(nil) return CFBridgingRelease(CTFontCopyPostScriptName(f)); }
- (NSString *)familyName { FONT_ONLY(nil) return CFBridgingRelease(CTFontCopyFamilyName(f)); }
- (NSString *)displayName { FONT_ONLY(nil) return CFBridgingRelease(CTFontCopyDisplayName(f)); }
- (NSFontDescriptor *)fontDescriptor { FONT_ONLY(nil) return own_desc(CTFontCopyFontDescriptor(f)); }
- (CGFloat)ascender { FONT_ONLY(0) return CTFontGetAscent(f); }
- (CGFloat)descender { FONT_ONLY(0) return -CTFontGetDescent(f); }
- (CGFloat)leading { FONT_ONLY(0) return CTFontGetLeading(f); }
- (CGFloat)capHeight { FONT_ONLY(0) return CTFontGetCapHeight(f); }
- (CGFloat)xHeight { FONT_ONLY(0) return CTFontGetXHeight(f); }
- (CGFloat)italicAngle { FONT_ONLY(0) return CTFontGetSlantAngle(f); }
- (CGFloat)underlinePosition { FONT_ONLY(0) return CTFontGetUnderlinePosition(f); }
- (CGFloat)underlineThickness { FONT_ONLY(0) return CTFontGetUnderlineThickness(f); }
- (NSRect)boundingRectForFont { FONT_ONLY(NSZeroRect) return CTFontGetBoundingBox(f); }
- (BOOL)isFixedPitch { FONT_ONLY(NO) return (CTFontGetSymbolicTraits(f) & kCTFontTraitMonoSpace) != 0; }
- (BOOL)isVertical { FONT_ONLY(NO) return (CTFontGetSymbolicTraits(f) & kCTFontTraitVertical) != 0; }
- (NSUInteger)numberOfGlyphs { FONT_ONLY(0) return (NSUInteger)CTFontGetGlyphCount(f); }
- (NSSize)maximumAdvancement { FONT_ONLY(NSZeroSize) return NSMakeSize(CTFontGetBoundingBox(f).size.width, 0); }
- (NSUInteger)mostCompatibleStringEncoding { FONT_ONLY(0) return NSUTF8StringEncoding; }
- (NSSize)advancementForCGGlyph:(CGGlyph)glyph {
    FONT_ONLY(NSZeroSize)
    CGSize adv = CGSizeZero;
    CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &glyph, &adv, 1);
    return adv;
}
- (NSSize)advancementForGlyph:(unsigned int)glyph { return [self advancementForCGGlyph:(CGGlyph)glyph]; }
- (NSRect)boundingRectForCGGlyph:(CGGlyph)glyph {
    FONT_ONLY(NSZeroRect)
    CGRect r = CGRectZero;
    CTFontGetBoundingRectsForGlyphs(f, kCTFontOrientationHorizontal, &glyph, &r, 1);
    return r;
}
- (NSRect)boundingRectForGlyph:(unsigned int)glyph { return [self boundingRectForCGGlyph:(CGGlyph)glyph]; }
- (void)getAdvancements:(NSSize *)advancements forCGGlyphs:(const CGGlyph *)glyphs count:(NSUInteger)count {
    FONT_ONLY()
    CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, glyphs, advancements, (CFIndex)count);
}
- (void)getBoundingRects:(NSRect *)rects forCGGlyphs:(const CGGlyph *)glyphs count:(NSUInteger)count {
    FONT_ONLY()
    CTFontGetBoundingRectsForGlyphs(f, kCTFontOrientationHorizontal, glyphs, rects, (CFIndex)count);
}
- (unsigned int)glyphWithName:(NSString *)name {
    FONT_ONLY(0)
    return name ? CTFontGetGlyphWithName(f, (__bridge CFStringRef)name) : 0;
}
- (NSCharacterSet *)coveredCharacterSet { FONT_ONLY(nil) return CFBridgingRelease(CTFontCopyCharacterSet(f)); }
- (NSFont *)fontWithSize:(CGFloat)size { FONT_ONLY(nil) return own_font(CTFontCreateCopyWithAttributes(f, size, NULL, NULL)); }
- (NSFont *)screenFont { FONT_ONLY(nil) return (NSFont *)self; }
- (NSFont *)printerFont { FONT_ONLY(nil) return (NSFont *)self; }
- (NSFont *)verticalFont { FONT_ONLY(nil) return (NSFont *)self; }
- (NSFont *)screenFontWithRenderingMode:(NSUInteger)mode { FONT_ONLY(nil) return (NSFont *)self; }
- (NSUInteger)renderingMode { FONT_ONLY(0) return 0; }
- (void)setInContext:(NSGraphicsContext *)context {
    FONT_ONLY()
    CGContextRef c = context.CGContext;
    if (!c) return;
    CGFontRef g = CTFontCopyGraphicsFont(f, NULL);
    CGContextSetFont(c, g);
    CGContextSetFontSize(c, CTFontGetSize(f));
    CGFontRelease(g);
}
- (void)set { [self setInContext:[NSGraphicsContext currentContext]]; }

/* NSFontDescriptor */
- (NSDictionary *)fontAttributes { DESC_ONLY(nil) return CFBridgingRelease(CTFontDescriptorCopyAttributes(d)); }
- (id)objectForKey:(NSString *)key {
    DESC_ONLY(nil)
    return key ? CFBridgingRelease(CTFontDescriptorCopyAttribute(d, (__bridge CFStringRef)key)) : nil;
}
- (NSString *)postscriptName { DESC_ONLY(nil) return CFBridgingRelease(CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute)); }
- (uint32_t)symbolicTraits {
    DESC_ONLY(0)
    NSDictionary *t = CFBridgingRelease(CTFontDescriptorCopyAttribute(d, kCTFontTraitsAttribute));
    return [t[(id)kCTFontSymbolicTrait] unsignedIntValue];
}
- (NSFontDescriptor *)fontDescriptorWithSymbolicTraits:(uint32_t)traits {
    DESC_ONLY(nil)
    return own_desc(CTFontDescriptorCreateCopyWithSymbolicTraits(d, traits, 0xffffffffu));
}
- (NSFontDescriptor *)fontDescriptorByAddingAttributes:(NSDictionary *)attributes {
    DESC_ONLY(nil)
    return own_desc(CTFontDescriptorCreateCopyWithAttributes(d, (__bridge CFDictionaryRef)(attributes ?: @{})));
}
- (NSFontDescriptor *)fontDescriptorWithSize:(CGFloat)size {
    DESC_ONLY(nil)
    return own_desc(CTFontDescriptorCreateCopyWithAttributes(d, (__bridge CFDictionaryRef) @{(id)kCTFontSizeAttribute : @(size)}));
}
- (NSFontDescriptor *)fontDescriptorWithFamily:(NSString *)family {
    DESC_ONLY(nil)
    return family ? own_desc(CTFontDescriptorCreateCopyWithFamily(d, (__bridge CFStringRef)family)) : (NSFontDescriptor *)self;
}
- (NSFontDescriptor *)fontDescriptorWithFace:(NSString *)face {
    DESC_ONLY(nil)
    if (!face) return (NSFontDescriptor *)self;
    return own_desc(CTFontDescriptorCreateCopyWithAttributes(d, (__bridge CFDictionaryRef) @{(id)kCTFontStyleNameAttribute : face}));
}
- (NSFontDescriptor *)fontDescriptorWithDesign:(NSString *)design { DESC_ONLY(nil) return nil; }
- (NSArray *)matchingFontDescriptorsWithMandatoryKeys:(NSSet *)keys {
    DESC_ONLY(nil)
    NSArray *a = CFBridgingRelease(CTFontDescriptorCreateMatchingFontDescriptors(d, (__bridge CFSetRef)keys));
    for (id m in a) hk_font_adopt(m);
    return a ?: @[];
}
- (NSFontDescriptor *)matchingFontDescriptorWithMandatoryKeys:(NSSet *)keys {
    DESC_ONLY(nil)
    return own_desc(CTFontDescriptorCreateMatchingFontDescriptor(d, (__bridge CFSetRef)keys));
}
- (BOOL)requiresFontAssetRequest { DESC_ONLY(NO) return NO; }
#undef FONT_ONLY
#undef DESC_ONLY
@end

static void add_font_methods(Class cls) {
    unsigned n = 0;
    Method *ms = class_copyMethodList([HKFontMethods class], &n);
    for (unsigned i = 0; i < n; i++) {
        SEL s = method_getName(ms[i]);
        if (!class_respondsToSelector(cls, s))
            class_addMethod(cls, s, method_getImplementation(ms[i]), method_getTypeEncoding(ms[i]));
    }
    free(ms);
}

id hk_font_adopt(id obj) {
    static _Atomic(Class) known[16];
    static _Atomic int nknown;
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    if (!obj) {
        CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13, NULL);
        CTFontDescriptorRef d = f ? CTFontCopyFontDescriptor(f) : NULL;
        if (f) hk_font_adopt((__bridge id)f);
        if (d) hk_font_adopt((__bridge id)d);
        if (d) CFRelease(d);
        if (f) CFRelease(f);
        return nil;
    }
    Class cls = object_getClass(obj);
    int n = atomic_load(&nknown);
    for (int i = 0; i < n; i++)
        if (atomic_load(&known[i]) == cls) return obj;
    os_unfair_lock_lock(&lock);
    n = atomic_load(&nknown);
    BOOL seen = NO;
    for (int i = 0; i < n; i++) seen |= atomic_load(&known[i]) == cls;
    if (!seen) {
        add_font_methods(cls);
        if (n < 16) {
            atomic_store(&known[n], cls);
            atomic_store(&nknown, n + 1);
        }
    }
    os_unfair_lock_unlock(&lock);
    return obj;
}

static CTFontRef ui_font(CTFontUIFontType type, CGFloat size, CGFloat fallback) {
    return CTFontCreateUIFontForLanguage(type, size > 0 ? size : fallback, NULL);
}

/* The font with the weight trait (NSFontWeight, -1...1) asked for, from the same family. */
static CTFontRef with_weight(CTFontRef base, CGFloat weight) {
    if (!base) return NULL;
    CTFontDescriptorRef d = CTFontCopyFontDescriptor(base);
    NSDictionary *attrs = @{(id)kCTFontTraitsAttribute : @{(id)kCTFontWeightTrait : @(weight)}};
    CTFontDescriptorRef w = CTFontDescriptorCreateCopyWithAttributes(d, (__bridge CFDictionaryRef)attrs);
    CTFontRef f = CTFontCreateWithFontDescriptor(w, CTFontGetSize(base), NULL);
    CFRelease(w);
    CFRelease(d);
    CFRelease(base);
    return f;
}

static BOOL same_name(NSString *a, NSString *b) { return a && b && [a caseInsensitiveCompare:b] == NSOrderedSame; }

@implementation NSFont
/* No warm-up at load: the font methods go onto the system's font classes only once a Mac app is loaded (the hosts
   call hk_font_adopt(nil) then), never in a launch that runs none. */
+ (void)initialize {
    if (self == [NSFont class]) hk_font_adopt(nil);
}
+ (CGFloat)systemFontSize { return 13; }
+ (CGFloat)smallSystemFontSize { return 11; }
+ (CGFloat)labelFontSize { return 10; }
+ (CGFloat)systemFontSizeForControlSize:(NSControlSize)size {
    switch (size) {
    case NSControlSizeSmall: return 11;
    case NSControlSizeMini: return 9;
    default: return 13;
    }
}
+ (NSFont *)systemFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 13)); }
+ (NSFont *)boldSystemFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontEmphasizedSystem, size, 13)); }
+ (NSFont *)systemFontOfSize:(CGFloat)size weight:(NSFontWeight)weight {
    return own_font(with_weight(ui_font(kCTFontUIFontSystem, size, 13), weight));
}
+ (NSFont *)systemFontOfSize:(CGFloat)size weight:(NSFontWeight)weight width:(CGFloat)width {
    return [self systemFontOfSize:size weight:weight];
}
+ (NSFont *)monospacedSystemFontOfSize:(CGFloat)size weight:(NSFontWeight)weight {
    return own_font(with_weight(ui_font(kCTFontUIFontUserFixedPitch, size, 13), weight));
}
+ (NSFont *)monospacedDigitSystemFontOfSize:(CGFloat)size weight:(NSFontWeight)weight {
    CTFontRef base = with_weight(ui_font(kCTFontUIFontSystem, size, 13), weight);
    if (!base) return nil;
    NSDictionary *feature = @{(id)kCTFontFeatureTypeIdentifierKey : @6, (id)kCTFontFeatureSelectorIdentifierKey : @0};
    CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(
        (__bridge CFDictionaryRef) @{(id)kCTFontFeatureSettingsAttribute : @[ feature ]});
    CTFontRef f = CTFontCreateCopyWithAttributes(base, 0, NULL, d);
    CFRelease(d);
    CFRelease(base);
    return own_font(f);
}
+ (NSFont *)labelFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 10)); }
+ (NSFont *)messageFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 13)); }
+ (NSFont *)controlContentFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 13)); }
+ (NSFont *)menuFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 13)); }
+ (NSFont *)menuBarFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 13)); }
+ (NSFont *)titleBarFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontEmphasizedSystem, size, 13)); }
+ (NSFont *)paletteFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 11)); }
+ (NSFont *)toolTipsFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontSystem, size, 11)); }
+ (NSFont *)userFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontUser, size, 12)); }
+ (NSFont *)userFixedPitchFontOfSize:(CGFloat)size { return own_font(ui_font(kCTFontUIFontUserFixedPitch, size, 12)); }
+ (NSFont *)fontWithName:(NSString *)name size:(CGFloat)size {
    if (!name.length) return nil;
    CTFontRef f = CTFontCreateWithName((__bridge CFStringRef)name, size > 0 ? size : 12, NULL);
    if (!f) return nil;
    /* CoreText substitutes a fallback for a name it does not know; AppKit answers nil. */
    if (![name hasPrefix:@"."]) {
        NSString *ps = CFBridgingRelease(CTFontCopyPostScriptName(f)), *family = CFBridgingRelease(CTFontCopyFamilyName(f)),
                 *full = CFBridgingRelease(CTFontCopyFullName(f));
        if (!same_name(ps, name) && !same_name(family, name) && !same_name(full, name)) {
            CFRelease(f);
            return nil;
        }
    }
    return own_font(f);
}
+ (NSFont *)fontWithName:(NSString *)name matrix:(const CGFloat *)matrix {
    return [self fontWithName:name size:matrix ? matrix[3] : 0];
}
+ (NSFont *)fontWithDescriptor:(NSFontDescriptor *)descriptor size:(CGFloat)size {
    if (!is_ct_desc(descriptor)) return [self systemFontOfSize:size];
    return own_font(CTFontCreateWithFontDescriptor((__bridge CTFontDescriptorRef)(id)descriptor, size, NULL));
}
+ (NSFont *)fontWithDescriptor:(NSFontDescriptor *)descriptor textTransform:(id)transform {
    return [self fontWithDescriptor:descriptor size:0];
}
+ (NSArray *)preferredFontNames { return @[]; }
+ (void)setUserFont:(NSFont *)font {}
+ (void)setUserFixedPitchFont:(NSFont *)font {}
@end

@implementation NSFontDescriptor
+ (void)initialize {
    if (self == [NSFontDescriptor class]) hk_font_adopt(nil);
}
+ (NSFontDescriptor *)fontDescriptorWithFontAttributes:(NSDictionary *)attributes {
    return own_desc(CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)(attributes ?: @{})));
}
+ (NSFontDescriptor *)fontDescriptorWithName:(NSString *)name size:(CGFloat)size {
    return name ? own_desc(CTFontDescriptorCreateWithNameAndSize((__bridge CFStringRef)name, size)) : nil;
}
+ (NSFontDescriptor *)fontDescriptorWithName:(NSString *)name matrix:(id)matrix {
    return [self fontDescriptorWithName:name size:0];
}
+ (NSFontDescriptor *)preferredFontDescriptorForTextStyle:(NSString *)style options:(NSDictionary *)options {
    CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13, NULL);
    NSFontDescriptor *d = f ? own_desc(CTFontCopyFontDescriptor(f)) : nil;
    if (f) CFRelease(f);
    return d;
}
@end

/* ---- the font manager ---- */

enum {
    HKItalicFontMask = 0x1, HKBoldFontMask = 0x2, HKUnboldFontMask = 0x4, HKNarrowFontMask = 0x10,
    HKExpandedFontMask = 0x20, HKCondensedFontMask = 0x40, HKSmallCapsFontMask = 0x80, HKFixedPitchFontMask = 0x400,
    HKUnitalicFontMask = 0x1000000,
};

/* AppKit's 0-15 weights against CoreText's -1...1 weight trait. */
static const struct { CGFloat ct; int ak; } g_weights[] = {
    {-0.8, 2}, {-0.6, 3}, {-0.4, 4}, {0, 5}, {0.23, 6}, {0.3, 8}, {0.4, 9}, {0.56, 10}, {0.62, 11},
};
static int appkit_weight(CGFloat ct) {
    int best = 5;
    CGFloat dist = 10;
    for (size_t i = 0; i < sizeof g_weights / sizeof g_weights[0]; i++)
        if (fabs(g_weights[i].ct - ct) < dist) dist = fabs(g_weights[i].ct - ct), best = g_weights[i].ak;
    return best;
}
static CGFloat ct_weight(NSInteger ak) {
    CGFloat best = 0;
    NSInteger dist = 100;
    for (size_t i = 0; i < sizeof g_weights / sizeof g_weights[0]; i++)
        if (labs(g_weights[i].ak - ak) < dist) dist = labs(g_weights[i].ak - ak), best = g_weights[i].ct;
    return best;
}
static NSUInteger traits_of(CTFontSymbolicTraits t) {
    return t & (kCTFontTraitItalic | kCTFontTraitBold | kCTFontTraitExpanded | kCTFontTraitCondensed | kCTFontTraitMonoSpace);
}
static CGFloat weight_trait(NSDictionary *traits) { return [traits[(id)kCTFontWeightTrait] doubleValue]; }

@interface NSFontManager : HKShimObject
@end
@implementation NSFontManager {
    NSFont *_selected;
    __weak id _delegate, _target;
    SEL _action;
}
+ (void)initialize {
    if (self == [NSFontManager class]) hk_font_adopt(nil);
}
+ (NSFontManager *)sharedFontManager {
    static NSFontManager *m;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ m = [[NSFontManager alloc] init]; });
    return m;
}
+ (void)setFontManagerFactory:(Class)cls {}
+ (void)setFontPanelFactory:(Class)cls {}
- (NSArray<NSString *> *)availableFonts { return CFBridgingRelease(CTFontManagerCopyAvailablePostScriptNames()) ?: @[]; }
- (NSArray<NSString *> *)availableFontFamilies { return CFBridgingRelease(CTFontManagerCopyAvailableFontFamilyNames()) ?: @[]; }
- (NSArray *)availableMembersOfFontFamily:(NSString *)family {
    if (!family) return nil;
    CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef) @{(id)kCTFontFamilyNameAttribute : family});
    NSArray *matches = CFBridgingRelease(
        CTFontDescriptorCreateMatchingFontDescriptors(d, (__bridge CFSetRef)[NSSet setWithObject:(id)kCTFontFamilyNameAttribute]));
    CFRelease(d);
    if (!matches.count) return nil;
    NSMutableArray *out = [NSMutableArray array];
    for (id m in matches) {
        CTFontDescriptorRef md = (__bridge CTFontDescriptorRef)m;
        NSString *ps = CFBridgingRelease(CTFontDescriptorCopyAttribute(md, kCTFontNameAttribute));
        NSString *style = CFBridgingRelease(CTFontDescriptorCopyAttribute(md, kCTFontStyleNameAttribute));
        NSDictionary *traits = CFBridgingRelease(CTFontDescriptorCopyAttribute(md, kCTFontTraitsAttribute));
        [out addObject:@[
            ps ?: @"", style ?: @"", @(appkit_weight(weight_trait(traits))),
            @(traits_of([traits[(id)kCTFontSymbolicTrait] unsignedIntValue]))
        ]];
    }
    return out;
}
- (NSArray<NSString *> *)availableFontNamesWithTraits:(NSUInteger)traits {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *name in self.availableFonts) {
        CTFontRef f = CTFontCreateWithName((__bridge CFStringRef)name, 12, NULL);
        if (!f) continue;
        NSUInteger t = traits_of(CTFontGetSymbolicTraits(f));
        CFRelease(f);
        NSUInteger want = traits & ~(NSUInteger)(HKUnboldFontMask | HKUnitalicFontMask);
        if ((t & want) != want) continue;
        if ((traits & HKUnboldFontMask) && (t & HKBoldFontMask)) continue;
        if ((traits & HKUnitalicFontMask) && (t & HKItalicFontMask)) continue;
        [out addObject:name];
    }
    return out;
}
- (NSFont *)fontWithFamily:(NSString *)family traits:(NSUInteger)traits weight:(NSInteger)weight size:(CGFloat)size {
    if (!family) return nil;
    uint32_t sym = 0;
    if ((traits & HKBoldFontMask) || weight >= 9) sym |= kCTFontTraitBold;
    if (traits & HKItalicFontMask) sym |= kCTFontTraitItalic;
    if (traits & HKCondensedFontMask) sym |= kCTFontTraitCondensed;
    if (traits & HKExpandedFontMask) sym |= kCTFontTraitExpanded;
    NSDictionary *attrs = @{
        (id)kCTFontFamilyNameAttribute : family,
        (id)kCTFontTraitsAttribute : @{(id)kCTFontSymbolicTrait : @(sym), (id)kCTFontWeightTrait : @(ct_weight(weight))}
    };
    CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
    CTFontDescriptorRef m =
        CTFontDescriptorCreateMatchingFontDescriptor(d, (__bridge CFSetRef)[NSSet setWithObject:(id)kCTFontFamilyNameAttribute]);
    CFRelease(d);
    if (!m) return nil;
    CTFontRef f = CTFontCreateWithFontDescriptor(m, size, NULL);
    CFRelease(m);
    return own_font(f);
}
static NSFont *with_traits(NSFont *font, uint32_t set, uint32_t clear) {
    if (!is_ct_font(font)) return font;
    CTFontRef c = CTFontCreateCopyWithSymbolicTraits((__bridge CTFontRef)(id)font, 0, NULL, set, set | clear);
    return c ? own_font(c) : font;
}
- (NSFont *)convertFont:(NSFont *)font toHaveTrait:(NSUInteger)trait {
    uint32_t set = 0, clear = 0;
    if (trait & HKBoldFontMask) set |= kCTFontTraitBold;
    if (trait & HKUnboldFontMask) clear |= kCTFontTraitBold;
    if (trait & HKItalicFontMask) set |= kCTFontTraitItalic;
    if (trait & HKUnitalicFontMask) clear |= kCTFontTraitItalic;
    if (trait & HKCondensedFontMask) set |= kCTFontTraitCondensed;
    if (trait & HKExpandedFontMask) set |= kCTFontTraitExpanded;
    if (trait & HKFixedPitchFontMask) set |= kCTFontTraitMonoSpace;
    return with_traits(font, set, clear);
}
- (NSFont *)convertFont:(NSFont *)font toNotHaveTrait:(NSUInteger)trait {
    uint32_t clear = 0;
    if (trait & (HKBoldFontMask | HKUnboldFontMask)) clear |= kCTFontTraitBold;
    if (trait & (HKItalicFontMask | HKUnitalicFontMask)) clear |= kCTFontTraitItalic;
    if (trait & HKCondensedFontMask) clear |= kCTFontTraitCondensed;
    if (trait & HKExpandedFontMask) clear |= kCTFontTraitExpanded;
    if (trait & HKFixedPitchFontMask) clear |= kCTFontTraitMonoSpace;
    return with_traits(font, 0, clear);
}
- (NSFont *)convertFont:(NSFont *)font toSize:(CGFloat)size {
    return is_ct_font(font) ? own_font(CTFontCreateCopyWithAttributes((__bridge CTFontRef)(id)font, size, NULL, NULL)) : font;
}
- (NSFont *)convertFont:(NSFont *)font toFamily:(NSString *)family {
    if (!is_ct_font(font) || !family) return font;
    CTFontRef c = CTFontCreateCopyWithFamily((__bridge CTFontRef)(id)font, 0, NULL, (__bridge CFStringRef)family);
    return c ? own_font(c) : font;
}
- (NSFont *)convertFont:(NSFont *)font toFace:(NSString *)face {
    return [NSFont fontWithName:face size:is_ct_font(font) ? CTFontGetSize((__bridge CTFontRef)(id)font) : 0];
}
- (NSFont *)convertWeight:(BOOL)upFlag ofFont:(NSFont *)font {
    return upFlag ? [self convertFont:font toHaveTrait:HKBoldFontMask] : [self convertFont:font toHaveTrait:HKUnboldFontMask];
}
- (NSFont *)convertFont:(NSFont *)font { return font; }
- (NSUInteger)traitsOfFont:(NSFont *)font {
    return is_ct_font(font) ? traits_of(CTFontGetSymbolicTraits((__bridge CTFontRef)(id)font)) : 0;
}
- (NSInteger)weightOfFont:(NSFont *)font {
    if (!is_ct_font(font)) return 5;
    NSDictionary *traits = CFBridgingRelease(CTFontCopyTraits((__bridge CTFontRef)(id)font));
    return appkit_weight(weight_trait(traits));
}
- (BOOL)fontNamed:(NSString *)name hasTraits:(NSUInteger)traits {
    NSFont *f = [NSFont fontWithName:name size:12];
    return f && ([self traitsOfFont:f] & traits) == traits;
}
- (NSString *)localizedNameForFamily:(NSString *)family face:(NSString *)face {
    return face.length ? [NSString stringWithFormat:@"%@ %@", family, face] : family;
}
- (NSFont *)selectedFont { return _selected; }
- (void)setSelectedFont:(NSFont *)font isMultiple:(BOOL)multiple { _selected = font; }
- (BOOL)isMultiple { return NO; }
- (BOOL)isEnabled { return YES; }
- (void)setEnabled:(BOOL)flag {}
- (id)delegate { return _delegate; }
- (void)setDelegate:(id)delegate { _delegate = delegate; }
- (id)target { return _target; }
- (void)setTarget:(id)target { _target = target; }
- (SEL)action { return _action; }
- (void)setAction:(SEL)action { _action = action; }
- (id)fontPanel:(BOOL)create { return nil; }
- (NSMenu *)fontMenu:(BOOL)create { return nil; }
- (void)orderFrontFontPanel:(id)sender {}
- (void)addFontTrait:(id)sender {}
- (void)removeFontTrait:(id)sender {}
- (void)modifyFont:(id)sender {}
@end

/* ================================================================ graphics contexts */

@interface NSImageRep : HKShimObject <NSCopying>
@property NSSize size;
@property NSInteger pixelsWide, pixelsHigh;
- (BOOL)drawInRect:(NSRect)rect;
- (CGImageRef)CGImageForProposedRect:(NSRect *)rect context:(NSGraphicsContext *)context hints:(NSDictionary *)hints;
@end
@interface NSBitmapImageRep : NSImageRep
+ (instancetype)imageRepWithData:(NSData *)data;
+ (NSArray *)imageRepsWithData:(NSData *)data;
- (instancetype)initWithCGImage:(CGImageRef)image;
@property(readonly) CGImageRef CGImage;
@property(readonly) NSData *TIFFRepresentation;
- (CGContextRef)hk_newContext CF_RETURNS_RETAINED;
- (void)hk_contextAttached:(BOOL)attached;
@end

@implementation NSGraphicsContext {
    CGContextRef _cg;
    BOOL _flipped, _toScreen;
    NSBitmapImageRep *_rep;
    NSInteger _interpolation, _operation;
    BOOL _antialias;
    NSPoint _patternPhase;
    NSMutableArray *_states;
}

static NSString *const kCurrentKey = @"HKCurrentGraphicsContext";
static NSString *const kStackKey = @"HKGraphicsContextStack";

- (instancetype)initWithHKContext:(CGContextRef)cg flipped:(BOOL)flipped {
    if (!cg || !(self = [super init])) return nil;
    _cg = CGContextRetain(cg);
    _flipped = flipped;
    _toScreen = YES;
    _antialias = YES;
    _operation = NSCompositingOperationSourceOver;
    _states = [NSMutableArray array];
    return self;
}
- (void)dealloc {
    if (_rep) [_rep hk_contextAttached:NO];
    CGContextRelease(_cg);
}
+ (NSGraphicsContext *)graphicsContextWithCGContext:(CGContextRef)context flipped:(BOOL)flipped {
    return [[self alloc] initWithHKContext:context flipped:flipped];
}
+ (NSGraphicsContext *)graphicsContextWithGraphicsPort:(void *)port flipped:(BOOL)flipped {
    return [self graphicsContextWithCGContext:(CGContextRef)port flipped:flipped];
}
+ (NSGraphicsContext *)graphicsContextWithBitmapImageRep:(NSBitmapImageRep *)rep {
    CGContextRef cg = [rep hk_newContext];
    if (!cg) return nil;
    NSGraphicsContext *g = [[self alloc] initWithHKContext:cg flipped:NO];
    CGContextRelease(cg);
    g->_rep = rep;
    g->_toScreen = NO;
    [rep hk_contextAttached:YES];
    return g;
}
+ (NSGraphicsContext *)graphicsContextWithAttributes:(NSDictionary *)attributes { return nil; }
+ (NSGraphicsContext *)graphicsContextWithWindow:(NSWindow *)window { return nil; }

+ (NSGraphicsContext *)currentContext {
    id c = [NSThread currentThread].threadDictionary[kCurrentKey];
    return c == [NSNull null] ? nil : c;
}
+ (void)setCurrentContext:(NSGraphicsContext *)context {
    NSMutableDictionary *d = [NSThread currentThread].threadDictionary;
    if (context) d[kCurrentKey] = context;
    else [d removeObjectForKey:kCurrentKey];
}
+ (BOOL)currentContextDrawingToScreen {
    NSGraphicsContext *c = [self currentContext];
    return c ? c->_toScreen : YES;
}
+ (void)saveGraphicsState {
    NSMutableDictionary *d = [NSThread currentThread].threadDictionary;
    NSMutableArray *stack = d[kStackKey];
    if (!stack) d[kStackKey] = stack = [NSMutableArray array];
    NSGraphicsContext *c = [self currentContext];
    [stack addObject:c ?: (id)[NSNull null]];
    [c saveGraphicsState];
}
+ (void)restoreGraphicsState {
    NSMutableArray *stack = [NSThread currentThread].threadDictionary[kStackKey];
    if (!stack.count) return;
    id saved = stack.lastObject;
    [stack removeLastObject];
    NSGraphicsContext *c = saved == [NSNull null] ? nil : saved;
    [self setCurrentContext:c];
    [c restoreGraphicsState];
}

- (CGContextRef)CGContext { return _cg; }
- (void *)graphicsPort { return _cg; }
- (BOOL)isFlipped { return _flipped; }
- (BOOL)isDrawingToScreen { return _toScreen; }
- (NSDictionary *)attributes { return nil; }
- (void)saveGraphicsState {
    CGContextSaveGState(_cg);
    [_states addObject:@[ @(_interpolation), @(_operation), @(_antialias) ]];
}
- (void)restoreGraphicsState {
    if (!_states.count) return;
    NSArray *s = _states.lastObject;
    [_states removeLastObject];
    _interpolation = [s[0] integerValue];
    _operation = [s[1] integerValue];
    _antialias = [s[2] boolValue];
    CGContextRestoreGState(_cg);
}
- (void)flushGraphics { CGContextFlush(_cg); }
- (NSInteger)imageInterpolation { return _interpolation; }
- (void)setImageInterpolation:(NSInteger)interpolation {
    _interpolation = interpolation;
    /* NSImageInterpolation and CGInterpolationQuality share their values. */
    CGContextSetInterpolationQuality(_cg, (CGInterpolationQuality)interpolation);
}
- (BOOL)shouldAntialias { return _antialias; }
- (void)setShouldAntialias:(BOOL)flag {
    _antialias = flag;
    CGContextSetShouldAntialias(_cg, flag);
}
static CGBlendMode blend_mode(NSInteger op) {
    switch (op) {
    case NSCompositingOperationClear: return kCGBlendModeClear;
    case NSCompositingOperationCopy: return kCGBlendModeCopy;
    case NSCompositingOperationSourceIn: return kCGBlendModeSourceIn;
    case NSCompositingOperationSourceOut: return kCGBlendModeSourceOut;
    case NSCompositingOperationSourceAtop: return kCGBlendModeSourceAtop;
    case NSCompositingOperationDestinationOver: return kCGBlendModeDestinationOver;
    case NSCompositingOperationDestinationIn: return kCGBlendModeDestinationIn;
    case NSCompositingOperationDestinationOut: return kCGBlendModeDestinationOut;
    case NSCompositingOperationDestinationAtop: return kCGBlendModeDestinationAtop;
    case NSCompositingOperationXOR: return kCGBlendModeXOR;
    case NSCompositingOperationPlusDarker: return kCGBlendModePlusDarker;
    case NSCompositingOperationPlusLighter: return kCGBlendModePlusLighter;
    default:
        /* Multiply (14) through Luminosity (28) follow CGBlendMode's Multiply (1) through Luminosity (15). */
        if (op >= 14 && op <= 28) return (CGBlendMode)(op - 13);
        return kCGBlendModeNormal;
    }
}
- (NSCompositingOperation)compositingOperation { return (NSCompositingOperation)_operation; }
- (void)setCompositingOperation:(NSCompositingOperation)op {
    _operation = op;
    CGContextSetBlendMode(_cg, blend_mode(op));
}
- (NSPoint)patternPhase { return _patternPhase; }
- (void)setPatternPhase:(NSPoint)phase {
    _patternPhase = phase;
    CGContextSetPatternPhase(_cg, CGSizeMake(phase.x, phase.y));
}
- (NSInteger)colorRenderingIntent { return 0; }
- (void)setColorRenderingIntent:(NSInteger)intent {}
- (id)focusStack { return nil; }
- (void)setFocusStack:(id)stack {}
- (id)CIContext { return nil; }
@end

/* ================================================================ image representations */

@implementation NSImageRep {
  @protected
    NSSize _size;
    NSInteger _pixelsWide, _pixelsHigh, _bitsPerSample;
    BOOL _hasAlpha, _opaque;
    NSString *_colorSpaceName;
    NSInteger _layoutDirection;
}
+ (NSArray<Class> *)registeredImageRepClasses { return @[ [NSBitmapImageRep class] ]; }
+ (void)registerImageRepClass:(Class)cls {}
+ (void)unregisterImageRepClass:(Class)cls {}
+ (NSArray<NSString *> *)imageTypes { return CFBridgingRelease(CGImageSourceCopyTypeIdentifiers()); }
+ (NSArray<NSString *> *)imageUnfilteredTypes { return [self imageTypes]; }
+ (BOOL)canInitWithData:(NSData *)data {
    if (!data) return NO;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    BOOL ok = src && CGImageSourceGetCount(src) > 0;
    if (src) CFRelease(src);
    return ok;
}
+ (Class)imageRepClassForData:(NSData *)data { return [NSBitmapImageRep canInitWithData:data] ? [NSBitmapImageRep class] : nil; }
+ (Class)imageRepClassForType:(NSString *)type { return [NSBitmapImageRep class]; }
+ (Class)imageRepClassForFileType:(NSString *)type { return [NSBitmapImageRep class]; }
+ (instancetype)imageRepWithContentsOfFile:(NSString *)path {
    NSData *d = path ? [NSData dataWithContentsOfFile:path] : nil;
    return d ? [NSBitmapImageRep imageRepWithData:d] : nil;
}
+ (NSArray *)imageRepsWithContentsOfFile:(NSString *)path {
    NSData *d = path ? [NSData dataWithContentsOfFile:path] : nil;
    return d ? [NSBitmapImageRep imageRepsWithData:d] : nil;
}
+ (instancetype)imageRepWithContentsOfURL:(NSURL *)url { return [self imageRepWithContentsOfFile:url.path]; }
- (instancetype)init {
    if ((self = [super init])) {
        _colorSpaceName = @"NSCalibratedRGBColorSpace";
        _bitsPerSample = 8;
    }
    return self;
}
- (id)copyWithZone:(NSZone *)zone {
    NSImageRep *r = [[[self class] alloc] init];
    r->_size = _size;
    r->_pixelsWide = _pixelsWide;
    r->_pixelsHigh = _pixelsHigh;
    r->_bitsPerSample = _bitsPerSample;
    r->_hasAlpha = _hasAlpha;
    r->_opaque = _opaque;
    r->_colorSpaceName = _colorSpaceName;
    r->_layoutDirection = _layoutDirection;
    return r;
}
- (NSSize)size { return _size; }
- (void)setSize:(NSSize)size { _size = size; }
- (NSInteger)pixelsWide { return _pixelsWide; }
- (void)setPixelsWide:(NSInteger)n { _pixelsWide = n; }
- (NSInteger)pixelsHigh { return _pixelsHigh; }
- (void)setPixelsHigh:(NSInteger)n { _pixelsHigh = n; }
- (NSInteger)bitsPerSample { return _bitsPerSample; }
- (void)setBitsPerSample:(NSInteger)n { _bitsPerSample = n; }
- (BOOL)hasAlpha { return _hasAlpha; }
- (void)setAlpha:(BOOL)flag { _hasAlpha = flag; }
- (BOOL)isOpaque { return _opaque; }
- (void)setOpaque:(BOOL)flag { _opaque = flag; }
- (NSString *)colorSpaceName { return _colorSpaceName; }
- (void)setColorSpaceName:(NSString *)name { _colorSpaceName = [name copy]; }
- (NSInteger)layoutDirection { return _layoutDirection; }
- (void)setLayoutDirection:(NSInteger)d { _layoutDirection = d; }
- (CGImageRef)CGImageForProposedRect:(NSRect *)rect context:(NSGraphicsContext *)context hints:(NSDictionary *)hints {
    return NULL;
}
- (BOOL)draw { return [self drawInRect:NSMakeRect(0, 0, _size.width, _size.height)]; }
- (BOOL)drawAtPoint:(NSPoint)point { return [self drawInRect:NSMakeRect(point.x, point.y, _size.width, _size.height)]; }
- (BOOL)drawInRect:(NSRect)rect {
    return [self drawInRect:rect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:NO hints:nil];
}
- (BOOL)drawInRect:(NSRect)dst fromRect:(NSRect)src operation:(NSCompositingOperation)op fraction:(CGFloat)fraction
    respectFlipped:(BOOL)respect hints:(NSDictionary *)hints {
    NSGraphicsContext *g = [NSGraphicsContext currentContext];
    CGImageRef img = [self CGImageForProposedRect:&dst context:g hints:hints];
    if (!img || !g.CGContext) return NO;
    CGContextRef c = g.CGContext;
    CGImageRef part = NULL;
    if (!NSIsEmptyRect(src) && _size.width > 0 && _size.height > 0) {
        CGFloat sx = CGImageGetWidth(img) / _size.width, sy = CGImageGetHeight(img) / _size.height;
        CGRect px = CGRectMake(src.origin.x * sx, CGImageGetHeight(img) - (src.origin.y + src.size.height) * sy,
                               src.size.width * sx, src.size.height * sy);
        part = CGImageCreateWithImageInRect(img, CGRectIntegral(px));
        if (part) img = part;
    }
    CGContextSaveGState(c);
    CGContextSetAlpha(c, fraction);
    CGContextSetBlendMode(c, blend_mode(op));
    if (respect && g.isFlipped) {
        CGContextTranslateCTM(c, dst.origin.x, dst.origin.y + dst.size.height);
        CGContextScaleCTM(c, 1, -1);
        CGContextDrawImage(c, CGRectMake(0, 0, dst.size.width, dst.size.height), img);
    } else {
        CGContextDrawImage(c, dst, img);
    }
    CGContextRestoreGState(c);
    if (part) CGImageRelease(part);
    return YES;
}
@end

typedef NS_OPTIONS(NSUInteger, HKBitmapFormat) {
    HKBitmapFormatAlphaFirst = 1 << 0, HKBitmapFormatAlphaNonpremultiplied = 1 << 1, HKBitmapFormatFloatingPoint = 1 << 2,
    HKBitmapFormat16BitLittleEndian = 1 << 8, HKBitmapFormat32BitLittleEndian = 1 << 9,
    HKBitmapFormat16BitBigEndian = 1 << 10, HKBitmapFormat32BitBigEndian = 1 << 11,
};

/* A bitmap: its own pixel buffer (made, given, or copied out of a CGImage on first access to the pixels), or a
   CGImage while nobody has asked for the pixels. */
@implementation NSBitmapImageRep {
    CGImageRef _image;    /* the source, or the last snapshot of the buffer */
    unsigned char *_data; /* the buffer, when there is one */
    BOOL _ownsData, _dirty, _planar;
    NSInteger _bytesPerRow, _bitsPerPixel, _samplesPerPixel, _contexts;
    NSUInteger _format;
    NSColorSpace *_colorSpace;
    NSMutableDictionary *_properties;
}

static CGBitmapInfo bitmap_info(NSUInteger format, BOOL alpha, NSInteger bps, NSInteger spp, NSInteger bpp) {
    CGImageAlphaInfo a;
    BOOL first = format & HKBitmapFormatAlphaFirst, straight = format & HKBitmapFormatAlphaNonpremultiplied;
    if (alpha)
        a = first ? (straight ? kCGImageAlphaFirst : kCGImageAlphaPremultipliedFirst)
                  : (straight ? kCGImageAlphaLast : kCGImageAlphaPremultipliedLast);
    else if (bpp > bps * spp)
        a = first ? kCGImageAlphaNoneSkipFirst : kCGImageAlphaNoneSkipLast;
    else
        a = kCGImageAlphaNone;
    CGBitmapInfo info = (CGBitmapInfo)a;
    if (format & HKBitmapFormat32BitLittleEndian) info |= kCGBitmapByteOrder32Little;
    else if (format & HKBitmapFormat16BitLittleEndian) info |= kCGBitmapByteOrder16Little;
    else if (format & HKBitmapFormat32BitBigEndian) info |= kCGBitmapByteOrder32Big;
    else if (format & HKBitmapFormat16BitBigEndian) info |= kCGBitmapByteOrder16Big;
    if (format & HKBitmapFormatFloatingPoint) info |= kCGBitmapFloatComponents;
    return info;
}

static NSUInteger format_of(CGBitmapInfo info) {
    CGImageAlphaInfo a = (CGImageAlphaInfo)(info & kCGBitmapAlphaInfoMask);
    NSUInteger f = 0;
    if (a == kCGImageAlphaFirst || a == kCGImageAlphaPremultipliedFirst || a == kCGImageAlphaNoneSkipFirst) f |= HKBitmapFormatAlphaFirst;
    if (a == kCGImageAlphaFirst || a == kCGImageAlphaLast) f |= HKBitmapFormatAlphaNonpremultiplied;
    if (info & kCGBitmapFloatComponents) f |= HKBitmapFormatFloatingPoint;
    switch (info & kCGBitmapByteOrderMask) {
    case kCGBitmapByteOrder32Little: f |= HKBitmapFormat32BitLittleEndian; break;
    case kCGBitmapByteOrder16Little: f |= HKBitmapFormat16BitLittleEndian; break;
    case kCGBitmapByteOrder32Big: f |= HKBitmapFormat32BitBigEndian; break;
    case kCGBitmapByteOrder16Big: f |= HKBitmapFormat16BitBigEndian; break;
    default: break;
    }
    return f;
}

static BOOL alpha_info_has_alpha(CGImageAlphaInfo a) {
    return a == kCGImageAlphaFirst || a == kCGImageAlphaLast || a == kCGImageAlphaPremultipliedFirst ||
           a == kCGImageAlphaPremultipliedLast || a == kCGImageAlphaOnly;
}

- (NSColorSpace *)hk_space {
    if (_colorSpace) return _colorSpace;
    NSString *n = _colorSpaceName;
    if ([n isEqualToString:@"NSDeviceRGBColorSpace"]) return [NSColorSpace deviceRGBColorSpace];
    if ([n isEqualToString:@"NSCalibratedWhiteColorSpace"]) return [NSColorSpace genericGrayColorSpace];
    if ([n isEqualToString:@"NSDeviceWhiteColorSpace"]) return [NSColorSpace deviceGrayColorSpace];
    if ([n isEqualToString:@"NSDeviceCMYKColorSpace"]) return [NSColorSpace deviceCMYKColorSpace];
    return [NSColorSpace sRGBColorSpace];
}

- (instancetype)initWithBitmapDataPlanes:(unsigned char **)planes pixelsWide:(NSInteger)width pixelsHigh:(NSInteger)height
                           bitsPerSample:(NSInteger)bps samplesPerPixel:(NSInteger)spp hasAlpha:(BOOL)alpha
                                isPlanar:(BOOL)planar colorSpaceName:(NSString *)space bitmapFormat:(NSUInteger)format
                             bytesPerRow:(NSInteger)rowBytes bitsPerPixel:(NSInteger)pixelBits {
    if (width <= 0 || height <= 0 || bps <= 0 || spp <= 0 || !(self = [super init])) return nil;
    _pixelsWide = width;
    _pixelsHigh = height;
    _size = NSMakeSize(width, height);
    _bitsPerSample = bps;
    _samplesPerPixel = spp;
    _hasAlpha = alpha;
    _opaque = !alpha;
    _planar = planar && spp > 1;
    _colorSpaceName = space ? [space copy] : @"NSCalibratedRGBColorSpace";
    _format = format;
    _bitsPerPixel = pixelBits ?: (_planar ? bps : bps * spp);
    _bytesPerRow = rowBytes ?: (width * _bitsPerPixel + 7) / 8;
    if (planes && planes[0] && !_planar) {
        _data = planes[0];
    } else {
        size_t bytes = (size_t)_bytesPerRow * (size_t)height * (size_t)(_planar ? spp : 1);
        _data = calloc(1, bytes ? bytes : 1);
        _ownsData = YES;
        if (planes && _planar)
            for (NSInteger i = 0; i < spp; i++)
                if (planes[i]) memcpy(_data + i * _bytesPerRow * height, planes[i], (size_t)(_bytesPerRow * height));
    }
    _dirty = YES;
    return self;
}
- (instancetype)initWithBitmapDataPlanes:(unsigned char **)planes pixelsWide:(NSInteger)width pixelsHigh:(NSInteger)height
                           bitsPerSample:(NSInteger)bps samplesPerPixel:(NSInteger)spp hasAlpha:(BOOL)alpha
                                isPlanar:(BOOL)planar colorSpaceName:(NSString *)space bytesPerRow:(NSInteger)rowBytes
                            bitsPerPixel:(NSInteger)pixelBits {
    return [self initWithBitmapDataPlanes:planes pixelsWide:width pixelsHigh:height bitsPerSample:bps samplesPerPixel:spp
                                 hasAlpha:alpha isPlanar:planar colorSpaceName:space bitmapFormat:0 bytesPerRow:rowBytes
                             bitsPerPixel:pixelBits];
}
- (instancetype)initWithCGImage:(CGImageRef)image {
    if (!image || !(self = [super init])) return nil;
    _image = CGImageRetain(image);
    _pixelsWide = (NSInteger)CGImageGetWidth(image);
    _pixelsHigh = (NSInteger)CGImageGetHeight(image);
    _size = NSMakeSize(_pixelsWide, _pixelsHigh);
    _bitsPerSample = (NSInteger)CGImageGetBitsPerComponent(image);
    _bitsPerPixel = (NSInteger)CGImageGetBitsPerPixel(image);
    _bytesPerRow = (NSInteger)CGImageGetBytesPerRow(image);
    CGImageAlphaInfo a = CGImageGetAlphaInfo(image);
    _hasAlpha = alpha_info_has_alpha(a);
    _opaque = !_hasAlpha;
    CGColorSpaceRef cs = CGImageGetColorSpace(image);
    size_t comps = cs ? CGColorSpaceGetNumberOfComponents(cs) : 1;
    _samplesPerPixel = (NSInteger)comps + (_hasAlpha ? 1 : 0);
    _format = format_of(CGImageGetBitmapInfo(image));
    if (cs) _colorSpace = [[NSColorSpace alloc] initWithCGColorSpace:cs];
    _colorSpaceName = comps == 1 ? @"NSCalibratedWhiteColorSpace" : comps == 4 ? @"NSDeviceCMYKColorSpace" : @"NSCalibratedRGBColorSpace";
    return self;
}
- (instancetype)initWithCIImage:(id)image { return nil; }
- (instancetype)initWithFocusedViewRect:(NSRect)rect { return nil; }

/* One frame of image data, sized in points by its resolution (72 dpi is one point a pixel). */
static NSBitmapImageRep *rep_from_source(CGImageSourceRef src, size_t index) {
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, index, NULL);
    if (!img) return nil;
    NSBitmapImageRep *r = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    NSDictionary *props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, index, NULL));
    double dx = [props[(id)kCGImagePropertyDPIWidth] doubleValue], dy = [props[(id)kCGImagePropertyDPIHeight] doubleValue];
    if (dx > 0 && dy > 0) r.size = NSMakeSize(r->_pixelsWide * 72.0 / dx, r->_pixelsHigh * 72.0 / dy);
    return r;
}
- (instancetype)initWithData:(NSData *)data {
    CGImageSourceRef src = data ? CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL) : NULL;
    NSBitmapImageRep *r = src ? rep_from_source(src, 0) : nil;
    if (src) CFRelease(src);
    return r;
}
+ (instancetype)imageRepWithData:(NSData *)data { return [[self alloc] initWithData:data]; }
+ (NSArray *)imageRepsWithData:(NSData *)data {
    CGImageSourceRef src = data ? CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL) : NULL;
    if (!src) return @[];
    NSMutableArray *out = [NSMutableArray array];
    for (size_t i = 0, n = CGImageSourceGetCount(src); i < n; i++) {
        NSBitmapImageRep *r = rep_from_source(src, i);
        if (r) [out addObject:r];
    }
    CFRelease(src);
    return out;
}
+ (BOOL)defaultRespectOrientation { return YES; }
+ (void)setDefaultRespectOrientation:(BOOL)flag {}
- (void)dealloc {
    if (_ownsData) free(_data);
    CGImageRelease(_image);
}

/* Makes the pixels the rep's own buffer (copied out of the source image) and marks any snapshot stale. */
- (void)hk_materialize {
    if (!_data && _image) {
        CFDataRef bytes = CGDataProviderCopyData(CGImageGetDataProvider(_image));
        if (bytes) {
            size_t n = (size_t)CFDataGetLength(bytes);
            _data = malloc(n ? n : 1);
            memcpy(_data, CFDataGetBytePtr(bytes), n);
            _ownsData = YES;
            CFRelease(bytes);
        }
    }
    _dirty = YES;
}
- (unsigned char *)bitmapData {
    [self hk_materialize];
    return _data;
}
- (void)getBitmapDataPlanes:(unsigned char **)planes {
    [self hk_materialize];
    if (!planes) return;
    if (!_planar) {
        planes[0] = _data;
        for (int i = 1; i < 5; i++) planes[i] = NULL;
    } else {
        for (NSInteger i = 0; i < _samplesPerPixel && i < 5; i++) planes[i] = _data + i * _bytesPerRow * _pixelsHigh;
    }
}
- (CGImageRef)CGImage {
    if (_data && _planar) return NULL;
    if (_data && (_dirty || _contexts > 0 || !_image)) {
        CFDataRef bytes = CFDataCreate(NULL, _data, _bytesPerRow * _pixelsHigh);
        CGDataProviderRef p = CGDataProviderCreateWithCFData(bytes);
        CGImageRef img = CGImageCreate((size_t)_pixelsWide, (size_t)_pixelsHigh, (size_t)_bitsPerSample, (size_t)_bitsPerPixel,
                                       (size_t)_bytesPerRow, [self hk_space].CGColorSpace,
                                       bitmap_info(_format, _hasAlpha, _bitsPerSample, _samplesPerPixel, _bitsPerPixel), p,
                                       NULL, true, kCGRenderingIntentDefault);
        CGDataProviderRelease(p);
        CFRelease(bytes);
        /* A snapshot handed out earlier stays valid until the caller's autorelease pool drains. */
        if (_image) CFAutorelease(_image);
        _image = img;
        _dirty = NO;
    }
    return _image;
}
- (CGImageRef)CGImageForProposedRect:(NSRect *)rect context:(NSGraphicsContext *)context hints:(NSDictionary *)hints {
    return self.CGImage;
}
- (CGContextRef)hk_newContext {
    if (_planar || (_bitsPerSample != 8 && _bitsPerSample != 16 && _bitsPerSample != 32)) return NULL;
    [self hk_materialize];
    if (!_data) return NULL;
    return CGBitmapContextCreate(_data, (size_t)_pixelsWide, (size_t)_pixelsHigh, (size_t)_bitsPerSample, (size_t)_bytesPerRow,
                                 [self hk_space].CGColorSpace,
                                 bitmap_info(_format, _hasAlpha, _bitsPerSample, _samplesPerPixel, _bitsPerPixel));
}
- (void)hk_contextAttached:(BOOL)attached {
    _contexts += attached ? 1 : -1;
    _dirty = YES;
}
- (id)copyWithZone:(NSZone *)zone {
    NSBitmapImageRep *r;
    if (_data) {
        r = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:_pixelsWide pixelsHigh:_pixelsHigh
                                                 bitsPerSample:_bitsPerSample samplesPerPixel:_samplesPerPixel hasAlpha:_hasAlpha
                                                      isPlanar:_planar colorSpaceName:_colorSpaceName bitmapFormat:_format
                                                   bytesPerRow:_bytesPerRow bitsPerPixel:_bitsPerPixel];
        memcpy(r->_data, _data, (size_t)(_bytesPerRow * _pixelsHigh * (_planar ? _samplesPerPixel : 1)));
        r->_colorSpace = _colorSpace;
    } else {
        r = [[NSBitmapImageRep alloc] initWithCGImage:_image];
    }
    r->_size = _size;
    r->_properties = [_properties mutableCopy];
    return r;
}
- (NSInteger)bitsPerPixel { return _bitsPerPixel; }
- (NSInteger)samplesPerPixel { return _samplesPerPixel; }
- (NSInteger)bytesPerRow { return _bytesPerRow; }
- (NSInteger)bytesPerPlane { return _bytesPerRow * _pixelsHigh; }
- (BOOL)isPlanar { return _planar; }
- (NSInteger)numberOfPlanes { return _planar ? _samplesPerPixel : 1; }
- (NSUInteger)bitmapFormat { return _format; }
- (NSColorSpace *)colorSpace { return [self hk_space]; }
- (BOOL)respectOrientation { return YES; }
- (id)valueForProperty:(NSString *)property { return _properties[property]; }
- (void)setProperty:(NSString *)property withValue:(id)value {
    if (!property) return;
    if (!_properties) _properties = [NSMutableDictionary dictionary];
    if (value) _properties[property] = value;
    else [_properties removeObjectForKey:property];
}
- (NSBitmapImageRep *)bitmapImageRepByRetaggingWithColorSpace:(NSColorSpace *)space {
    if (!space || space.colorSpaceModel != [self hk_space].colorSpaceModel) return nil;
    NSBitmapImageRep *r = [self copy];
    r->_colorSpace = space;
    r->_dirty = YES;
    return r;
}
- (NSBitmapImageRep *)bitmapImageRepByConvertingToColorSpace:(NSColorSpace *)space renderingIntent:(NSInteger)intent {
    CGImageRef img = self.CGImage, conv = img && space ? CGImageCreateCopyWithColorSpace(img, space.CGColorSpace) : NULL;
    if (!conv) return [self bitmapImageRepByRetaggingWithColorSpace:space];
    NSBitmapImageRep *r = [[NSBitmapImageRep alloc] initWithCGImage:conv];
    CGImageRelease(conv);
    r.size = _size;
    return r;
}

static NSString *uti_for_file_type(NSUInteger type) {
    switch (type) {
    case 1: return @"com.microsoft.bmp";
    case 2: return @"com.compuserve.gif";
    case 3: return @"public.jpeg";
    case 4: return @"public.png";
    case 5: return @"public.jpeg-2000";
    default: return @"public.tiff";
    }
}
static NSData *encode_images(NSArray *reps, NSUInteger type, NSDictionary *properties) {
    NSMutableData *out = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out,
                                                                 (__bridge CFStringRef)uti_for_file_type(type), reps.count, NULL);
    if (!dst) return nil;
    NSMutableDictionary *opts = [NSMutableDictionary dictionary];
    id q = properties[@"NSImageCompressionFactor"];
    if (q) opts[(id)kCGImageDestinationLossyCompressionQuality] = q;
    for (NSBitmapImageRep *r in reps) {
        CGImageRef img = r.CGImage;
        if (!img) continue;
        NSMutableDictionary *frame = [opts mutableCopy];
        if (r.pixelsWide && r.size.width > 0) {
            frame[(id)kCGImagePropertyDPIWidth] = @(72.0 * r.pixelsWide / r.size.width);
            frame[(id)kCGImagePropertyDPIHeight] = @(72.0 * r.pixelsHigh / r.size.height);
        }
        CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)frame);
    }
    BOOL ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    return ok ? out : nil;
}
- (NSData *)representationUsingType:(NSUInteger)type properties:(NSDictionary *)properties {
    return encode_images(@[ self ], type, properties);
}
+ (NSData *)representationOfImageRepsInArray:(NSArray *)reps usingType:(NSUInteger)type properties:(NSDictionary *)properties {
    return encode_images(reps, type, properties);
}
- (NSData *)TIFFRepresentation { return encode_images(@[ self ], 0, nil); }
- (NSData *)TIFFRepresentationUsingCompression:(NSUInteger)comp factor:(float)factor { return self.TIFFRepresentation; }
+ (NSData *)TIFFRepresentationOfImageRepsInArray:(NSArray *)reps { return encode_images(reps, 0, nil); }
@end

/* ================================================================ images */

typedef BOOL (^HKDrawingHandler)(NSRect);

@implementation NSImage {
    NSSize _size;
    NSMutableArray<NSImageRep *> *_reps;
    HKDrawingHandler _handler;
    BOOL _handlerFlipped, _template, _flipped, _matchesOnlyOnBestFittingAxis, _prefersColorMatch, _usesEPS;
    NSString *_name, *_accessibilityDescription;
    CGImageRef _rendered; /* the drawing handler's output, at _renderedSize pixels */
    CGSize _renderedSize;
    NSRect _alignmentRect;
    NSEdgeInsets _capInsets;
    NSInteger _resizingMode, _cacheMode;
    NSColor *_backgroundColor;
    __weak id _delegate;
    CGContextRef _focus;
    CGFloat _focusScale;
}

static NSMapTable<NSString *, NSImage *> *g_named;
static os_unfair_lock g_named_lock = OS_UNFAIR_LOCK_INIT;

static CGFloat screen_scale(void) {
    const HKPresenter *p = hk_presenter();
    return p && p->scale > 0 ? p->scale : 2;
}

- (instancetype)init {
    if ((self = [super init])) {
        _reps = [NSMutableArray array];
        _prefersColorMatch = YES;
    }
    return self;
}
- (void)dealloc {
    CGImageRelease(_rendered);
    CGContextRelease(_focus);
}
- (instancetype)initWithSize:(NSSize)size {
    if ((self = [self init])) _size = size;
    return self;
}
- (instancetype)initWithCGImage:(CGImageRef)image size:(NSSize)size {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    if (!rep || !(self = [self init])) return nil;
    if (size.width <= 0 || size.height <= 0) size = rep.size;
    rep.size = size;
    _size = size;
    [_reps addObject:rep];
    return self;
}
- (instancetype)initWithData:(NSData *)data {
    NSArray *reps = [NSBitmapImageRep imageRepsWithData:data];
    if (!reps.count || !(self = [self init])) return nil;
    [_reps addObjectsFromArray:reps];
    _size = [reps[0] size];
    return self;
}
- (instancetype)initWithDataIgnoringOrientation:(NSData *)data { return [self initWithData:data]; }
- (instancetype)initWithContentsOfFile:(NSString *)path {
    NSData *d = path ? [NSData dataWithContentsOfFile:path] : nil;
    return d ? [self initWithData:d] : nil;
}
- (instancetype)initWithContentsOfURL:(NSURL *)url { return [self initWithContentsOfFile:url.path]; }
- (instancetype)initByReferencingFile:(NSString *)path { return [self initWithContentsOfFile:path]; }
- (instancetype)initByReferencingURL:(NSURL *)url { return [self initWithContentsOfFile:url.path]; }
- (instancetype)initWithImageRep:(NSImageRep *)rep {
    if (!(self = [self init])) return nil;
    if (rep) {
        [_reps addObject:rep];
        _size = rep.size;
    }
    return self;
}
- (instancetype)initWithPasteboard:(id)pasteboard { return nil; }
+ (NSImage *)imageWithImageRep:(NSImageRep *)rep { return [[self alloc] initWithImageRep:rep]; }
+ (NSImage *)imageWithSize:(NSSize)size flipped:(BOOL)flipped drawingHandler:(BOOL (^)(NSRect))handler {
    NSImage *img = [[self alloc] initWithSize:size];
    img->_handler = [handler copy];
    img->_handlerFlipped = flipped;
    return img;
}
+ (NSImage *)imageWithSize:(NSSize)size drawHandler:(BOOL (^)(NSRect))handler {
    return [self imageWithSize:size flipped:NO drawingHandler:handler];
}
/* A transparent stand-in (system symbols and named system images are not available here). */
static NSImage *blank_image(NSString *description) {
    NSImage *img = [NSImage imageWithSize:NSMakeSize(16, 16) flipped:NO drawingHandler:^BOOL(NSRect r) { return YES; }];
    img->_template = YES;
    img->_accessibilityDescription = [description copy];
    return img;
}
+ (NSImage *)imageNamed:(NSImageName)name {
    if (!name) return nil;
    os_unfair_lock_lock(&g_named_lock);
    NSImage *img = [g_named objectForKey:name];
    os_unfair_lock_unlock(&g_named_lock);
    if (img) return img;
    if ([name isEqualToString:@"NSApplicationIcon"] && NSApp.applicationIconImage) return NSApp.applicationIconImage;
    NSString *path = name.pathExtension.length ? [hk_app_bundle() pathForResource:name ofType:nil] : nil;
    for (NSString *ext in @[ @"png", @"tiff", @"icns", @"jpg", @"gif" ])
        if (!path) path = [hk_app_bundle() pathForResource:name ofType:ext];
    if (path && (img = [[self alloc] initWithContentsOfFile:path])) {
        img.name = name;
        return img;
    }
    if ([name hasPrefix:@"NS"]) {
        img = blank_image(nil);
        img.name = name;
        return img;
    }
    return nil;
}
+ (NSImage *)imageWithSystemSymbolName:(NSString *)name accessibilityDescription:(NSString *)description {
    return blank_image(description);
}
+ (NSImage *)imageWithSystemSymbolName:(NSString *)name variableValue:(double)value accessibilityDescription:(NSString *)description {
    return blank_image(description);
}
+ (NSImage *)imageWithSymbolName:(NSString *)name variableValue:(double)value { return blank_image(nil); }
+ (NSImage *)imageWithSymbolName:(NSString *)name bundle:(NSBundle *)bundle variableValue:(double)value { return blank_image(nil); }
+ (NSImage *)imageWithPrivateSystemSymbolName:(NSString *)name accessibilityDescription:(NSString *)description {
    return blank_image(description);
}
+ (NSArray<NSString *> *)imageTypes { return [NSImageRep imageTypes]; }
+ (NSArray<NSString *> *)imageUnfilteredTypes { return [NSImageRep imageTypes]; }
+ (BOOL)canInitWithPasteboard:(id)pasteboard { return NO; }

- (id)copyWithZone:(NSZone *)zone {
    NSImage *c = [[NSImage alloc] initWithSize:_size];
    [c->_reps addObjectsFromArray:_reps];
    c->_handler = _handler;
    c->_handlerFlipped = _handlerFlipped;
    c->_template = _template;
    c->_flipped = _flipped;
    c->_accessibilityDescription = _accessibilityDescription;
    c->_alignmentRect = _alignmentRect;
    c->_capInsets = _capInsets;
    c->_resizingMode = _resizingMode;
    c->_backgroundColor = _backgroundColor;
    return c;
}

- (NSSize)size {
    if ((_size.width <= 0 || _size.height <= 0) && _reps.count) return _reps[0].size;
    return _size;
}
- (void)setSize:(NSSize)size {
    _size = size;
    [self recache];
}
- (BOOL)isValid { return (_handler || _reps.count) && self.size.width > 0 && self.size.height > 0; }
- (NSString *)name { return _name; }
- (BOOL)setName:(NSString *)name {
    os_unfair_lock_lock(&g_named_lock);
    if (!g_named) g_named = [NSMapTable strongToWeakObjectsMapTable];
    if (_name && [g_named objectForKey:_name] == self) [g_named removeObjectForKey:_name];
    _name = [name copy];
    if (name) [g_named setObject:self forKey:name];
    os_unfair_lock_unlock(&g_named_lock);
    return YES;
}
- (BOOL)isTemplate { return _template; }
- (void)setTemplate:(BOOL)flag { _template = flag; }
- (NSString *)accessibilityDescription { return _accessibilityDescription; }
- (void)setAccessibilityDescription:(NSString *)d { _accessibilityDescription = [d copy]; }
- (BOOL)isFlipped { return _flipped; }
- (void)setFlipped:(BOOL)flag { _flipped = flag; }
- (NSRect)alignmentRect { return NSIsEmptyRect(_alignmentRect) ? NSMakeRect(0, 0, self.size.width, self.size.height) : _alignmentRect; }
- (void)setAlignmentRect:(NSRect)r { _alignmentRect = r; }
- (NSEdgeInsets)capInsets { return _capInsets; }
- (void)setCapInsets:(NSEdgeInsets)insets { _capInsets = insets; }
- (NSInteger)resizingMode { return _resizingMode; }
- (void)setResizingMode:(NSInteger)mode { _resizingMode = mode; }
- (NSInteger)cacheMode { return _cacheMode; }
- (void)setCacheMode:(NSInteger)mode { _cacheMode = mode; }
- (NSColor *)backgroundColor { return _backgroundColor ?: [NSColor clearColor]; }
- (void)setBackgroundColor:(NSColor *)color { _backgroundColor = color; }
- (id)delegate { return _delegate; }
- (void)setDelegate:(id)delegate { _delegate = delegate; }
- (BOOL)matchesOnlyOnBestFittingAxis { return _matchesOnlyOnBestFittingAxis; }
- (void)setMatchesOnlyOnBestFittingAxis:(BOOL)f { _matchesOnlyOnBestFittingAxis = f; }
- (BOOL)prefersColorMatch { return _prefersColorMatch; }
- (void)setPrefersColorMatch:(BOOL)f { _prefersColorMatch = f; }
- (BOOL)usesEPSOnResolutionMismatch { return _usesEPS; }
- (void)setUsesEPSOnResolutionMismatch:(BOOL)f { _usesEPS = f; }
- (id)symbolConfiguration { return nil; }
- (NSImage *)imageWithSymbolConfiguration:(id)configuration { return self; }
- (id)locale { return nil; }
- (NSImage *)imageWithLocale:(id)locale { return self; }
- (void)recache {
    CGImageRelease(_rendered);
    _rendered = NULL;
}

- (NSArray<NSImageRep *> *)representations { return [_reps copy]; }
- (void)addRepresentation:(NSImageRep *)rep {
    if (!rep) return;
    [_reps addObject:rep];
    [self recache];
}
- (void)addRepresentations:(NSArray<NSImageRep *> *)reps {
    for (NSImageRep *r in reps) [self addRepresentation:r];
}
- (void)removeRepresentation:(NSImageRep *)rep {
    [_reps removeObjectIdenticalTo:rep];
    [self recache];
}

/* The bitmap with the most pixels at least as many as wanted, else the biggest. */
- (NSImageRep *)hk_bestBitmapForPixels:(CGSize)want {
    NSImageRep *best = nil, *biggest = nil;
    for (NSImageRep *r in _reps) {
        if (![r isKindOfClass:[NSBitmapImageRep class]]) continue;
        if (!biggest || r.pixelsWide * r.pixelsHigh > biggest.pixelsWide * biggest.pixelsHigh) biggest = r;
        if (r.pixelsWide >= want.width && r.pixelsHigh >= want.height &&
            (!best || r.pixelsWide * r.pixelsHigh < best.pixelsWide * best.pixelsHigh))
            best = r;
    }
    return best ?: biggest;
}
- (NSImageRep *)bestRepresentationForRect:(NSRect)rect context:(NSGraphicsContext *)context hints:(NSDictionary *)hints {
    CGFloat scale = screen_scale();
    if (context.CGContext) scale = fabs(CGContextGetUserSpaceToDeviceSpaceTransform(context.CGContext).a) ?: scale;
    NSSize sz = NSIsEmptyRect(rect) ? self.size : rect.size;
    return [self hk_bestBitmapForPixels:CGSizeMake(sz.width * scale, sz.height * scale)] ?: _reps.firstObject;
}

/* Draws the image's handler (or its representations) into a new bitmap of the given pixel size. */
- (CGImageRef)hk_renderPixels:(CGSize)px CF_RETURNS_RETAINED {
    NSSize size = self.size;
    if (px.width < 1 || px.height < 1 || size.width <= 0 || size.height <= 0) return NULL;
    CGColorSpaceRef cs = [NSColorSpace sRGBColorSpace].CGColorSpace;
    CGContextRef c = CGBitmapContextCreate(NULL, (size_t)px.width, (size_t)px.height, 8, 0, cs,
                                           kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (!c) return NULL;
    CGContextScaleCTM(c, px.width / size.width, px.height / size.height);
    BOOL flipped = _handler ? _handlerFlipped : NO;
    if (flipped) {
        CGContextTranslateCTM(c, 0, size.height);
        CGContextScaleCTM(c, 1, -1);
    }
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:c flipped:flipped]];
    NSRect all = NSMakeRect(0, 0, size.width, size.height);
    if (_handler) _handler(all);
    else
        for (NSImageRep *r in _reps) [r drawInRect:all];
    [NSGraphicsContext restoreGraphicsState];
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    return img;
}

/* The image as a CGImage of about want points at scale: a bitmap representation, else drawn (and kept). */
- (CGImageRef)hk_imageForSize:(NSSize)want scale:(CGFloat)scale {
    CGSize px = CGSizeMake(ceil(want.width * scale), ceil(want.height * scale));
    if (!_handler) {
        NSImageRep *r = [self hk_bestBitmapForPixels:px];
        CGImageRef img = [r CGImageForProposedRect:NULL context:nil hints:nil];
        if (img || !_reps.count) return img;
    }
    if (!_rendered || !CGSizeEqualToSize(_renderedSize, px)) {
        CGImageRelease(_rendered);
        _rendered = [self hk_renderPixels:px];
        _renderedSize = px;
    }
    return _rendered;
}
- (CGImageRef)CGImageForProposedRect:(NSRect *)rect context:(NSGraphicsContext *)context hints:(NSDictionary *)hints {
    CGFloat scale = screen_scale();
    if (context.CGContext) scale = fabs(CGContextGetUserSpaceToDeviceSpaceTransform(context.CGContext).a) ?: scale;
    return [self hk_imageForSize:rect && !NSIsEmptyRect(*rect) ? rect->size : self.size scale:scale];
}
- (id)layerContentsForContentsScale:(CGFloat)scale {
    return (__bridge id)[self hk_imageForSize:self.size scale:scale > 0 ? scale : screen_scale()];
}
- (CGFloat)recommendedLayerContentsScale:(CGFloat)preferred {
    if (preferred > 0) return preferred;
    NSImageRep *r = [self hk_bestBitmapForPixels:CGSizeZero];
    return r && r.size.width > 0 ? r.pixelsWide / r.size.width : 1;
}

- (void)drawInRect:(NSRect)dst fromRect:(NSRect)src operation:(NSCompositingOperation)op fraction:(CGFloat)fraction
    respectFlipped:(BOOL)respect hints:(NSDictionary *)hints {
    NSGraphicsContext *g = [NSGraphicsContext currentContext];
    CGContextRef c = g.CGContext;
    NSSize size = self.size;
    if (!c || size.width <= 0 || size.height <= 0) return;
    if (NSIsEmptyRect(dst)) return;
    CGImageRef img = [self CGImageForProposedRect:&dst context:g hints:hints];
    if (!img) return;
    CGImageRef part = NULL;
    if (!NSIsEmptyRect(src) && !NSEqualRects(src, NSMakeRect(0, 0, size.width, size.height))) {
        CGFloat sx = CGImageGetWidth(img) / size.width, sy = CGImageGetHeight(img) / size.height;
        CGRect px = CGRectMake(src.origin.x * sx, CGImageGetHeight(img) - (src.origin.y + src.size.height) * sy,
                               src.size.width * sx, src.size.height * sy);
        part = CGImageCreateWithImageInRect(img, CGRectIntegral(px));
        if (part) img = part;
    }
    CGContextSaveGState(c);
    CGContextSetAlpha(c, fraction);
    CGContextSetBlendMode(c, blend_mode(op));
    if (respect && g.isFlipped) {
        CGContextTranslateCTM(c, dst.origin.x, dst.origin.y + dst.size.height);
        CGContextScaleCTM(c, 1, -1);
        CGContextDrawImage(c, CGRectMake(0, 0, dst.size.width, dst.size.height), img);
    } else {
        CGContextDrawImage(c, dst, img);
    }
    CGContextRestoreGState(c);
    if (part) CGImageRelease(part);
}
- (void)drawInRect:(NSRect)rect {
    [self drawInRect:rect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
}
- (void)drawInRect:(NSRect)dst fromRect:(NSRect)src operation:(NSCompositingOperation)op fraction:(CGFloat)fraction {
    [self drawInRect:dst fromRect:src operation:op fraction:fraction respectFlipped:NO hints:nil];
}
- (void)drawAtPoint:(NSPoint)point fromRect:(NSRect)src operation:(NSCompositingOperation)op fraction:(CGFloat)fraction {
    NSSize s = NSIsEmptyRect(src) ? self.size : src.size;
    [self drawInRect:NSMakeRect(point.x, point.y, s.width, s.height) fromRect:src operation:op fraction:fraction
        respectFlipped:NO hints:nil];
}
- (void)drawRepresentation:(NSImageRep *)rep inRect:(NSRect)rect { [rep drawInRect:rect]; }

/* lockFocus: drawing goes into a bitmap at the screen's scale, which becomes the image's representation. */
- (void)lockFocusFlipped:(BOOL)flipped {
    NSSize size = self.size;
    if (_focus || size.width <= 0 || size.height <= 0) return;
    _focusScale = screen_scale();
    size_t w = (size_t)ceil(size.width * _focusScale), h = (size_t)ceil(size.height * _focusScale);
    _focus = CGBitmapContextCreate(NULL, w, h, 8, 0, [NSColorSpace sRGBColorSpace].CGColorSpace,
                                   kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (!_focus) return;
    CGContextScaleCTM(_focus, _focusScale, _focusScale);
    if (_reps.count || _handler) {
        CGImageRef old = [self hk_imageForSize:size scale:_focusScale];
        if (old) CGContextDrawImage(_focus, NSMakeRect(0, 0, size.width, size.height), old);
    }
    if (flipped) {
        CGContextTranslateCTM(_focus, 0, size.height);
        CGContextScaleCTM(_focus, 1, -1);
    }
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:_focus flipped:flipped]];
}
- (void)lockFocus { [self lockFocusFlipped:NO]; }
- (void)unlockFocus {
    if (!_focus) return;
    [NSGraphicsContext restoreGraphicsState];
    CGImageRef img = CGBitmapContextCreateImage(_focus);
    CGContextRelease(_focus);
    _focus = NULL;
    if (!img) return;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    rep.size = self.size;
    [_reps removeAllObjects];
    _handler = nil;
    [self addRepresentation:rep];
}
- (NSData *)TIFFRepresentation {
    NSSize size = self.size;
    CGImageRef img = [self hk_imageForSize:size scale:[self recommendedLayerContentsScale:0]];
    if (!img) return nil;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    rep.size = size;
    return rep.TIFFRepresentation;
}
- (NSData *)TIFFRepresentationUsingCompression:(NSUInteger)comp factor:(float)factor { return self.TIFFRepresentation; }
- (BOOL)hitTestRect:(NSRect)rect withImageDestinationRect:(NSRect)dst context:(NSGraphicsContext *)ctx hints:(NSDictionary *)hints
            flipped:(BOOL)flipped {
    return CGRectIntersectsRect(rect, dst);
}
- (NSString *)description {
    return [NSString stringWithFormat:@"<NSImage %p %@ size=%@ reps=%lu>", self, _name ?: @"", NSStringFromSize(self.size),
                                      (unsigned long)_reps.count];
}
@end

/* ================================================================ Bézier paths */

typedef NS_ENUM(NSUInteger, HKBezierElement) { HKMoveTo = 0, HKLineTo = 1, HKCurveTo = 2, HKClosePath = 3 };

@interface NSBezierPath : HKShimObject <NSCopying>
@end
@implementation NSBezierPath {
    CGMutablePathRef _path;
    CGFloat _lineWidth, _miterLimit, _flatness;
    NSInteger _cap, _join, _winding;
    CGFloat *_dash;
    NSInteger _dashCount;
    CGFloat _dashPhase;
    NSMutableData *_elements; /* cached (type, 3 points) records for elementAtIndex: */
}

static CGFloat g_default_line_width = 1, g_default_miter = 10, g_default_flatness = 0.6;
static NSInteger g_default_cap, g_default_join, g_default_winding;

typedef struct {
    NSInteger type;
    CGPoint pts[3];
} HKElement;

+ (instancetype)bezierPath { return [[self alloc] init]; }
+ (instancetype)bezierPathWithRect:(NSRect)rect {
    NSBezierPath *p = [self bezierPath];
    [p appendBezierPathWithRect:rect];
    return p;
}
+ (instancetype)bezierPathWithOvalInRect:(NSRect)rect {
    NSBezierPath *p = [self bezierPath];
    [p appendBezierPathWithOvalInRect:rect];
    return p;
}
+ (instancetype)bezierPathWithRoundedRect:(NSRect)rect xRadius:(CGFloat)rx yRadius:(CGFloat)ry {
    NSBezierPath *p = [self bezierPath];
    [p appendBezierPathWithRoundedRect:rect xRadius:rx yRadius:ry];
    return p;
}
+ (instancetype)bezierPathWithCGPath:(CGPathRef)path {
    NSBezierPath *p = [self bezierPath];
    if (path) CGPathAddPath(p->_path, NULL, path);
    return p;
}
- (instancetype)init {
    if ((self = [super init])) {
        _path = CGPathCreateMutable();
        _lineWidth = g_default_line_width;
        _miterLimit = g_default_miter;
        _flatness = g_default_flatness;
        _cap = g_default_cap;
        _join = g_default_join;
        _winding = g_default_winding;
    }
    return self;
}
- (void)dealloc {
    CGPathRelease(_path);
    free(_dash);
}
- (id)copyWithZone:(NSZone *)zone {
    NSBezierPath *p = [[NSBezierPath alloc] init];
    CGPathRelease(p->_path);
    p->_path = CGPathCreateMutableCopy(_path);
    p->_lineWidth = _lineWidth;
    p->_miterLimit = _miterLimit;
    p->_flatness = _flatness;
    p->_cap = _cap;
    p->_join = _join;
    p->_winding = _winding;
    [p setLineDash:_dash count:_dashCount phase:_dashPhase];
    return p;
}

+ (CGFloat)defaultLineWidth { return g_default_line_width; }
+ (void)setDefaultLineWidth:(CGFloat)w { g_default_line_width = w; }
+ (CGFloat)defaultMiterLimit { return g_default_miter; }
+ (void)setDefaultMiterLimit:(CGFloat)m { g_default_miter = m; }
+ (CGFloat)defaultFlatness { return g_default_flatness; }
+ (void)setDefaultFlatness:(CGFloat)f { g_default_flatness = f; }
+ (NSInteger)defaultLineCapStyle { return g_default_cap; }
+ (void)setDefaultLineCapStyle:(NSInteger)s { g_default_cap = s; }
+ (NSInteger)defaultLineJoinStyle { return g_default_join; }
+ (void)setDefaultLineJoinStyle:(NSInteger)s { g_default_join = s; }
+ (NSInteger)defaultWindingRule { return g_default_winding; }
+ (void)setDefaultWindingRule:(NSInteger)r { g_default_winding = r; }

- (CGFloat)lineWidth { return _lineWidth; }
- (void)setLineWidth:(CGFloat)w { _lineWidth = w; }
- (CGFloat)miterLimit { return _miterLimit; }
- (void)setMiterLimit:(CGFloat)m { _miterLimit = m; }
- (CGFloat)flatness { return _flatness; }
- (void)setFlatness:(CGFloat)f { _flatness = f; }
- (NSInteger)lineCapStyle { return _cap; }
- (void)setLineCapStyle:(NSInteger)s { _cap = s; }
- (NSInteger)lineJoinStyle { return _join; }
- (void)setLineJoinStyle:(NSInteger)s { _join = s; }
- (NSInteger)windingRule { return _winding; }
- (void)setWindingRule:(NSInteger)r { _winding = r; }
- (void)setLineDash:(const CGFloat *)pattern count:(NSInteger)count phase:(CGFloat)phase {
    free(_dash);
    _dash = NULL;
    _dashCount = pattern && count > 0 ? count : 0;
    _dashPhase = phase;
    if (_dashCount) {
        _dash = malloc(sizeof(CGFloat) * (size_t)count);
        memcpy(_dash, pattern, sizeof(CGFloat) * (size_t)count);
    }
}
- (void)getLineDash:(CGFloat *)pattern count:(NSInteger *)count phase:(CGFloat *)phase {
    if (pattern && _dash) memcpy(pattern, _dash, sizeof(CGFloat) * (size_t)_dashCount);
    if (count) *count = _dashCount;
    if (phase) *phase = _dashPhase;
}

- (void)hk_changed { _elements = nil; }
- (CGPathRef)CGPath { return _path; }
- (void)setCGPath:(CGPathRef)path {
    CGPathRelease(_path);
    _path = path ? CGPathCreateMutableCopy(path) : CGPathCreateMutable();
    [self hk_changed];
}
- (void)moveToPoint:(NSPoint)p {
    CGPathMoveToPoint(_path, NULL, p.x, p.y);
    [self hk_changed];
}
- (void)lineToPoint:(NSPoint)p {
    if (CGPathIsEmpty(_path)) CGPathMoveToPoint(_path, NULL, p.x, p.y);
    else CGPathAddLineToPoint(_path, NULL, p.x, p.y);
    [self hk_changed];
}
- (void)curveToPoint:(NSPoint)p controlPoint1:(NSPoint)c1 controlPoint2:(NSPoint)c2 {
    if (CGPathIsEmpty(_path)) CGPathMoveToPoint(_path, NULL, c1.x, c1.y);
    CGPathAddCurveToPoint(_path, NULL, c1.x, c1.y, c2.x, c2.y, p.x, p.y);
    [self hk_changed];
}
- (void)curveToPoint:(NSPoint)p controlPoint:(NSPoint)c {
    if (CGPathIsEmpty(_path)) CGPathMoveToPoint(_path, NULL, c.x, c.y);
    CGPathAddQuadCurveToPoint(_path, NULL, c.x, c.y, p.x, p.y);
    [self hk_changed];
}
- (void)closePath {
    if (!CGPathIsEmpty(_path)) CGPathCloseSubpath(_path);
    [self hk_changed];
}
- (NSPoint)currentPoint { return CGPathIsEmpty(_path) ? NSZeroPoint : CGPathGetCurrentPoint(_path); }
- (void)relativeMoveToPoint:(NSPoint)p {
    NSPoint c = self.currentPoint;
    [self moveToPoint:NSMakePoint(c.x + p.x, c.y + p.y)];
}
- (void)relativeLineToPoint:(NSPoint)p {
    NSPoint c = self.currentPoint;
    [self lineToPoint:NSMakePoint(c.x + p.x, c.y + p.y)];
}
- (void)relativeCurveToPoint:(NSPoint)p controlPoint1:(NSPoint)c1 controlPoint2:(NSPoint)c2 {
    NSPoint c = self.currentPoint;
    [self curveToPoint:NSMakePoint(c.x + p.x, c.y + p.y) controlPoint1:NSMakePoint(c.x + c1.x, c.y + c1.y)
         controlPoint2:NSMakePoint(c.x + c2.x, c.y + c2.y)];
}
- (void)removeAllPoints {
    CGPathRelease(_path);
    _path = CGPathCreateMutable();
    [self hk_changed];
}
- (void)appendBezierPath:(NSBezierPath *)path {
    if (path) CGPathAddPath(_path, NULL, path->_path);
    [self hk_changed];
}
- (void)appendBezierPathWithRect:(NSRect)rect {
    CGPathAddRect(_path, NULL, rect);
    [self hk_changed];
}
- (void)appendBezierPathWithOvalInRect:(NSRect)rect {
    CGPathAddEllipseInRect(_path, NULL, rect);
    [self hk_changed];
}
- (void)appendBezierPathWithRoundedRect:(NSRect)rect xRadius:(CGFloat)rx yRadius:(CGFloat)ry {
    rx = fmax(0, fmin(rx, rect.size.width / 2));
    ry = fmax(0, fmin(ry, rect.size.height / 2));
    if (rx > 0 && ry > 0) CGPathAddRoundedRect(_path, NULL, rect, rx, ry);
    else CGPathAddRect(_path, NULL, rect);
    [self hk_changed];
}
- (void)appendBezierPathWithArcWithCenter:(NSPoint)c radius:(CGFloat)r startAngle:(CGFloat)a0 endAngle:(CGFloat)a1
                                clockwise:(BOOL)clockwise {
    CGPathAddArc(_path, NULL, c.x, c.y, r, a0 * M_PI / 180, a1 * M_PI / 180, clockwise);
    [self hk_changed];
}
- (void)appendBezierPathWithArcWithCenter:(NSPoint)c radius:(CGFloat)r startAngle:(CGFloat)a0 endAngle:(CGFloat)a1 {
    [self appendBezierPathWithArcWithCenter:c radius:r startAngle:a0 endAngle:a1 clockwise:NO];
}
- (void)appendBezierPathWithArcFromPoint:(NSPoint)p1 toPoint:(NSPoint)p2 radius:(CGFloat)r {
    if (CGPathIsEmpty(_path)) return;
    CGPathAddArcToPoint(_path, NULL, p1.x, p1.y, p2.x, p2.y, r);
    [self hk_changed];
}
- (void)appendBezierPathWithPoints:(NSPoint *)points count:(NSInteger)count {
    for (NSInteger i = 0; i < count; i++) {
        if (i == 0 && CGPathIsEmpty(_path)) CGPathMoveToPoint(_path, NULL, points[0].x, points[0].y);
        else CGPathAddLineToPoint(_path, NULL, points[i].x, points[i].y);
    }
    [self hk_changed];
}
- (NSBezierPath *)bezierPathByFlatteningPath { return [self copy]; }
- (NSBezierPath *)bezierPathByReversingPath { return [self copy]; }
- (void)hk_apply:(CGAffineTransform)t {
    CGPathRef moved = CGPathCreateCopyByTransformingPath(_path, &t);
    CGPathRelease(_path);
    _path = CGPathCreateMutableCopy(moved);
    CGPathRelease(moved);
    [self hk_changed];
}
#if TARGET_OS_OSX
- (void)transformUsingAffineTransform:(NSAffineTransform *)transform {
    NSAffineTransformStruct s = transform.transformStruct;
    [self hk_apply:CGAffineTransformMake(s.m11, s.m12, s.m21, s.m22, s.tX, s.tY)];
}
#endif

static void collect_element(void *info, const CGPathElement *e) {
    NSMutableData *out = (__bridge NSMutableData *)info;
    HKElement el = {0};
    switch (e->type) {
    case kCGPathElementMoveToPoint: el.type = HKMoveTo, el.pts[0] = e->points[0]; break;
    case kCGPathElementAddLineToPoint: el.type = HKLineTo, el.pts[0] = e->points[0]; break;
    case kCGPathElementAddCurveToPoint:
        el.type = HKCurveTo, el.pts[0] = e->points[0], el.pts[1] = e->points[1], el.pts[2] = e->points[2];
        break;
    case kCGPathElementAddQuadCurveToPoint: {
        /* As a cubic: control points two thirds of the way to the quadratic's. */
        CGPoint p0 = CGPointZero;
        if (out.length) {
            HKElement *last = &((HKElement *)out.mutableBytes)[out.length / sizeof(HKElement) - 1];
            p0 = last->type == HKCurveTo ? last->pts[2] : last->pts[0];
        }
        CGPoint q = e->points[0], p = e->points[1];
        el.type = HKCurveTo;
        el.pts[0] = CGPointMake(p0.x + 2.0 / 3 * (q.x - p0.x), p0.y + 2.0 / 3 * (q.y - p0.y));
        el.pts[1] = CGPointMake(p.x + 2.0 / 3 * (q.x - p.x), p.y + 2.0 / 3 * (q.y - p.y));
        el.pts[2] = p;
        break;
    }
    case kCGPathElementCloseSubpath: el.type = HKClosePath; break;
    }
    [out appendBytes:&el length:sizeof el];
}
- (NSMutableData *)hk_elements {
    if (!_elements) {
        _elements = [NSMutableData data];
        CGPathApply(_path, (__bridge void *)_elements, collect_element);
    }
    return _elements;
}
- (NSInteger)elementCount { return (NSInteger)([self hk_elements].length / sizeof(HKElement)); }
- (NSUInteger)elementAtIndex:(NSInteger)index associatedPoints:(NSPoint *)points {
    if (index < 0 || index >= self.elementCount) return HKMoveTo;
    HKElement *e = &((HKElement *)[self hk_elements].mutableBytes)[index];
    if (points) {
        int n = e->type == HKCurveTo ? 3 : e->type == HKClosePath ? 0 : 1;
        for (int i = 0; i < n; i++) points[i] = e->pts[i];
    }
    return (NSUInteger)e->type;
}
- (NSUInteger)elementAtIndex:(NSInteger)index { return [self elementAtIndex:index associatedPoints:NULL]; }

- (BOOL)isEmpty { return CGPathIsEmpty(_path); }
- (NSRect)bounds { return CGPathIsEmpty(_path) ? NSZeroRect : CGPathGetPathBoundingBox(_path); }
- (NSRect)controlPointBounds { return CGPathIsEmpty(_path) ? NSZeroRect : CGPathGetBoundingBox(_path); }
- (BOOL)containsPoint:(NSPoint)p { return CGPathContainsPoint(_path, NULL, p, _winding == 1); }

- (void)hk_strokeSetup:(CGContextRef)c {
    CGContextSetLineWidth(c, _lineWidth);
    CGContextSetLineCap(c, (CGLineCap)_cap);
    CGContextSetLineJoin(c, (CGLineJoin)_join);
    CGContextSetMiterLimit(c, _miterLimit);
    CGContextSetFlatness(c, _flatness);
    if (_dashCount) CGContextSetLineDash(c, _dashPhase, _dash, (size_t)_dashCount);
}
- (void)fill {
    CGContextRef c = current_cg();
    if (!c) return;
    CGContextAddPath(c, _path);
    if (_winding == 1) CGContextEOFillPath(c);
    else CGContextFillPath(c);
}
- (void)stroke {
    CGContextRef c = current_cg();
    if (!c) return;
    CGContextSaveGState(c);
    [self hk_strokeSetup:c];
    CGContextAddPath(c, _path);
    CGContextStrokePath(c);
    CGContextRestoreGState(c);
}
- (void)addClip {
    CGContextRef c = current_cg();
    if (!c) return;
    CGContextAddPath(c, _path);
    if (_winding == 1) CGContextEOClip(c);
    else CGContextClip(c);
}
/* CoreGraphics cannot widen a clip, so this intersects with it as addClip does. */
- (void)setClip { [self addClip]; }

+ (void)fillRect:(NSRect)rect {
    CGContextRef c = current_cg();
    if (c) CGContextFillRect(c, rect);
}
+ (void)strokeRect:(NSRect)rect {
    CGContextRef c = current_cg();
    if (!c) return;
    CGContextSaveGState(c);
    CGContextSetLineWidth(c, g_default_line_width);
    CGContextStrokeRect(c, rect);
    CGContextRestoreGState(c);
}
+ (void)clipRect:(NSRect)rect {
    CGContextRef c = current_cg();
    if (c) CGContextClipToRect(c, rect);
}
+ (void)strokeLineFromPoint:(NSPoint)a toPoint:(NSPoint)b {
    CGContextRef c = current_cg();
    if (!c) return;
    CGContextSaveGState(c);
    CGContextSetLineWidth(c, g_default_line_width);
    CGContextSetLineCap(c, (CGLineCap)g_default_cap);
    CGPoint pts[2] = {a, b};
    CGContextStrokeLineSegments(c, pts, 2);
    CGContextRestoreGState(c);
}
- (NSString *)description {
    return [NSString stringWithFormat:@"<NSBezierPath %p %ld elements, bounds %@>", self, (long)self.elementCount,
                                      NSStringFromRect(self.bounds)];
}
@end
