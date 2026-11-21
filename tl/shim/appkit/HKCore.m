/* Henge's AppKit, the core: responders, views, windows, events, the application and its run loop, the screen.
   Windows are layer trees handed to the presenter; the run loop is the thread's CFRunLoop (on iOS the main thread's,
   shared with UIKit), with an event queue the presenter feeds. */
#import "HKShim.h"
#include "../../loader/tl_loader.h"
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <pthread.h>

static const HKPresenter *g_presenter;
static HKPresenter g_default_presenter = {{1280, 800}, 2, NULL};

void hk_set_presenter(const HKPresenter *presenter) { g_presenter = presenter; }
const HKPresenter *hk_presenter(void) { return g_presenter ? g_presenter : &g_default_presenter; }

static void tell_presenter(NSWindow *w, int visible) {
    const HKPresenter *p = hk_presenter();
    if (p->window_changed) p->window_changed(w, visible);
}

/* A class method of one of this AppKit's own classes (never looked up by name: iOS has classes called NSColor and
   NSFont of its own). */
static id cls_msg(Class c, SEL sel) { return ((id(*)(id, SEL))objc_msgSend)(c, sel); }

static NSTimeInterval now_seconds(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static void no_actions(void (^block)(void)) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    block();
    [CATransaction commit];
}

/* ================================================================ responders */

@implementation NSResponder
+ (void)load { hk_add_fallback(self); }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)becomeFirstResponder { return YES; }
- (BOOL)resignFirstResponder { return YES; }
- (BOOL)tryToPerform:(SEL)action with:(id)object {
    if ([self respondsToSelector:action]) {
        ((void (*)(id, SEL, id))objc_msgSend)(self, action, object);
        return YES;
    }
    return [_nextResponder tryToPerform:action with:object];
}
#define PASS_UP(sel) \
    - (void)sel:(NSEvent *)event { if (_nextResponder) [_nextResponder sel:event]; }
PASS_UP(mouseDown) PASS_UP(mouseUp) PASS_UP(mouseDragged) PASS_UP(mouseMoved) PASS_UP(mouseEntered) PASS_UP(mouseExited)
PASS_UP(rightMouseDown) PASS_UP(rightMouseUp) PASS_UP(rightMouseDragged) PASS_UP(otherMouseDown) PASS_UP(otherMouseUp)
PASS_UP(otherMouseDragged) PASS_UP(scrollWheel) PASS_UP(keyDown) PASS_UP(keyUp) PASS_UP(flagsChanged) PASS_UP(cursorUpdate)
PASS_UP(magnifyWithEvent) PASS_UP(swipeWithEvent) PASS_UP(rotateWithEvent) PASS_UP(smartMagnifyWithEvent)
PASS_UP(pressureChangeWithEvent) PASS_UP(beginGestureWithEvent) PASS_UP(endGestureWithEvent)
#undef PASS_UP
- (void)noResponderFor:(SEL)selector {}
- (BOOL)performKeyEquivalent:(NSEvent *)event { return NO; }
- (void)interpretKeyEvents:(NSArray<NSEvent *> *)events {
    id ctx = [self respondsToSelector:@selector(inputContext)] ? [(id)self inputContext] : nil;
    for (NSEvent *e in events)
        if (ctx) ((BOOL(*)(id, SEL, id))objc_msgSend)(ctx, @selector(handleEvent:), e);
}
- (void)doCommandBySelector:(SEL)selector {
    if (![self tryToPerform:selector with:nil]) [self noResponderFor:selector];
}
- (id)validRequestorForSendType:(NSString *)sendType returnType:(NSString *)returnType {
    return [_nextResponder validRequestorForSendType:sendType returnType:returnType];
}
- (void)encodeWithCoder:(NSCoder *)coder {}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (BOOL)isAccessibilityElement { return NO; }
- (void)invalidateRestorableState {}
/* AppKit's responders (NSApplication above all) observe things themselves, so an app's observer passing an
   unknown change on to super is fine there; NSObject's version would raise. */
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {}
@end

/* ================================================================ views */

@interface NSView () {
  @public
    NSRect _frame, _bounds;
    __unsafe_unretained NSView *_superview;
    __unsafe_unretained NSWindow *_window;
    NSMutableArray<NSView *> *_subviews;
    NSMutableArray<NSTrackingArea *> *_trackingAreas;
    CALayer *_layer;
    id _inputContext;
}
@end

@interface NSWindow ()
- (void)hk_viewWillGo:(NSView *)view;
@end

@implementation NSView
@synthesize autoresizingMask = _autoresizingMask, autoresizesSubviews = _autoresizesSubviews, wantsLayer = _wantsLayer,
            postsFrameChangedNotifications = _postsFrameChangedNotifications,
            postsBoundsChangedNotifications = _postsBoundsChangedNotifications, identifier = _identifier,
            toolTip = _toolTip, appearance = _appearance, needsDisplay = _needsDisplay, needsLayout = _needsLayout,
            translatesAutoresizingMaskIntoConstraints = _translatesAutoresizingMaskIntoConstraints;

+ (void)load { hk_add_fallback(self); }

- (instancetype)init { return [self initWithFrame:NSZeroRect]; }

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super init])) {
        _frame = frame;
        _bounds = (NSRect){NSZeroPoint, frame.size};
        _subviews = [NSMutableArray new];
        _trackingAreas = [NSMutableArray new];
        _autoresizesSubviews = YES;
        _translatesAutoresizingMaskIntoConstraints = YES;
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder { return [self initWithFrame:NSZeroRect]; }

/* AppKit's rule: a backing layer is flipped when its view's flippedness differs from its superview's; then every
   sublayer's frame is simply its view's frame. */
- (void)hk_syncLayerFlip {
    if (!_layer) return;
    BOOL parent = _superview ? [_superview isFlipped] : NO;
    _layer.geometryFlipped = [self isFlipped] != parent;
}

- (CALayer *)makeBackingLayer { return [CALayer layer]; }

- (CALayer *)layer {
    if (!_layer) {
        CALayer *l = [self makeBackingLayer];
        [self hk_adoptLayer:l];
    }
    return _layer;
}

- (void)setLayer:(CALayer *)layer { [self hk_adoptLayer:layer]; }

- (void)hk_adoptLayer:(CALayer *)layer {
    if (layer == _layer) return;
    CALayer *old = _layer;
    no_actions(^{
        if (layer) {
            if (!layer.delegate) layer.delegate = (id)self;
            layer.anchorPoint = CGPointZero;
            layer.frame = self->_frame;
            layer.hidden = self.hidden;
            CGFloat scale = self->_window ? self->_window.backingScaleFactor : hk_presenter()->scale;
            layer.contentsScale = scale;
        }
        if (old) {
            for (NSView *v in self->_subviews)
                if (v->_layer.superlayer == old) [v->_layer removeFromSuperlayer];
            if (old.superlayer && layer) [old.superlayer replaceSublayer:old with:layer];
            else [old removeFromSuperlayer];
        } else if (layer && self->_superview) {
            [self->_superview.layer addSublayer:layer];
        }
        self->_layer = layer;
        for (NSView *v in self->_subviews)
            if (v->_layer || layer) [layer addSublayer:v.layer];
    });
    [self hk_syncLayerFlip];
    for (NSView *v in _subviews) [v hk_syncLayerFlip];
    if ([self hk_draws]) [_layer setNeedsDisplay];
}

/* Whether a subclass draws itself (overrides drawRect:): then its layer is drawn through drawLayer:inContext:. */
- (BOOL)hk_draws {
    static IMP base;
    if (!base) base = class_getMethodImplementation([NSView class], @selector(drawRect:));
    return class_getMethodImplementation([self class], @selector(drawRect:)) != base;
}

- (id<CAAction>)actionForLayer:(CALayer *)layer forKey:(NSString *)event { return (id)[NSNull null]; }

- (void)drawLayer:(CALayer *)layer inContext:(CGContextRef)ctx {
    if (![self hk_draws]) return;
    Class gc = [NSGraphicsContext class];
    id g = ((id(*)(id, SEL, CGContextRef, BOOL))objc_msgSend)(gc, @selector(graphicsContextWithCGContext:flipped:), ctx, [self isFlipped]);
    ((void (*)(id, SEL))objc_msgSend)(gc, @selector(saveGraphicsState));
    ((void (*)(id, SEL, id))objc_msgSend)(gc, @selector(setCurrentContext:), g);
    [self drawRect:_bounds];
    ((void (*)(id, SEL))objc_msgSend)(gc, @selector(restoreGraphicsState));
}

/* Core Animation calls this instead of drawLayer:inContext: when the delegate has it: a view that wants
   updateLayer gets that, any other draws into a bitmap that becomes the layer's contents. */
- (void)displayLayer:(CALayer *)layer {
    if ([self wantsUpdateLayer]) {
        [self updateLayer];
        return;
    }
    if (![self hk_draws]) return;
    CGFloat scale = layer.contentsScale > 0 ? layer.contentsScale : 1;
    size_t w = (size_t)ceil(layer.bounds.size.width * scale), h = (size_t)ceil(layer.bounds.size.height * scale);
    if (!w || !h) return;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) return;
    CGContextScaleCTM(ctx, scale, scale);
    if ([self isFlipped]) { /* bitmap contexts are y-up; a flipped view draws y-down */
        CGContextTranslateCTM(ctx, 0, layer.bounds.size.height);
        CGContextScaleCTM(ctx, 1, -1);
    }
    [self drawLayer:layer inContext:ctx];
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    no_actions(^{
        layer.contents = (__bridge id)img;
    });
    CGImageRelease(img);
}

- (BOOL)wantsUpdateLayer { return NO; }
- (void)updateLayer {}
- (void)drawRect:(NSRect)rect {}
- (BOOL)isFlipped { return NO; }
- (BOOL)isOpaque { return NO; }

- (NSRect)frame { return _frame; }
- (NSRect)bounds { return _bounds; }
- (NSView *)superview { return _superview; }

/* A view going away leaves its subviews without a superview (they only point back at it, unretained), and its layer
   without it as delegate, as AppKit does: a subview later removed from its superview must not message a freed one. */
- (void)dealloc {
    for (NSView *v in _subviews)
        if (v->_superview == self) v->_superview = nil;
    if (_layer.delegate == (id)self) _layer.delegate = nil;
}
- (NSWindow *)window { return _window; }
- (NSArray<NSView *> *)subviews { return [_subviews copy]; }
- (NSArray<NSTrackingArea *> *)trackingAreas { return [_trackingAreas copy]; }

- (void)setSubviews:(NSArray<NSView *> *)subviews {
    for (NSView *v in [_subviews copy])
        if (![subviews containsObject:v]) [v removeFromSuperview];
    for (NSView *v in subviews)
        if (v->_superview != self) [self addSubview:v];
    [_subviews setArray:subviews];
    no_actions(^{
        for (NSView *v in subviews) [self->_layer addSublayer:v.layer];
    });
}

- (void)hk_setWindow:(NSWindow *)window {
    if (_window == window) return;
    [self viewWillMoveToWindow:window];
    if (_window && !window) [_window hk_viewWillGo:self];
    _window = window;
    if (_layer && window) _layer.contentsScale = window.backingScaleFactor;
    for (NSView *v in [_subviews copy]) [v hk_setWindow:window];
    [self viewDidMoveToWindow];
}

- (void)addSubview:(NSView *)view { [self addSubview:view positioned:NSWindowAbove relativeTo:nil]; }

- (void)addSubview:(NSView *)view positioned:(NSWindowOrderingMode)place relativeTo:(NSView *)other {
    if (!view || view == self) return;
    if (view->_superview && view->_superview != self) [view removeFromSuperview];
    [view viewWillMoveToSuperview:self];
    [_subviews removeObjectIdenticalTo:view];
    NSUInteger at = other ? [_subviews indexOfObjectIdenticalTo:other] : NSNotFound;
    if (at == NSNotFound) at = place == NSWindowBelow ? 0 : _subviews.count;
    else if (place != NSWindowBelow) at++;
    [_subviews insertObject:view atIndex:at];
    view->_superview = self;
    view.nextResponder = self;
    CALayer *mine = self.layer, *theirs = view.layer;
    no_actions(^{
        [theirs removeFromSuperlayer];
        if (at >= mine.sublayers.count) [mine addSublayer:theirs];
        else [mine insertSublayer:theirs atIndex:(unsigned)at];
    });
    [view hk_syncLayerFlip];
    [view hk_setWindow:_window];
    [self didAddSubview:view];
    [view viewDidMoveToSuperview];
}

- (void)removeFromSuperview {
    NSView *sup = _superview;
    if (!sup) return;
    [self viewWillMoveToSuperview:nil];
    [sup willRemoveSubview:self];
    [self hk_setWindow:nil];
    no_actions(^{
        [self->_layer removeFromSuperlayer];
    });
    _superview = nil;
    if (self.nextResponder == sup) self.nextResponder = nil;
    [sup->_subviews removeObjectIdenticalTo:self];
    [self viewDidMoveToSuperview];
}

- (void)replaceSubview:(NSView *)old with:(NSView *)replacement {
    NSUInteger i = [_subviews indexOfObjectIdenticalTo:old];
    if (i == NSNotFound) return;
    NSView *below = i > 0 ? _subviews[i - 1] : nil;
    [old removeFromSuperview];
    if (below) [self addSubview:replacement positioned:NSWindowAbove relativeTo:below];
    else [self addSubview:replacement positioned:NSWindowBelow relativeTo:nil];
}

- (void)viewWillMoveToWindow:(NSWindow *)window {}
- (void)viewDidMoveToWindow {}
- (void)viewWillMoveToSuperview:(NSView *)superview {}
- (void)viewDidMoveToSuperview {}
- (void)viewDidChangeBackingProperties {}
- (void)viewDidChangeEffectiveAppearance {}
- (void)didAddSubview:(NSView *)view {}
- (void)willRemoveSubview:(NSView *)view {}
- (void)viewWillDraw {}

- (BOOL)isDescendantOf:(NSView *)view {
    for (NSView *v = self; v; v = v->_superview)
        if (v == view) return YES;
    return NO;
}

- (void)hk_layerFrame {
    CALayer *l = _layer;
    NSRect f = _frame;
    no_actions(^{
        l.frame = f;
        l.bounds = (CGRect){l.bounds.origin, f.size};
    });
}

- (void)setFrame:(NSRect)frame {
    [self setFrameOrigin:frame.origin];
    [self setFrameSize:frame.size];
}

- (void)setFrameOrigin:(NSPoint)origin {
    if (CGPointEqualToPoint(origin, _frame.origin)) return;
    _frame.origin = origin;
    [self hk_layerFrame];
    if (_postsFrameChangedNotifications)
        [[NSNotificationCenter defaultCenter] postNotificationName:@"NSViewFrameDidChangeNotification" object:self];
}

- (void)setFrameSize:(NSSize)size {
    NSSize old = _frame.size;
    if (CGSizeEqualToSize(old, size)) return;
    _frame.size = size;
    _bounds.size = size;
    [self hk_layerFrame];
    if (_autoresizesSubviews) [self resizeSubviewsWithOldSize:old];
    _needsLayout = YES;
    if ([self hk_draws]) [_layer setNeedsDisplay];
    if (_postsFrameChangedNotifications)
        [[NSNotificationCenter defaultCenter] postNotificationName:@"NSViewFrameDidChangeNotification" object:self];
}

- (void)setBounds:(NSRect)bounds {
    _bounds = bounds;
    if (_postsBoundsChangedNotifications)
        [[NSNotificationCenter defaultCenter] postNotificationName:@"NSViewBoundsDidChangeNotification" object:self];
}
- (void)setBoundsOrigin:(NSPoint)origin { [self setBounds:(NSRect){origin, _bounds.size}]; }
- (void)setBoundsSize:(NSSize)size { [self setBounds:(NSRect){_bounds.origin, size}]; }

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    for (NSView *v in [_subviews copy]) [v resizeWithOldSuperviewSize:old];
}

- (void)resizeWithOldSuperviewSize:(NSSize)old {
    NSAutoresizingMaskOptions m = _autoresizingMask;
    if (!m || !_superview) return;
    NSSize now = _superview->_bounds.size;
    NSRect f = _frame;
    CGFloat dw = now.width - old.width, dh = now.height - old.height;
    int hx = (m & NSViewMinXMargin ? 1 : 0) + (m & NSViewWidthSizable ? 1 : 0) + (m & NSViewMaxXMargin ? 1 : 0);
    int hy = (m & NSViewMinYMargin ? 1 : 0) + (m & NSViewHeightSizable ? 1 : 0) + (m & NSViewMaxYMargin ? 1 : 0);
    if (hx) {
        CGFloat share = dw / hx;
        if (m & NSViewMinXMargin) f.origin.x += share;
        if (m & NSViewWidthSizable) f.size.width += share;
    }
    if (hy) {
        CGFloat share = dh / hy;
        BOOL flipped = [_superview isFlipped];
        if (m & (flipped ? NSViewMaxYMargin : NSViewMinYMargin)) f.origin.y += share;
        if (m & NSViewHeightSizable) f.size.height += share;
    }
    [self setFrame:f];
}

- (void)setHidden:(BOOL)hidden {
    if (self.hidden == hidden) return;
    objc_setAssociatedObject(self, @selector(isHidden), @(hidden), OBJC_ASSOCIATION_RETAIN);
    CALayer *l = _layer;
    no_actions(^{
        l.hidden = hidden;
    });
}
- (BOOL)isHidden { return [objc_getAssociatedObject(self, @selector(isHidden)) boolValue]; }
- (BOOL)isHiddenOrHasHiddenAncestor {
    for (NSView *v = self; v; v = v->_superview)
        if (v.hidden) return YES;
    return NO;
}

- (CGFloat)alphaValue { return _layer ? _layer.opacity : 1; }
- (void)setAlphaValue:(CGFloat)alpha {
    CALayer *l = self.layer;
    no_actions(^{
        l.opacity = (float)alpha;
    });
}

/* ---- coordinates: a point in this view's bounds to its superview's (the window's for the top view) ---- */
- (NSPoint)hk_toSuper:(NSPoint)p {
    BOOL superFlipped = _superview ? [_superview isFlipped] : NO;
    CGFloat fromEdge = p.y - _bounds.origin.y; /* from the top if flipped, else from the bottom */
    CGFloat y = ([self isFlipped] == superFlipped) ? _frame.origin.y + fromEdge : _frame.origin.y + _frame.size.height - fromEdge;
    return NSMakePoint(_frame.origin.x + (p.x - _bounds.origin.x), y);
}

- (NSPoint)hk_fromSuper:(NSPoint)p {
    BOOL superFlipped = _superview ? [_superview isFlipped] : NO;
    CGFloat fromEdge = ([self isFlipped] == superFlipped) ? p.y - _frame.origin.y : _frame.origin.y + _frame.size.height - p.y;
    return NSMakePoint(p.x - _frame.origin.x + _bounds.origin.x, fromEdge + _bounds.origin.y);
}

- (NSPoint)hk_toWindow:(NSPoint)p {
    for (NSView *v = self; v; v = v->_superview) p = [v hk_toSuper:p];
    return p;
}

- (NSPoint)hk_fromWindow:(NSPoint)p {
    NSMutableArray *chain = [NSMutableArray array];
    for (NSView *v = self; v; v = v->_superview) [chain insertObject:v atIndex:0];
    for (NSView *v in chain) p = [v hk_fromSuper:p];
    return p;
}

- (NSPoint)convertPoint:(NSPoint)point fromView:(NSView *)view {
    NSPoint w = view ? [view hk_toWindow:point] : point;
    return [self hk_fromWindow:w];
}

- (NSPoint)convertPoint:(NSPoint)point toView:(NSView *)view {
    NSPoint w = [self hk_toWindow:point];
    return view ? [view hk_fromWindow:w] : w;
}

static NSRect rect_of(NSPoint a, NSPoint b) {
    return NSMakeRect(MIN(a.x, b.x), MIN(a.y, b.y), fabs(a.x - b.x), fabs(a.y - b.y));
}

- (NSRect)convertRect:(NSRect)r fromView:(NSView *)view {
    return rect_of([self convertPoint:r.origin fromView:view], [self convertPoint:NSMakePoint(NSMaxX(r), NSMaxY(r)) fromView:view]);
}
- (NSRect)convertRect:(NSRect)r toView:(NSView *)view {
    return rect_of([self convertPoint:r.origin toView:view], [self convertPoint:NSMakePoint(NSMaxX(r), NSMaxY(r)) toView:view]);
}
- (NSSize)convertSize:(NSSize)size fromView:(NSView *)view { return size; }
- (NSSize)convertSize:(NSSize)size toView:(NSView *)view { return size; }

- (CGFloat)hk_scale { return _window ? _window.backingScaleFactor : hk_presenter()->scale; }
- (NSRect)convertRectToBacking:(NSRect)r { CGFloat s = [self hk_scale]; return NSMakeRect(r.origin.x * s, r.origin.y * s, r.size.width * s, r.size.height * s); }
- (NSRect)convertRectFromBacking:(NSRect)r { CGFloat s = [self hk_scale]; return NSMakeRect(r.origin.x / s, r.origin.y / s, r.size.width / s, r.size.height / s); }
- (NSPoint)convertPointToBacking:(NSPoint)p { CGFloat s = [self hk_scale]; return NSMakePoint(p.x * s, p.y * s); }
- (NSPoint)convertPointFromBacking:(NSPoint)p { CGFloat s = [self hk_scale]; return NSMakePoint(p.x / s, p.y / s); }
- (NSSize)convertSizeToBacking:(NSSize)z { CGFloat s = [self hk_scale]; return NSMakeSize(z.width * s, z.height * s); }
- (NSSize)convertSizeFromBacking:(NSSize)z { CGFloat s = [self hk_scale]; return NSMakeSize(z.width / s, z.height / s); }
- (NSRect)convertRectToLayer:(NSRect)r { return r; }
- (NSRect)convertRectFromLayer:(NSRect)r { return r; }
- (NSPoint)convertPointToLayer:(NSPoint)p { return p; }

- (BOOL)mouse:(NSPoint)p inRect:(NSRect)r { return NSPointInRect(p, r); }

- (NSView *)hitTest:(NSPoint)p {
    if (self.hidden || !NSPointInRect(p, _frame)) return nil;
    NSPoint local = [self hk_fromSuper:p];
    for (NSView *v in [_subviews reverseObjectEnumerator]) {
        NSView *hit = [v hitTest:local];
        if (hit) return hit;
    }
    return self;
}

- (NSRect)visibleRect { return _bounds; }

/* A view whose layer it fills: one that draws (drawRect:) or updates its layer itself (wantsUpdateLayer; Qt 6
   paints only from updateLayer). */
- (BOOL)hk_fillsLayer {
    static IMP base;
    if (!base) base = class_getMethodImplementation([NSView class], @selector(displayLayer:));
    return [self hk_draws] || [self wantsUpdateLayer] ||
           class_getMethodImplementation([self class], @selector(displayLayer:)) != base; /* Qt 6's QNSView */
}
- (void)setNeedsDisplay:(BOOL)flag {
    _needsDisplay = flag;
    if (flag && [self hk_fillsLayer]) [self.layer setNeedsDisplay];
}
- (void)setNeedsDisplayInRect:(NSRect)rect { [self setNeedsDisplay:YES]; }
- (void)display { if ([self hk_fillsLayer]) [self.layer displayIfNeeded]; }
- (void)displayIfNeeded { [self display]; }
- (void)displayRect:(NSRect)rect { [self display]; }
- (void)layout {}
- (void)layoutSubtreeIfNeeded {
    if (_needsLayout) {
        _needsLayout = NO;
        [self layout];
    }
    for (NSView *v in [_subviews copy]) [v layoutSubtreeIfNeeded];
}
- (void)updateConstraints {}
- (void)updateConstraintsForSubtreeIfNeeded {}
- (BOOL)canDrawSubviewsIntoLayer { return NO; }
- (BOOL)inLiveResize { return NO; }

/* The old tracking rects: a tag (never 0) for a tracking area with enter/exit for the owner. */
- (NSInteger)addTrackingRect:(NSRect)rect owner:(id)owner userData:(void *)data assumeInside:(BOOL)inside {
    static NSInteger next = 1;
    NSTrackingArea *a = [[NSTrackingArea alloc] initWithRect:rect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways
                                                       owner:owner userInfo:nil];
    NSInteger tag = next++;
    objc_setAssociatedObject(a, @selector(addTrackingRect:owner:userData:assumeInside:), @(tag), OBJC_ASSOCIATION_RETAIN);
    [self addTrackingArea:a];
    return tag;
}
- (void)removeTrackingRect:(NSInteger)tag {
    for (NSTrackingArea *a in [_trackingAreas copy])
        if ([objc_getAssociatedObject(a, @selector(addTrackingRect:owner:userData:assumeInside:)) integerValue] == tag)
            [_trackingAreas removeObjectIdenticalTo:a];
}
- (void)setWantsBestResolutionOpenGLSurface:(BOOL)flag {}
- (void)addTrackingArea:(NSTrackingArea *)area { if (area && ![_trackingAreas containsObject:area]) [_trackingAreas addObject:area]; }
- (void)removeTrackingArea:(NSTrackingArea *)area { [_trackingAreas removeObjectIdenticalTo:area]; }
- (void)updateTrackingAreas {}

- (NSTextInputContext *)inputContext {
    /* A text input client by protocol, or by its methods (a Rust (objc2) view declares the protocol by name, which
       need not resolve without AppKit's runtime protocol object). */
    if (!_inputContext && ([self conformsToProtocol:NSProtocolFromString(@"NSTextInputClient")] ||
                           [self respondsToSelector:NSSelectorFromString(@"insertText:replacementRange:")])) {
        Class c = [NSTextInputContext class];
        _inputContext = ((id(*)(id, SEL, id))objc_msgSend)([c alloc], @selector(initWithClient:), self);
    }
    return _inputContext;
}

- (NSAppearance *)effectiveAppearance {
    if (_appearance) return _appearance;
    if (_superview) return _superview.effectiveAppearance;
    if (_window) return _window.effectiveAppearance;
    return NSApp.effectiveAppearance;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event { return NO; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)needsPanelToBecomeKey { return NO; }
- (NSView *)ancestorSharedWithView:(NSView *)view {
    for (NSView *v = self; v; v = v->_superview)
        if ([view isDescendantOf:v]) return v;
    return nil;
}
- (NSView *)opaqueAncestor { return self; }
- (void)setPostsFrameChangedNotifications:(BOOL)f { _postsFrameChangedNotifications = f; }
- (void)registerForDraggedTypes:(NSArray *)types {}
- (void)unregisterDraggedTypes {}
- (void)scrollRectToVisible:(NSRect)rect {}
- (BOOL)autoscroll:(NSEvent *)event { return NO; }
- (void)discardCursorRects {}
- (void)resetCursorRects {}
- (void)addCursorRect:(NSRect)rect cursor:(NSCursor *)cursor {}
- (void)setAccessibilityElement:(BOOL)flag {}
- (BOOL)isAccessibilityElement { return NO; }
- (id)accessibilityParent { return _superview ?: (id)_window; }
- (NSString *)description {
    return [NSString stringWithFormat:@"<%@ %p %@>", NSStringFromClass([self class]), self, NSStringFromRect(_frame)];
}
@end

/* ================================================================ windows */

static NSMutableArray<NSWindow *> *g_windows;        /* every window not closed, back to front */
static __unsafe_unretained NSWindow *g_key, *g_main;
static NSInteger g_next_number = 1;
static NSPoint g_mouse;                               /* screen coordinates */
static NSUInteger g_buttons;
static NSEventModifierFlags g_modifiers;

/* The view between a window and its content view (AppKit's NSThemeFrame). */
@interface HKThemeFrame : NSView
@end
@implementation HKThemeFrame
@end

@interface NSWindow () {
    NSRect _frame;
    NSView *_contentView;
    NSView *_frameView; /* the content view's superview, as AppKit's theme frame (programs swizzle on its class) */
    CALayer *_root;
    NSInteger _number;
    BOOL _visible;
    NSResponder *_firstResponder;
    NSMutableArray<NSWindow *> *_children;
    __unsafe_unretained NSView *_mouseDownView;
    __unsafe_unretained NSView *_hoverView;
    NSEvent *_currentEvent;
}
@end

static NSArray<NSString *> *window_notifications(void) {
    return @[ @"NSWindowDidResizeNotification", @"NSWindowDidMoveNotification", @"NSWindowWillMoveNotification",
              @"NSWindowDidBecomeKeyNotification", @"NSWindowDidResignKeyNotification",
              @"NSWindowDidBecomeMainNotification", @"NSWindowDidResignMainNotification",
              @"NSWindowWillCloseNotification", @"NSWindowDidChangeBackingPropertiesNotification",
              @"NSWindowDidChangeScreenNotification", @"NSWindowDidChangeOcclusionStateNotification",
              @"NSWindowWillStartLiveResizeNotification", @"NSWindowDidEndLiveResizeNotification",
              @"NSWindowWillEnterFullScreenNotification", @"NSWindowDidEnterFullScreenNotification",
              @"NSWindowWillExitFullScreenNotification", @"NSWindowDidExitFullScreenNotification",
              @"NSWindowDidMiniaturizeNotification", @"NSWindowDidDeminiaturizeNotification",
              @"NSWindowDidExposeNotification", @"NSWindowDidUpdateNotification" ];
}

/* NSWindowDidResizeNotification -> windowDidResize: */
static SEL delegate_selector(NSString *note, NSString *prefix) {
    NSString *s = [note substringFromIndex:2];
    s = [s substringToIndex:s.length - 12];
    s = [[[s substringToIndex:1] lowercaseString] stringByAppendingString:[s substringFromIndex:1]];
    return NSSelectorFromString([s stringByAppendingString:@":"]);
}

@implementation NSWindow
@synthesize delegate = _delegate, title = _title, styleMask = _styleMask, level = _level,
            backgroundColor = _backgroundColor, opaque = _opaque, hasShadow = _hasShadow, alphaValue = _alphaValue,
            ignoresMouseEvents = _ignoresMouseEvents, acceptsMouseMovedEvents = _acceptsMouseMovedEvents,
            releasedWhenClosed = _releasedWhenClosed, collectionBehavior = _collectionBehavior, minSize = _minSize,
            maxSize = _maxSize, contentMinSize = _contentMinSize, contentMaxSize = _contentMaxSize,
            parentWindow = _parentWindow, appearance = _appearance;

+ (void)load { hk_add_fallback(self); }

- (instancetype)init { return [self initWithContentRect:NSMakeRect(0, 0, 400, 300) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO]; }

- (instancetype)initWithContentRect:(NSRect)rect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing defer:(BOOL)defer screen:(NSScreen *)screen {
    return [self initWithContentRect:rect styleMask:style backing:backing defer:defer];
}

- (instancetype)initWithContentRect:(NSRect)rect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing defer:(BOOL)defer {
    if ((self = [super init])) {
        _frame = rect;
        _styleMask = style;
        _frame = [self constrainFrameRect:rect toScreen:nil];
        rect = _frame;
        _number = g_next_number++;
        _alphaValue = 1;
        _hasShadow = YES;
        _releasedWhenClosed = YES;
        _children = [NSMutableArray new];
        _root = [CALayer layer];
        _root.anchorPoint = CGPointZero;
        _root.delegate = (id)self;
        _root.frame = (CGRect){CGPointZero, rect.size};
        _root.contentsScale = self.backingScaleFactor;
        _root.masksToBounds = YES;
        if (!g_windows) g_windows = [NSMutableArray new];
        [g_windows addObject:self];
        self.backgroundColor = cls_msg([NSColor class], @selector(windowBackgroundColor));
        self.contentView = [[NSView alloc] initWithFrame:(NSRect){NSZeroPoint, rect.size}];
    }
    return self;
}

- (id<CAAction>)actionForLayer:(CALayer *)layer forKey:(NSString *)event { return (id)[NSNull null]; }

/* A window is filled with its background color, as AppKit's frame view does (windowBackgroundColor by default). */
- (NSColor *)backgroundColor { return _backgroundColor ?: cls_msg([NSColor class], @selector(windowBackgroundColor)); }
- (void)setBackgroundColor:(NSColor *)color {
    _backgroundColor = color;
    CGColorRef cg = color ? ((CGColorRef(*)(id, SEL))objc_msgSend)(color, @selector(CGColor)) : NULL;
    CALayer *root = _root;
    no_actions(^{
        root.backgroundColor = cg;
    });
}

- (CALayer *)hk_rootLayer { return _root; }
- (NSRect)frame { return _frame; }
- (NSInteger)windowNumber { return _number; }
- (BOOL)isVisible { return _visible; }
- (BOOL)isKeyWindow { return g_key == self; }
- (BOOL)isMainWindow { return g_main == self; }
- (NSResponder *)firstResponder { return _firstResponder ?: self; }
- (CGFloat)backingScaleFactor { return hk_presenter()->scale; }
- (NSScreen *)screen { return [NSScreen mainScreen]; }
- (NSScreen *)deepestScreen { return [NSScreen mainScreen]; }
- (NSArray<NSWindow *> *)childWindows { return [_children copy]; }
- (NSWindowOcclusionState)occlusionState { return _visible ? NSWindowOcclusionStateVisible : 0; }
- (NSEvent *)currentEvent { return NSApp.currentEvent; }
- (NSView *)contentView { return _contentView; }
- (NSAppearance *)effectiveAppearance { return _appearance ?: NSApp.effectiveAppearance; }

- (void)setDelegate:(id)delegate {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    for (NSString *n in window_notifications()) {
        if (_delegate) [nc removeObserver:_delegate name:n object:self];
        SEL sel = delegate_selector(n, @"window");
        if (delegate && [delegate respondsToSelector:sel]) [nc addObserver:delegate selector:sel name:n object:self];
    }
    _delegate = delegate;
}

- (void)hk_post:(NSString *)name {
    [[NSNotificationCenter defaultCenter] postNotificationName:name object:self];
}

- (void)setContentView:(NSView *)view {
    if (view == _contentView) return;
    NSView *old = _contentView;
    if (old) {
        [old hk_setWindow:nil];
        no_actions(^{
            [old->_layer removeFromSuperlayer];
        });
        if (old.nextResponder == self) old.nextResponder = nil;
        if (old->_superview == _frameView) {
            old->_superview = nil;
            [_frameView->_subviews removeObjectIdenticalTo:old];
        }
    }
    _contentView = view;
    if (!view) return;
    if (view->_superview) [view removeFromSuperview];
    /* The theme frame: the whole window, unflipped at the origin, so the content view's coordinates are as without
       it; it draws nothing, and its layer is not shown (the content view's layer hangs from the window's root). */
    if (!_frameView) {
        _frameView = [[HKThemeFrame alloc] initWithFrame:(NSRect){NSZeroPoint, _frame.size}];
        [_frameView hk_setWindow:self];
        _frameView.nextResponder = self;
    }
    view->_superview = _frameView;
    if (!_frameView->_subviews) _frameView->_subviews = [NSMutableArray array];
    [_frameView->_subviews addObject:view];
    [view setFrame:(NSRect){NSZeroPoint, _frame.size}];
    CALayer *l = view.layer, *root = _root;
    no_actions(^{
        [root addSublayer:l];
    });
    [view hk_syncLayerFlip];
    view.nextResponder = self;
    [view hk_setWindow:self];
    [self hk_changed];
}

- (void)hk_viewWillGo:(NSView *)view {
    if (_mouseDownView && [_mouseDownView isDescendantOf:view]) _mouseDownView = nil;
    if (_hoverView && [_hoverView isDescendantOf:view]) _hoverView = nil;
    if ([_firstResponder isKindOfClass:[NSView class]] && [(NSView *)_firstResponder isDescendantOf:view]) _firstResponder = nil;
}

- (void)hk_changed {
    if (_visible) tell_presenter(self, 1);
}

- (NSRect)contentRectForFrameRect:(NSRect)frame { return frame; }
- (NSRect)frameRectForContentRect:(NSRect)content { return content; }
+ (NSRect)contentRectForFrameRect:(NSRect)frame styleMask:(NSWindowStyleMask)style { return frame; }
+ (NSRect)frameRectForContentRect:(NSRect)content styleMask:(NSWindowStyleMask)style { return content; }
- (NSRect)contentLayoutRect { return (NSRect){NSZeroPoint, _frame.size}; }
/* Normal windows stay on the screen (AppKit keeps a title bar reachable; here the screen is all there is to see). */
- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen *)screen {
    if (_level != 0 || _parentWindow) return frame;
    NSSize s = hk_presenter()->screen_size;
    if (frame.size.width >= s.width) frame.origin.x = 0;
    else frame.origin.x = MIN(MAX(frame.origin.x, 0), s.width - frame.size.width);
    if (frame.size.height >= s.height) frame.origin.y = s.height - frame.size.height;
    else frame.origin.y = MIN(MAX(frame.origin.y, 0), s.height - frame.size.height);
    return frame;
}

- (void)setFrame:(NSRect)frame display:(BOOL)display animate:(BOOL)animate { [self setFrame:frame display:display]; }
- (void)setFrame:(NSRect)frame display:(BOOL)display {
    frame = [self constrainFrameRect:frame toScreen:nil];
    BOOL moved = !CGPointEqualToPoint(frame.origin, _frame.origin), resized = !CGSizeEqualToSize(frame.size, _frame.size);
    if (!moved && !resized) return;
    _frame = frame;
    CALayer *root = _root;
    no_actions(^{
        root.frame = (CGRect){CGPointZero, frame.size};
    });
    if (resized) {
        [_frameView setFrameSize:frame.size];
        [_contentView setFrameSize:frame.size];
    }
    if (moved) [self hk_post:@"NSWindowDidMoveNotification"];
    if (resized) [self hk_post:@"NSWindowDidResizeNotification"];
    [self hk_changed];
}
- (void)setFrameOrigin:(NSPoint)origin { [self setFrame:(NSRect){origin, _frame.size} display:YES]; }
- (void)setFrameTopLeftPoint:(NSPoint)p { [self setFrameOrigin:NSMakePoint(p.x, p.y - _frame.size.height)]; }
- (void)setContentSize:(NSSize)size { [self setFrame:(NSRect){_frame.origin, size} display:YES]; }
- (void)center {
    NSRect s = [NSScreen mainScreen].visibleFrame;
    [self setFrameOrigin:NSMakePoint(NSMidX(s) - _frame.size.width / 2, NSMidY(s) - _frame.size.height / 2)];
}

/* ---- ordering ---- */
/* AppKit's stacking: windows of a higher level above those of a lower one, and a child window just above its parent
   (Chromium's bubbles, such as "Restore pages?", are child windows: they stayed behind the browser window when it
   came to the front). Order is otherwise kept. */
static NSWindow *top_parent(NSWindow *w) {
    for (int n = 0; w.parentWindow && n < 16; n++) w = w.parentWindow;
    return w;
}
static void add_family(NSMutableArray *out, NSWindow *w, NSArray *order, int depth) {
    [out addObject:w];
    if (depth > 16) return;
    for (NSWindow *c in order)
        if (c.parentWindow == w && ![out containsObject:c]) add_family(out, c, order, depth + 1);
}
static void restack(void) {
    NSArray *order = [g_windows copy];
    NSMutableArray *tops = [NSMutableArray array];
    for (NSWindow *w in order) {
        NSWindow *t = top_parent(w);
        if (![tops containsObject:t]) [tops addObject:t];
    }
    NSArray *byLevel = [tops sortedArrayWithOptions:NSSortStable usingComparator:^NSComparisonResult(NSWindow *a, NSWindow *b) {
        return a.level < b.level ? NSOrderedAscending : a.level > b.level ? NSOrderedDescending : NSOrderedSame;
    }];
    NSMutableArray *out = [NSMutableArray array];
    for (NSWindow *t in byLevel) add_family(out, t, order, 0);
    for (NSWindow *w in order) /* any left out (a parent cycle) keep their place at the end */
        if (![out containsObject:w]) [out addObject:w];
    [g_windows setArray:out];
}
- (void)hk_show {
    BOOL was = _visible;
    [g_windows removeObjectIdenticalTo:self];
    [g_windows addObject:self];
    restack();
    _visible = YES;
    tell_presenter(self, 1);
    if (!was) [self hk_post:@"NSWindowDidChangeOcclusionStateNotification"];
}
/* As in AppKit, every way of showing or hiding a window goes through orderWindow:relativeTo:, which apps override
   (Chromium learns there that its window is on screen and starts drawing). */
- (void)orderFront:(id)sender { [self orderWindow:NSWindowAbove relativeTo:0]; }
- (void)orderFrontRegardless { [self orderWindow:NSWindowAbove relativeTo:0]; }
- (void)orderBack:(id)sender { [self orderWindow:NSWindowBelow relativeTo:0]; }
- (void)orderOut:(id)sender { [self orderWindow:NSWindowOut relativeTo:0]; }
- (void)orderWindow:(NSWindowOrderingMode)place relativeTo:(NSInteger)other {
    if (place == NSWindowOut) {
        [self hk_hide];
        return;
    }
    [self hk_show];
    if (place == NSWindowBelow) {
        [g_windows removeObjectIdenticalTo:self];
        NSWindow *rel = other ? [NSApp windowWithWindowNumber:other] : nil;
        NSUInteger at = rel ? [g_windows indexOfObjectIdenticalTo:rel] : 0;
        [g_windows insertObject:self atIndex:at == NSNotFound ? 0 : at];
    }
}
- (void)hk_hide {
    if (!_visible) return;
    _visible = NO;
    if (g_key == self) [self resignKeyWindow];
    if (g_main == self) [self resignMainWindow];
    tell_presenter(self, 0);
    [self hk_post:@"NSWindowDidChangeOcclusionStateNotification"];
}
- (void)makeKeyAndOrderFront:(id)sender {
    [self orderFront:sender];
    [self makeKeyWindow];
    if ([self canBecomeMainWindow]) [self makeMainWindow];
}
- (BOOL)canBecomeKeyWindow { return (_styleMask & NSWindowStyleMaskTitled) != 0; }
- (BOOL)canBecomeMainWindow { return (_styleMask & NSWindowStyleMaskTitled) != 0; }
- (void)makeKeyWindow {
    if (g_key == self) return;
    [g_key resignKeyWindow];
    g_key = self;
    [self becomeKeyWindow];
}
- (void)makeMainWindow {
    if (g_main == self) return;
    [g_main resignMainWindow];
    g_main = self;
    [self becomeMainWindow];
}
- (void)becomeKeyWindow { [self hk_post:@"NSWindowDidBecomeKeyNotification"]; }
- (void)resignKeyWindow {
    if (g_key == self) g_key = nil;
    [self hk_post:@"NSWindowDidResignKeyNotification"];
}
- (void)becomeMainWindow { [self hk_post:@"NSWindowDidBecomeMainNotification"]; }
- (void)resignMainWindow {
    if (g_main == self) g_main = nil;
    [self hk_post:@"NSWindowDidResignMainNotification"];
}

- (void)performClose:(id)sender {
    if ([_delegate respondsToSelector:@selector(windowShouldClose:)] &&
        !((BOOL(*)(id, SEL, id))objc_msgSend)(_delegate, @selector(windowShouldClose:), self))
        return;
    [self close];
}
- (void)close {
    [self hk_post:@"NSWindowWillCloseNotification"];
    [self orderOut:nil];
    for (NSWindow *c in [_children copy]) [self removeChildWindow:c];
    [_parentWindow removeChildWindow:self];
    [g_windows removeObjectIdenticalTo:self];
}

- (void)addChildWindow:(NSWindow *)child ordered:(NSWindowOrderingMode)place {
    if (!child || [_children containsObject:child]) return;
    [_children addObject:child];
    child.parentWindow = self;
    if (_visible) [child orderWindow:NSWindowAbove relativeTo:_number];
}
- (void)removeChildWindow:(NSWindow *)child {
    [_children removeObjectIdenticalTo:child];
    if (child.parentWindow == self) child.parentWindow = nil;
}

/* ---- first responder and events ---- */
- (BOOL)makeFirstResponder:(NSResponder *)responder {
    if (responder == _firstResponder) return YES;
    if (_firstResponder && ![_firstResponder resignFirstResponder]) return NO;
    if (responder && responder != self && ![responder becomeFirstResponder]) {
        _firstResponder = nil;
        return NO;
    }
    _firstResponder = responder == self ? nil : responder;
    return YES;
}

static BOOL is_mouse(NSEventType t) {
    switch (t) {
    case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp:
    case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
    case NSEventTypeMouseMoved: case NSEventTypeScrollWheel: case NSEventTypeMagnify:
        return YES;
    default: return NO;
    }
}

- (NSView *)hk_viewAt:(NSPoint)p {
    return [_contentView hitTest:p] ?: _contentView;
}

/* Tracking areas: whoever owns an area of the view under the mouse hears it move, enter and leave. */
- (void)hk_track:(NSEvent *)event {
    NSView *now = [self hk_viewAt:event.locationInWindow];
    NSMutableArray *owners = [NSMutableArray array];
    for (NSView *v = now; v; v = v->_superview)
        for (NSTrackingArea *a in v->_trackingAreas) {
            id owner = [(id)a valueForKey:@"owner"];
            if (owner) [owners addObject:@[ a, owner ]];
        }
    if (_hoverView != now) {
        for (NSView *v = _hoverView; v; v = v->_superview)
            if (![now isDescendantOf:v])
                for (NSTrackingArea *a in [v->_trackingAreas copy]) {
                    id owner = [(id)a valueForKey:@"owner"];
                    if ([owner respondsToSelector:@selector(mouseExited:)]) [owner mouseExited:event];
                }
        for (NSView *v = now; v; v = v->_superview)
            if (!_hoverView || ![_hoverView isDescendantOf:v])
                for (NSTrackingArea *a in [v->_trackingAreas copy]) {
                    id owner = [(id)a valueForKey:@"owner"];
                    if ([owner respondsToSelector:@selector(mouseEntered:)]) [owner mouseEntered:event];
                }
        _hoverView = now;
    }
    BOOL sent = NO;
    for (NSArray *pair in owners) {
        NSTrackingArea *a = pair[0];
        NSUInteger opts = [[(id)a valueForKey:@"options"] unsignedIntegerValue];
        if ((opts & NSTrackingMouseMoved) && [pair[1] respondsToSelector:@selector(mouseMoved:)]) {
            [pair[1] mouseMoved:event];
            sent = YES;
        }
    }
    if (!sent && _acceptsMouseMovedEvents) [now mouseMoved:event];
}

- (void)sendEvent:(NSEvent *)event {
    NSEventType t = event.type;
    if (is_mouse(t)) {
        NSPoint p = event.locationInWindow;
        switch (t) {
        case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown: {
            NSView *v = [self hk_viewAt:p];
            if (getenv("HK_LOG_EVENTS"))
                hk_log("mouse down (type %lu, %ld clicks) at %.0f,%.0f in %s to %s", (unsigned long)t, (long)event.clickCount, p.x, p.y,
                       object_getClassName(self), object_getClassName(v));
            _mouseDownView = v;
            if (g_key != self && [self canBecomeKeyWindow]) [self makeKeyAndOrderFront:nil];
            if ([v acceptsFirstResponder] && _firstResponder != v) [self makeFirstResponder:v];
            if (t == NSEventTypeLeftMouseDown) [v mouseDown:event];
            else if (t == NSEventTypeRightMouseDown) [v rightMouseDown:event];
            else [v otherMouseDown:event];
            break;
        }
        case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp: {
            NSView *v = _mouseDownView ?: [self hk_viewAt:p];
            if (getenv("HK_LOG_EVENTS")) hk_log("mouse up at %.0f,%.0f to %s", p.x, p.y, object_getClassName(v));
            _mouseDownView = nil;
            if (t == NSEventTypeLeftMouseUp) [v mouseUp:event];
            else if (t == NSEventTypeRightMouseUp) [v rightMouseUp:event];
            else [v otherMouseUp:event];
            break;
        }
        case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged: {
            NSView *v = _mouseDownView ?: [self hk_viewAt:p];
            if (t == NSEventTypeLeftMouseDragged) [v mouseDragged:event];
            else if (t == NSEventTypeRightMouseDragged) [v rightMouseDragged:event];
            else [v otherMouseDragged:event];
            break;
        }
        case NSEventTypeMouseMoved: [self hk_track:event]; break;
        case NSEventTypeScrollWheel: {
            NSView *v = [self hk_viewAt:p];
            if (getenv("HK_LOG_EVENTS")) hk_log("scroll %.0f,%.0f phase %lu to %s", event.scrollingDeltaX, event.scrollingDeltaY, (unsigned long)event.phase, object_getClassName(v));
            [v scrollWheel:event];
            break;
        }
        case NSEventTypeMagnify: [[self hk_viewAt:p] magnifyWithEvent:event]; break;
        default: break;
        }
        return;
    }
    NSResponder *r = self.firstResponder;
    switch (t) {
    case NSEventTypeKeyDown:
        if ((event.modifierFlags & NSEventModifierFlagCommand) && [_contentView performKeyEquivalent:event]) break;
        [r keyDown:event];
        break;
    case NSEventTypeKeyUp: [r keyUp:event]; break;
    case NSEventTypeFlagsChanged: [r flagsChanged:event]; break;
    default: break;
    }
}

- (void)keyDown:(NSEvent *)event {}
- (void)mouseDown:(NSEvent *)event {}

- (NSPoint)convertPointToScreen:(NSPoint)p { return NSMakePoint(p.x + _frame.origin.x, p.y + _frame.origin.y); }
- (NSPoint)convertPointFromScreen:(NSPoint)p { return NSMakePoint(p.x - _frame.origin.x, p.y - _frame.origin.y); }
- (NSRect)convertRectToScreen:(NSRect)r { return (NSRect){[self convertPointToScreen:r.origin], r.size}; }
- (NSRect)convertRectFromScreen:(NSRect)r { return (NSRect){[self convertPointFromScreen:r.origin], r.size}; }
- (NSPoint)convertBaseToScreen:(NSPoint)p { return [self convertPointToScreen:p]; }
- (NSRect)convertRectToBacking:(NSRect)r {
    CGFloat s = self.backingScaleFactor;
    return NSMakeRect(r.origin.x * s, r.origin.y * s, r.size.width * s, r.size.height * s);
}
- (NSRect)convertRectFromBacking:(NSRect)r {
    CGFloat s = self.backingScaleFactor;
    return NSMakeRect(r.origin.x / s, r.origin.y / s, r.size.width / s, r.size.height / s);
}
- (NSPoint)mouseLocationOutsideOfEventStream { return [self convertPointFromScreen:g_mouse]; }

- (void)invalidateShadow {}
- (void)displayIfNeeded { [_contentView displayIfNeeded]; }
- (void)display { [_contentView display]; }
- (void)update {}
- (BOOL)isOnActiveSpace { return YES; }
- (BOOL)isZoomed { return NO; }
- (BOOL)isMiniaturized { return NO; }
- (BOOL)inLiveResize { return NO; }
- (BOOL)isSheet { return NO; }
- (BOOL)isRestorable { return NO; }
- (BOOL)isMovable { return YES; }
- (BOOL)isExcludedFromWindowsMenu { return YES; }
- (BOOL)hasKeyAppearance { return g_key == self; }
- (BOOL)hasMainAppearance { return g_main == self; }
- (BOOL)worksWhenModal { return NO; }
- (NSWindow *)attachedSheet { return nil; }
- (NSArray *)sheets { return @[]; }
- (id)toolbar { return nil; }
- (id)standardWindowButton:(NSUInteger)b { return nil; }
- (CGFloat)titlebarHeight { return 0; }
- (void)toggleFullScreen:(id)sender {}
- (void)miniaturize:(id)sender {}
- (void)deminiaturize:(id)sender {}
- (void)zoom:(id)sender {}
- (void)setTitleWithRepresentedFilename:(NSString *)f { self.title = f.lastPathComponent; }
- (void)disableCursorRects {}
- (void)enableCursorRects {}
- (void)invalidateCursorRectsForView:(NSView *)view {}
- (void)setIsVisible:(BOOL)v { if (v) [self orderFront:nil]; else [self orderOut:nil]; }
- (NSInteger)orderedIndex { return [g_windows indexOfObjectIdenticalTo:self]; }
- (NSString *)description {
    return [NSString stringWithFormat:@"<%@ %p #%ld %@>", NSStringFromClass([self class]), self, (long)_number, NSStringFromRect(_frame)];
}
@end

NSArray<NSWindow *> *hk_visible_windows(void) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSWindow *w in g_windows)
        if (w.isVisible) [out addObject:w];
    return out;
}

/* ================================================================ events */

@interface NSEvent () {
  @public
    NSEventType _type;
    NSEventModifierFlags _flags;
    NSTimeInterval _time;
    __weak NSWindow *_window;
    NSInteger _windowNumber;
    NSPoint _location;
    NSInteger _clicks, _button, _number;
    float _pressure;
    CGFloat _dx, _dy;
    BOOL _precise;
    NSEventPhase _phase, _momentum;
    unsigned short _keyCode;
    NSString *_chars, *_unmod;
    BOOL _repeat;
    short _subtype;
    NSInteger _data1, _data2;
}
@end

static NSMutableArray *g_monitors; /* @[mask, handler] */
static NSInteger g_event_number;

@implementation NSEvent
+ (void)load { hk_add_fallback(self); }
- (NSEventType)type { return _type; }
- (NSEventModifierFlags)modifierFlags { return _flags; }
- (NSTimeInterval)timestamp { return _time; }
- (NSWindow *)window { return _window; }
- (NSInteger)windowNumber { return _windowNumber; }
- (NSPoint)locationInWindow { return _location; }
- (NSInteger)clickCount { return _clicks; }
- (NSInteger)buttonNumber { return _button; }
- (NSInteger)eventNumber { return _number; }
- (float)pressure { return _pressure; }
- (CGFloat)deltaX { return _dx; }
- (CGFloat)deltaY { return _dy; }
- (CGFloat)deltaZ { return 0; }
- (CGFloat)scrollingDeltaX { return _dx; }
- (CGFloat)scrollingDeltaY { return _dy; }
- (BOOL)hasPreciseScrollingDeltas { return _precise; }
- (NSEventPhase)phase { return _phase; }
- (NSEventPhase)momentumPhase { return _momentum; }
- (BOOL)isDirectionInvertedFromDevice { return NO; }
- (unsigned short)keyCode { return _keyCode; }
- (NSString *)characters { return _chars; }
- (NSString *)charactersIgnoringModifiers { return _unmod; }
- (NSString *)charactersByApplyingModifiers:(NSEventModifierFlags)m { return _unmod; }
- (BOOL)isARepeat { return _repeat; }
- (short)subtype { return _subtype; }
- (NSInteger)data1 { return _data1; }
- (NSInteger)data2 { return _data2; }
- (CGFloat)magnification { return 0; }
- (NSTrackingArea *)trackingArea { return nil; }
- (void *)eventRef { return NULL; }
- (void *)CGEvent { return (__bridge void *)self; } /* the CGEvent functions read their fields from the NSEvent (HKMacC.m) */
- (NSUInteger)associatedEventsMask { return 0; }
- (NSInteger)stage { return 0; }
- (NSUInteger)deviceID { return 0; }
- (NSPoint)tilt { return NSZeroPoint; }
- (BOOL)isSwipeTrackingFromScrollEventsEnabled { return NO; }
- (id)copyWithZone:(NSZone *)zone {
    NSEvent *e = [NSEvent new];
    e->_type = _type, e->_flags = _flags, e->_time = _time, e->_window = _window, e->_windowNumber = _windowNumber;
    e->_location = _location, e->_clicks = _clicks, e->_button = _button, e->_number = _number, e->_pressure = _pressure;
    e->_dx = _dx, e->_dy = _dy, e->_precise = _precise, e->_phase = _phase, e->_momentum = _momentum;
    e->_keyCode = _keyCode, e->_chars = _chars, e->_unmod = _unmod, e->_repeat = _repeat, e->_subtype = _subtype;
    e->_data1 = _data1, e->_data2 = _data2;
    return e;
}

+ (NSEvent *)hk_eventWithType:(NSEventType)type window:(NSWindow *)window location:(NSPoint)location
                    modifiers:(NSEventModifierFlags)flags clickCount:(NSInteger)clicks button:(NSInteger)button {
    NSEvent *e = [NSEvent new];
    e->_type = type, e->_window = window, e->_windowNumber = window.windowNumber, e->_location = location;
    e->_flags = flags, e->_clicks = clicks, e->_button = button, e->_time = now_seconds();
    /* AppKit gives a press's down, drags and up one event number, and programs rely on it (cocos2d-x takes it as the
       touch's id: an up numbered apart from its down ends no touch, and the button stays pressed). */
    static NSInteger press;
    switch (type) {
    case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown:
        e->_number = press = ++g_event_number;
        break;
    case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
    case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
        e->_number = press ? press : ++g_event_number;
        break;
    default:
        e->_number = ++g_event_number;
    }
    e->_pressure = (type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseDragged) ? 1 : 0;
    return e;
}
- (NSEvent *)hk_withDeltaX:(CGFloat)dx deltaY:(CGFloat)dy precise:(BOOL)precise phase:(NSEventPhase)phase {
    _dx = dx, _dy = dy, _precise = precise, _phase = phase;
    return self;
}
- (NSEvent *)hk_withKeyCode:(unsigned short)code characters:(NSString *)chars unmodified:(NSString *)unmod repeat:(BOOL)repeat {
    _keyCode = code, _chars = [chars copy], _unmod = [unmod copy], _repeat = repeat;
    return self;
}

+ (NSEvent *)otherEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                      timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context subtype:(short)subtype
                          data1:(NSInteger)d1 data2:(NSInteger)d2 {
    NSEvent *e = [self hk_eventWithType:type window:[NSApp windowWithWindowNumber:wn] location:location modifiers:flags clickCount:0 button:0];
    e->_time = time, e->_subtype = subtype, e->_data1 = d1, e->_data2 = d2, e->_windowNumber = wn;
    return e;
}
+ (NSEvent *)mouseEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                      timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context eventNumber:(NSInteger)en
                     clickCount:(NSInteger)clicks pressure:(float)pressure {
    NSEvent *e = [self hk_eventWithType:type window:[NSApp windowWithWindowNumber:wn] location:location modifiers:flags clickCount:clicks button:0];
    e->_time = time, e->_number = en, e->_pressure = pressure, e->_windowNumber = wn;
    return e;
}
+ (NSEvent *)keyEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                    timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context characters:(NSString *)chars
  charactersIgnoringModifiers:(NSString *)unmod isARepeat:(BOOL)repeat keyCode:(unsigned short)code {
    NSEvent *e = [self hk_eventWithType:type window:[NSApp windowWithWindowNumber:wn] location:location modifiers:flags clickCount:0 button:0];
    e->_time = time, e->_windowNumber = wn;
    return [e hk_withKeyCode:code characters:chars unmodified:unmod repeat:repeat];
}
+ (NSEvent *)enterExitEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                          timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context eventNumber:(NSInteger)en
                     trackingNumber:(NSInteger)tn userData:(void *)data {
    return [self hk_eventWithType:type window:[NSApp windowWithWindowNumber:wn] location:location modifiers:flags clickCount:0 button:0];
}
+ (NSEvent *)eventWithCGEvent:(void *)event { return nil; }

+ (NSPoint)mouseLocation { return g_mouse; }
+ (NSUInteger)pressedMouseButtons { return g_buttons; }
+ (NSEventModifierFlags)modifierFlags { return g_modifiers; }
+ (NSTimeInterval)doubleClickInterval { return 0.5; }
+ (NSTimeInterval)keyRepeatDelay { return 0.5; }
+ (NSTimeInterval)keyRepeatInterval { return 0.083; }
+ (BOOL)isMouseCoalescingEnabled { return YES; }
+ (void)setMouseCoalescingEnabled:(BOOL)flag {}
+ (BOOL)isSwipeTrackingFromScrollEventsEnabled { return NO; }
+ (void)startPeriodicEventsAfterDelay:(NSTimeInterval)delay withPeriod:(NSTimeInterval)period {}
+ (void)stopPeriodicEvents {}

+ (id)addLocalMonitorForEventsMatchingMask:(NSEventMask)mask handler:(NSEvent * (^)(NSEvent *))handler {
    if (!g_monitors) g_monitors = [NSMutableArray new];
    NSArray *m = @[ @(mask), [handler copy] ];
    [g_monitors addObject:m];
    return m;
}
+ (id)addGlobalMonitorForEventsMatchingMask:(NSEventMask)mask handler:(void (^)(NSEvent *))handler { return [NSObject new]; }
+ (void)removeMonitor:(id)monitor { [g_monitors removeObjectIdenticalTo:monitor]; }

+ (NSEvent *)hk_runLocalMonitors:(NSEvent *)event {
    for (NSArray *m in [g_monitors copy]) {
        if (!event) break;
        if (!([m[0] unsignedLongLongValue] & NSEventMaskFromType(event.type))) continue;
        NSEvent * (^h)(NSEvent *) = m[1];
        event = h(event);
    }
    return event;
}
- (NSString *)description {
    return [NSString stringWithFormat:@"<NSEvent type=%lu loc=%@ win=%ld>", (unsigned long)_type, NSStringFromPoint(_location), (long)_windowNumber];
}
@end

/* ================================================================ the application */

NSApplication *NSApp;

static NSArray<NSString *> *app_notifications(void) {
    return @[ @"NSApplicationWillFinishLaunchingNotification", @"NSApplicationDidFinishLaunchingNotification",
              @"NSApplicationWillBecomeActiveNotification", @"NSApplicationDidBecomeActiveNotification",
              @"NSApplicationWillResignActiveNotification", @"NSApplicationDidResignActiveNotification",
              @"NSApplicationWillTerminateNotification", @"NSApplicationDidChangeScreenParametersNotification",
              @"NSApplicationWillHideNotification", @"NSApplicationDidHideNotification",
              @"NSApplicationWillUnhideNotification", @"NSApplicationDidUnhideNotification",
              @"NSApplicationWillUpdateNotification", @"NSApplicationDidUpdateNotification" ];
}

@interface NSApplication () {
    NSMutableArray<NSEvent *> *_queue;
    pthread_mutex_t _qlock;
    CFRunLoopSourceRef _wake;
    CFRunLoopRef _loop;
    BOOL _running, _active, _launched, _stopRequested, _terminating, _modalStopped;
    NSModalResponse _modalCode;
    NSMutableArray<NSWindow *> *_modal; /* windows of the modal sessions, innermost last */
    NSEvent *_current;
    NSApplicationActivationPolicy _policy;
}
@end

static void wake_perform(void *info) {}

@implementation NSApplication
@synthesize delegate = _delegate, mainMenu = _mainMenu, windowsMenu = _windowsMenu, servicesMenu = _servicesMenu,
            helpMenu = _helpMenu, appearance = _appearance, presentationOptions = _presentationOptions,
            applicationIconImage = _applicationIconImage;

+ (void)load { hk_add_fallback(self); }

+ (instancetype)sharedApplication {
    @synchronized([NSApplication class]) {
        if (!NSApp) {
            NSApp = (NSApplication *)[[self alloc] init];
        }
    }
    return NSApp;
}

- (instancetype)init {
    if ((self = [super init])) {
        if (!NSApp) NSApp = self;
        _queue = [NSMutableArray new];
        pthread_mutex_init(&_qlock, NULL);
        CFRunLoopSourceContext ctx = {0};
        ctx.perform = wake_perform;
        _wake = CFRunLoopSourceCreate(NULL, 0, &ctx);
        _loop = CFRunLoopGetCurrent();
        CFRunLoopAddSource(_loop, _wake, kCFRunLoopCommonModes);
        CFRunLoopAddSource(_loop, _wake, (__bridge CFStringRef) @"NSEventTrackingRunLoopMode");
        CFRunLoopAddSource(_loop, _wake, (__bridge CFStringRef) @"NSModalPanelRunLoopMode");
        /* As AppKit does: the common modes include its tracking and modal modes, so what an app (Chromium's message
           pump) and Core Animation's commits put in the common modes also run while a menu is tracked. */
        CFRunLoopAddCommonMode(_loop, (__bridge CFStringRef) @"NSEventTrackingRunLoopMode");
        CFRunLoopAddCommonMode(_loop, (__bridge CFStringRef) @"NSModalPanelRunLoopMode");
    }
    return self;
}

- (void)setDelegate:(id)delegate {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    for (NSString *n in app_notifications()) {
        if (_delegate) [nc removeObserver:_delegate name:n object:self];
        NSString *s = [n substringFromIndex:2];
        s = [s substringToIndex:s.length - 12];
        s = [[[s substringToIndex:1] lowercaseString] stringByAppendingString:[s substringFromIndex:1]];
        SEL sel = NSSelectorFromString([s stringByAppendingString:@":"]);
        if (delegate && [delegate respondsToSelector:sel]) [nc addObserver:delegate selector:sel name:n object:self];
    }
    _delegate = delegate;
}

- (void)hk_post:(NSString *)name {
    [[NSNotificationCenter defaultCenter] postNotificationName:name object:self];
}

- (NSArray<NSWindow *> *)windows { return [g_windows copy] ?: @[]; }
- (NSArray<NSWindow *> *)orderedWindows { return [[g_windows reverseObjectEnumerator] allObjects]; }
- (NSWindow *)keyWindow { return g_key; }
- (NSWindow *)mainWindow { return g_main; }
- (NSWindow *)modalWindow { return _modal.lastObject; }
- (NSEvent *)currentEvent { return _current; }
- (BOOL)isActive { return _active; }
- (BOOL)isRunning { return _running; }
- (BOOL)isHidden { return NO; }
- (NSAppearance *)effectiveAppearance {
    return _appearance ?: cls_msg([NSAppearance class], @selector(currentDrawingAppearance)) ?: nil;
}
- (NSWindow *)windowWithWindowNumber:(NSInteger)number {
    for (NSWindow *w in g_windows)
        if (w.windowNumber == number) return w;
    return nil;
}
- (void)enumerateWindowsWithOptions:(NSUInteger)options usingBlock:(void (^)(NSWindow *, BOOL *))block {
    BOOL stop = NO;
    for (NSWindow *w in [self orderedWindows]) {
        block(w, &stop);
        if (stop) break;
    }
}
- (void)updateWindows {}
- (void)setWindowsNeedUpdate:(BOOL)flag {}

- (void)activateIgnoringOtherApps:(BOOL)flag { [self hk_activate]; }
- (void)activate { [self hk_activate]; }
- (void)hk_activate {
    if (_active) return;
    [self hk_post:@"NSApplicationWillBecomeActiveNotification"];
    _active = YES;
    [self hk_post:@"NSApplicationDidBecomeActiveNotification"];
}
- (void)deactivate {
    if (!_active) return;
    [self hk_post:@"NSApplicationWillResignActiveNotification"];
    _active = NO;
    [self hk_post:@"NSApplicationDidResignActiveNotification"];
}
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)policy { _policy = policy; return YES; }
- (NSApplicationActivationPolicy)activationPolicy { return _policy; }
- (void)hide:(id)sender {}
- (void)unhide:(id)sender {}
- (void)unhideWithoutActivation {}
- (void)hideOtherApplications:(id)sender {}
- (NSInteger)requestUserAttention:(NSInteger)type { return 0; }
- (void)cancelUserAttentionRequest:(NSInteger)request {}
- (void)registerServicesMenuSendTypes:(NSArray *)send returnTypes:(NSArray *)ret {}
- (void)setServicesProvider:(id)provider {}
- (void)registerForRemoteNotifications {}
- (void)disableRelaunchOnLogin {}
- (void)enableRelaunchOnLogin {}
- (BOOL)isFullKeyboardAccessEnabled { return NO; }
- (NSUInteger)userInterfaceLayoutDirection { return 0; }
- (BOOL)isAutomaticCustomizeTouchBarMenuItemEnabled { return NO; }

- (void)finishLaunching {
    if (_launched) return;
    _launched = YES;
    [self hk_post:@"NSApplicationWillFinishLaunchingNotification"];
    [self hk_activate];
    [self hk_post:@"NSApplicationDidFinishLaunchingNotification"];
}

- (void)run {
    /* A stop: during finishLaunching (GLFW's delegate stops the run it started from applicationDidFinishLaunching:)
       ends this run, as in AppKit: the request is cleared before launching, not after. */
    _running = YES;
    _stopRequested = NO;
    [self finishLaunching];
    while (_running && !_stopRequested) {
        @autoreleasepool {
            NSEvent *e = [self nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantFuture] inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e) [self sendEvent:e];
        }
    }
    _running = NO;
    /* The stop ended this run only: an app that goes on taking events itself (GLFW, SDL poll after their launch
       run) gets them, and the run loop runs, unless the app is quitting. */
    if (!_terminating) _stopRequested = NO;
}

- (void)stop:(id)sender {
    _stopRequested = YES;
    CFRunLoopSourceSignal(_wake);
    CFRunLoopWakeUp(_loop);
}

- (void)terminate:(id)sender {
    NSApplicationTerminateReply reply = NSTerminateNow;
    if ([_delegate respondsToSelector:@selector(applicationShouldTerminate:)])
        reply = ((NSApplicationTerminateReply(*)(id, SEL, id))objc_msgSend)(_delegate, @selector(applicationShouldTerminate:), self);
    if (reply != NSTerminateNow) return;
    [self hk_finishTerminate];
}

- (void)replyToApplicationShouldTerminate:(BOOL)reply {
    if (reply) [self hk_finishTerminate];
}

- (void)hk_finishTerminate {
    _terminating = YES;
    [self hk_post:@"NSApplicationWillTerminateNotification"];
    for (NSWindow *w in [g_windows copy]) [w orderOut:nil];
    hk_log("the application terminated");
    [self stop:nil];
}

- (NSEvent *)hk_takeMatching:(NSEventMask)mask dequeue:(BOOL)dequeue {
    pthread_mutex_lock(&_qlock);
    NSEvent *found = nil;
    for (NSUInteger i = 0; i < _queue.count; i++) {
        NSEvent *e = _queue[i];
        if (mask & NSEventMaskFromType(e.type)) {
            found = e;
            if (dequeue) [_queue removeObjectAtIndex:i];
            break;
        }
    }
    pthread_mutex_unlock(&_qlock);
    return found;
}

- (NSEvent *)nextEventMatchingMask:(NSEventMask)mask untilDate:(NSDate *)date inMode:(NSRunLoopMode)mode dequeue:(BOOL)dequeue {
    if (!mode) mode = NSDefaultRunLoopMode;
    for (;;) {
        NSEvent *e = [self hk_takeMatching:mask dequeue:dequeue];
        if (e) {
            if (dequeue) _current = e;
            return e;
        }
        if (_stopRequested && !_running) return nil;
        NSTimeInterval left = date ? date.timeIntervalSinceNow : 0;
        if (left <= 0) {
            CFRunLoopRunInMode((__bridge CFStringRef)mode, 0, true);
            e = [self hk_takeMatching:mask dequeue:dequeue];
            if (e && dequeue) _current = e;
            return e;
        }
        CFRunLoopRunResult r = CFRunLoopRunInMode((__bridge CFStringRef)mode, MIN(left, 1e9), true);
        if (_stopRequested) {
            e = [self hk_takeMatching:mask dequeue:dequeue];
            if (e && dequeue) _current = e;
            return e;
        }
        if (r == kCFRunLoopRunFinished) usleep(1000); /* nothing in this mode yet: don't spin */
    }
}

- (void)discardEventsMatchingMask:(NSEventMask)mask beforeEvent:(NSEvent *)event {
    pthread_mutex_lock(&_qlock);
    NSIndexSet *drop = [_queue indexesOfObjectsPassingTest:^BOOL(NSEvent *e, NSUInteger i, BOOL *stop) {
        if (e == event) { *stop = YES; return NO; }
        return (mask & NSEventMaskFromType(e.type)) != 0;
    }];
    [_queue removeObjectsAtIndexes:drop];
    pthread_mutex_unlock(&_qlock);
}

- (void)postEvent:(NSEvent *)event atStart:(BOOL)atStart {
    if (!event) return;
    pthread_mutex_lock(&_qlock);
    if (atStart) [_queue insertObject:event atIndex:0];
    else [_queue addObject:event];
    pthread_mutex_unlock(&_qlock);
    CFRunLoopSourceSignal(_wake);
    CFRunLoopWakeUp(_loop);
}

- (void)sendEvent:(NSEvent *)event {
    NSEvent *saved = _current;
    _current = event;
    event = [NSEvent hk_runLocalMonitors:event];
    if (event) {
        NSEventType t = event.type;
        NSWindow *w = event.window;
        if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp || t == NSEventTypeFlagsChanged) {
            if (t == NSEventTypeKeyDown && (event.modifierFlags & NSEventModifierFlagCommand) && [(id)_mainMenu respondsToSelector:@selector(performKeyEquivalent:)] &&
                ((BOOL(*)(id, SEL, id))objc_msgSend)((id)_mainMenu, @selector(performKeyEquivalent:), event)) {
            } else {
                [(g_key ?: w) sendEvent:event];
            }
        } else if (w) {
            [w sendEvent:event];
        }
    }
    _current = saved;
}

- (BOOL)sendAction:(SEL)action to:(id)target from:(id)sender {
    id t = [self targetForAction:action to:target from:sender];
    if (!t) return NO;
    ((void (*)(id, SEL, id))objc_msgSend)(t, action, sender);
    return YES;
}

- (id)targetForAction:(SEL)action { return [self targetForAction:action to:nil from:nil]; }

- (id)targetForAction:(SEL)action to:(id)target from:(id)sender {
    if (!action) return nil;
    if (target) return [target respondsToSelector:action] ? target : nil;
    for (NSWindow *w in @[ g_key ?: (id)[NSNull null], g_main ?: (id)[NSNull null] ]) {
        if ((id)w == [NSNull null]) continue;
        for (NSResponder *r = w.firstResponder; r; r = r.nextResponder)
            if ([r respondsToSelector:action]) return r;
        if ([w respondsToSelector:action]) return w;
        if ([w.delegate respondsToSelector:action]) return w.delegate;
    }
    if ([self respondsToSelector:action]) return self;
    if ([_delegate respondsToSelector:action]) return _delegate;
    return nil;
}

/* Modal windows, as AppKit runs them: a session shows the window as key; runModalSession: handles what events are
   waiting (in the modal run-loop mode) and answers NSModalResponseContinue (-1002) until stopModal (-1000),
   stopModalWithCode: or abortModal (-1001) ends it; runModalForWindow: is a session run until then. Qt's dialogs
   (and NSAlert) run this way. */
- (void *)beginModalSessionForWindow:(NSWindow *)window {
    if (!window) return NULL;
    if (!_modal) _modal = [NSMutableArray array];
    [_modal addObject:window];
    _modalStopped = NO;
    [window makeKeyAndOrderFront:nil];
    return (__bridge void *)window;
}
- (NSModalResponse)runModalSession:(void *)session {
    for (int i = 0; i < 64 && !_modalStopped; i++) {
        @autoreleasepool {
            NSEvent *e = [self nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:@"NSModalPanelRunLoopMode" dequeue:YES];
            if (!e) break;
            [self sendEvent:e];
        }
    }
    if (!_modalStopped) return -1002; /* NSModalResponseContinue */
    _modalStopped = NO;
    return _modalCode;
}
- (void)endModalSession:(void *)session {
    NSWindow *w = (__bridge NSWindow *)session;
    if (w) [_modal removeObjectIdenticalTo:w];
}
- (NSModalResponse)runModalForWindow:(NSWindow *)window {
    void *session = [self beginModalSessionForWindow:window];
    if (!session) return -1001;
    NSModalResponse r;
    while ((r = [self runModalSession:session]) == -1002) {
        @autoreleasepool { /* wait for the next event (or a stop) without taking it */
            [self nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]
                                 inMode:@"NSModalPanelRunLoopMode" dequeue:NO];
        }
    }
    [self endModalSession:session];
    return r;
}
- (void)stopModal { [self stopModalWithCode:-1000]; }
- (void)stopModalWithCode:(NSModalResponse)code {
    _modalStopped = YES;
    _modalCode = code;
    CFRunLoopSourceSignal(_wake);
    CFRunLoopWakeUp(_loop);
}
- (void)abortModal { [self stopModalWithCode:-1001]; }
@end

/* The app's bundle (adopted by the loader), not the host's: what Henge's AppKit reads for the app. */
NSBundle *hk_app_bundle(void) { return (__bridge NSBundle *)tl_main_bundle() ?: [NSBundle mainBundle]; }

int NSApplicationMain(int argc, const char *argv[]) {
    @autoreleasepool {
        NSBundle *app = hk_app_bundle();
        Class principal = NSClassFromString(app.infoDictionary[@"NSPrincipalClass"] ?: @"NSApplication");
        if (!principal) principal = [NSApplication class];
        [principal sharedApplication];
        [NSApp run];
    }
    return 0;
}

/* ================================================================ input from the presenter */

static void post_on_queue(NSEvent *e) {
    if (!NSApp) return;
    [NSApp postEvent:e atStart:NO];
}

void hk_post_mouse(NSWindow *window, NSEventType type, CGPoint location, NSEventModifierFlags flags, NSInteger clicks) {
    @autoreleasepool {
        NSInteger button = (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp || type == NSEventTypeRightMouseDragged) ? 1 : 0;
        NSEvent *e = [NSEvent hk_eventWithType:type window:window location:location modifiers:flags clickCount:clicks button:button];
        g_mouse = [window convertPointToScreen:location];
        if (type == NSEventTypeLeftMouseDown) g_buttons |= 1;
        if (type == NSEventTypeLeftMouseUp) g_buttons &= ~1UL;
        if (type == NSEventTypeRightMouseDown) g_buttons |= 2;
        if (type == NSEventTypeRightMouseUp) g_buttons &= ~2UL;
        post_on_queue(e);
    }
}

void hk_post_scroll(NSWindow *window, CGPoint location, CGFloat dx, CGFloat dy, NSEventPhase phase) {
    @autoreleasepool {
        NSEvent *e = [NSEvent hk_eventWithType:NSEventTypeScrollWheel window:window location:location modifiers:g_modifiers clickCount:0 button:0];
        post_on_queue([e hk_withDeltaX:dx deltaY:dy precise:YES phase:phase]);
    }
}

void hk_post_key(NSWindow *window, NSEventType type, unsigned short keycode, NSString *chars, NSString *unmod, NSEventModifierFlags flags) {
    @autoreleasepool {
        g_modifiers = flags;
        NSEvent *e = [NSEvent hk_eventWithType:type window:window location:[window mouseLocationOutsideOfEventStream] modifiers:flags clickCount:0 button:0];
        post_on_queue([e hk_withKeyCode:keycode characters:chars ?: @"" unmodified:unmod ?: chars ?: @"" repeat:NO]);
    }
}

/* ================================================================ the screen */

@implementation NSScreen
+ (void)load { hk_add_fallback(self); }
+ (NSScreen *)mainScreen {
    static NSScreen *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [NSScreen new]; });
    return s;
}
+ (NSArray<NSScreen *> *)screens { return @[ [self mainScreen] ]; }
+ (NSScreen *)deepestScreen { return [self mainScreen]; }
+ (BOOL)screensHaveSeparateSpaces { return NO; }
- (NSRect)frame { return (NSRect){NSZeroPoint, hk_presenter()->screen_size}; }
- (NSRect)visibleFrame { return self.frame; }
- (CGFloat)backingScaleFactor { return hk_presenter()->scale; }
- (NSDictionary *)deviceDescription {
    NSSize size = self.frame.size, dpi = NSMakeSize(72 * self.backingScaleFactor, 72 * self.backingScaleFactor);
    return @{ @"NSScreenNumber" : @1,
              @"NSDeviceSize" : [NSValue valueWithBytes:&size objCType:@encode(CGSize)],
              @"NSDeviceResolution" : [NSValue valueWithBytes:&dpi objCType:@encode(CGSize)],
              @"NSDeviceIsScreen" : @"YES",
              @"NSDeviceBitsPerSample" : @8,
              @"NSDeviceColorSpaceName" : @"NSCalibratedRGBColorSpace" };
}
- (NSString *)localizedName { return @"Henge"; }
- (NSColorSpace *)colorSpace { return cls_msg([NSColorSpace class], @selector(sRGBColorSpace)); }
- (NSInteger)depth { return 0x208; }
- (NSInteger)maximumFramesPerSecond { return 60; }
- (CGFloat)maximumExtendedDynamicRangeColorComponentValue { return 1; }
- (CGFloat)maximumPotentialExtendedDynamicRangeColorComponentValue { return 1; }
- (CGFloat)maximumReferenceExtendedDynamicRangeColorComponentValue { return 0; }
- (NSTimeInterval)minimumRefreshInterval { return 1.0 / 60; }
- (NSTimeInterval)maximumRefreshInterval { return 1.0 / 60; }
- (NSTimeInterval)displayUpdateGranularity { return 0; }
- (NSEdgeInsets)safeAreaInsets { return NSEdgeInsetsZero; }
- (NSRect)convertRectToBacking:(NSRect)r {
    CGFloat s = self.backingScaleFactor;
    return NSMakeRect(r.origin.x * s, r.origin.y * s, r.size.width * s, r.size.height * s);
}
- (NSRect)convertRectFromBacking:(NSRect)r {
    CGFloat s = self.backingScaleFactor;
    return NSMakeRect(r.origin.x / s, r.origin.y / s, r.size.width / s, r.size.height / s);
}
- (BOOL)canRepresentDisplayGamut:(NSInteger)gamut { return gamut <= 1; }
@end

@implementation NSPanel
- (BOOL)canBecomeMainWindow { return NO; }
@end
