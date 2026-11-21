/* Henge's AppKit, controls and the other view classes a Mac app uses or subclasses: cells, controls (buttons, text
   fields, image views, pop-up buttons, scrollers), effect views, the window frame classes, and the accessibility
   element classes. Controls keep their values in their cell, as AppKit's do, and draw themselves plainly with
   CoreGraphics and CoreText (arm64 values: NSTextAlignment left 0, center 1, right 2). */
#import "HKShim.h"
#import <CoreText/CoreText.h>
#import <objc/message.h>
#import <objc/runtime.h>

enum { HKAlignLeft = 0, HKAlignCenter = 1, HKAlignRight = 2, HKAlignNatural = 4 };

@interface NSColor (HKControls)
+ (NSColor *)selectedControlColor;
+ (NSColor *)placeholderTextColor;
@end

/* ================================================================ text */

static CFAttributedStringRef make_text(NSString *s, NSFont *font, NSColor *color) CF_RETURNS_RETAINED;
static CFAttributedStringRef make_text(NSString *s, NSFont *font, NSColor *color) {
    NSDictionary *attrs = @{
        (id)kCTFontAttributeName : font ?: [NSFont systemFontOfSize:0],
        (id)kCTForegroundColorAttributeName : (__bridge id)(color ?: [NSColor labelColor]).CGColor,
    };
    return CFAttributedStringCreate(NULL, (__bridge CFStringRef)(s ?: @""), (__bridge CFDictionaryRef)attrs);
}

static NSSize text_size(NSString *s, NSFont *font) {
    CFAttributedStringRef a = make_text(s, font, nil);
    CTLineRef line = CTLineCreateWithAttributedString(a);
    CGFloat asc = 0, desc = 0, lead = 0;
    double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
    CFRelease(line);
    CFRelease(a);
    return NSMakeSize(ceil(w), ceil(asc + desc + lead));
}

/* Draws s in rect (one line, centered vertically and truncated at the tail; or wrapped from the top), in the current
   context, flipped or not. */
static void draw_text(NSString *s, NSFont *font, NSColor *color, NSRect rect, NSInteger alignment, BOOL wraps) {
    NSGraphicsContext *g = [NSGraphicsContext currentContext];
    CGContextRef c = g.CGContext;
    if (!c || !s.length || NSIsEmptyRect(rect)) return;
    CGContextSaveGState(c);
    if (g.isFlipped) {
        /* Flip about the rect's own middle, so CoreText's y-up drawing lands in the same rect. */
        CGContextTranslateCTM(c, 0, NSMaxY(rect) + NSMinY(rect));
        CGContextScaleCTM(c, 1, -1);
    }
    CGContextSetTextMatrix(c, CGAffineTransformIdentity);
    CFAttributedStringRef a = make_text(s, font, color);
    if (wraps) {
        CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(a);
        CGPathRef path = CGPathCreateWithRect(rect, NULL);
        CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, NULL);
        CTFrameDraw(frame, c);
        CFRelease(frame);
        CGPathRelease(path);
        CFRelease(fs);
    } else {
        CTLineRef line = CTLineCreateWithAttributedString(a);
        CGFloat asc = 0, desc = 0, lead = 0;
        double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
        if (w > rect.size.width) {
            CFAttributedStringRef dots = make_text(@"…", font, color);
            CTLineRef token = CTLineCreateWithAttributedString(dots);
            CTLineRef cut = CTLineCreateTruncatedLine(line, rect.size.width, kCTLineTruncationEnd, token);
            CFRelease(token);
            CFRelease(dots);
            if (cut) {
                CFRelease(line);
                line = cut;
                w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
            }
        }
        CGFloat x = rect.origin.x;
        if (alignment == HKAlignCenter) x += (rect.size.width - w) / 2;
        else if (alignment == HKAlignRight) x += rect.size.width - w;
        CGContextSetTextPosition(c, x, NSMinY(rect) + (rect.size.height - (asc + desc)) / 2 + desc);
        CTLineDraw(line, c);
        CFRelease(line);
    }
    CFRelease(a);
    CGContextRestoreGState(c);
}

static void fill_rounded(NSRect r, CGFloat radius, NSColor *fill, NSColor *stroke) {
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    if (!c || NSIsEmptyRect(r)) return;
    r = NSInsetRect(r, 0.5, 0.5);
    radius = fmin(radius, fmin(r.size.width, r.size.height) / 2);
    CGPathRef p = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
    CGContextSaveGState(c);
    if (fill) {
        CGContextSetFillColorWithColor(c, fill.CGColor);
        CGContextAddPath(c, p);
        CGContextFillPath(c);
    }
    if (stroke) {
        CGContextSetStrokeColorWithColor(c, stroke.CGColor);
        CGContextSetLineWidth(c, 1);
        CGContextAddPath(c, p);
        CGContextStrokePath(c);
    }
    CGContextRestoreGState(c);
    CGPathRelease(p);
}

/* ================================================================ cells */

@interface NSCell ()
@property(nonatomic) NSInteger alignment, lineBreakMode, type, backgroundStyle, focusRingType, baseWritingDirection;
@property(nonatomic) NSInteger userInterfaceLayoutDirection;
@property(nonatomic) NSControlSize controlSize;
@property(nonatomic, getter=isEditable) BOOL editable;
@property(nonatomic, getter=isSelectable) BOOL selectable;
@property(nonatomic, getter=isBordered) BOOL bordered;
@property(nonatomic, getter=isBezeled) BOOL bezeled;
@property(nonatomic, getter=isHighlighted) BOOL highlighted;
@property(nonatomic, getter=isScrollable) BOOL scrollable;
@property(nonatomic, getter=isContinuous) BOOL continuous;
@property(nonatomic) BOOL wraps, truncatesLastVisibleLine, usesSingleLineMode, refusesFirstResponder, showsFirstResponder;
@property(nonatomic) BOOL allowsMixedState, sendsActionOnEndEditing;
@property(nonatomic, strong) NSFormatter *formatter;
@property(nonatomic, strong) id representedObject;
@property(nonatomic, copy) NSString *placeholderString;
@end

@implementation NSCell {
    id _objectValue;
    NSFont *_font;
    NSAttributedString *_attributed;
    NSInteger _sendActionOn;
}
@synthesize controlView = _controlView, image = _image, enabled = _enabled, state = _state, tag = _tag, target = _target,
            action = _action;

- (instancetype)initTextCell:(NSString *)string {
    if ((self = [super init])) {
        _objectValue = [string copy] ?: @"";
        _enabled = YES;
        _type = 1; /* NSTextCellType */
        _wraps = YES;
        _sendActionOn = 1 << NSEventTypeLeftMouseUp;
        _alignment = HKAlignNatural;
    }
    return self;
}
- (instancetype)initImageCell:(NSImage *)image {
    if ((self = [self initTextCell:@""])) {
        _image = image;
        _type = 2; /* NSImageCellType */
    }
    return self;
}
- (instancetype)init { return [self initTextCell:@""]; }
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (id)copyWithZone:(NSZone *)zone {
    NSCell *c = [[[self class] alloc] initTextCell:@""];
    c->_objectValue = _objectValue;
    c->_font = _font;
    c->_attributed = _attributed;
    c.image = self.image;
    c.enabled = self.enabled;
    c.state = self.state;
    c.tag = self.tag;
    c.target = self.target;
    c.action = self.action;
    c.alignment = _alignment;
    c.controlSize = _controlSize;
    c.editable = _editable;
    c.bordered = _bordered;
    c.bezeled = _bezeled;
    return c;
}
- (id)objectValue { return _objectValue; }
- (void)setObjectValue:(id)value {
    _objectValue = [value conformsToProtocol:@protocol(NSCopying)] ? [value copy] : value;
    _attributed = nil;
}
- (NSString *)stringValue {
    if (_attributed) return _attributed.string;
    if ([_objectValue isKindOfClass:[NSString class]]) return _objectValue;
    if (_formatter && _objectValue) return [_formatter stringForObjectValue:_objectValue] ?: @"";
    return [_objectValue description] ?: @"";
}
- (void)setStringValue:(NSString *)s { self.objectValue = s ?: @""; }
- (NSString *)title { return self.stringValue; }
- (void)setTitle:(NSString *)title { self.stringValue = title; }
- (NSAttributedString *)attributedStringValue { return _attributed ?: [[NSAttributedString alloc] initWithString:self.stringValue]; }
- (void)setAttributedStringValue:(NSAttributedString *)s {
    _objectValue = s.string ?: @"";
    _attributed = [s copy];
}
- (int)intValue { return [_objectValue respondsToSelector:@selector(intValue)] ? [_objectValue intValue] : 0; }
- (void)setIntValue:(int)v { self.objectValue = @(v); }
- (NSInteger)integerValue { return [_objectValue respondsToSelector:@selector(integerValue)] ? [_objectValue integerValue] : 0; }
- (void)setIntegerValue:(NSInteger)v { self.objectValue = @(v); }
- (float)floatValue { return [_objectValue respondsToSelector:@selector(floatValue)] ? [_objectValue floatValue] : 0; }
- (void)setFloatValue:(float)v { self.objectValue = @(v); }
- (double)doubleValue { return [_objectValue respondsToSelector:@selector(doubleValue)] ? [_objectValue doubleValue] : 0; }
- (void)setDoubleValue:(double)v { self.objectValue = @(v); }
- (BOOL)boolValue { return [_objectValue respondsToSelector:@selector(boolValue)] ? [_objectValue boolValue] : NO; }
- (void)setBoolValue:(BOOL)v { self.objectValue = @(v); }
- (NSFont *)font { return _font ?: [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:_controlSize]]; }
- (void)setFont:(NSFont *)font { _font = font; }
- (NSInteger)sendActionOn:(NSInteger)mask {
    NSInteger old = _sendActionOn;
    _sendActionOn = mask;
    return old;
}
- (NSSize)cellSize {
    NSSize s = text_size(self.stringValue, self.font);
    if (_image) s.width += _image.size.width, s.height = fmax(s.height, _image.size.height);
    return NSMakeSize(s.width + (_bezeled || _bordered ? 8 : 4), s.height + (_bezeled || _bordered ? 6 : 2));
}
- (NSSize)cellSizeForBounds:(NSRect)bounds { return self.cellSize; }
- (NSRect)drawingRectForBounds:(NSRect)bounds { return NSInsetRect(bounds, 2, 1); }
- (NSRect)titleRectForBounds:(NSRect)bounds { return [self drawingRectForBounds:bounds]; }
- (NSRect)imageRectForBounds:(NSRect)bounds { return [self drawingRectForBounds:bounds]; }
- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView *)view {
    if (_image && _type == 2) [_image drawInRect:frame];
    else draw_text(self.stringValue, self.font, _enabled ? [NSColor controlTextColor] : [NSColor disabledControlTextColor],
                   [self titleRectForBounds:frame], _alignment, _wraps && !_usesSingleLineMode);
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view { [self drawInteriorWithFrame:frame inView:view]; }
- (void)highlight:(BOOL)flag withFrame:(NSRect)frame inView:(NSView *)view { _highlighted = flag; }
- (NSUInteger)hitTestForEvent:(NSEvent *)event inRect:(NSRect)frame ofView:(NSView *)view { return 1; /* content area */ }
- (BOOL)trackMouse:(NSEvent *)event inRect:(NSRect)frame ofView:(NSView *)view untilMouseUp:(BOOL)untilUp { return YES; }
- (BOOL)acceptsFirstResponder { return _enabled && !_refusesFirstResponder; }
- (void)performClick:(id)sender {
    NSControl *v = (NSControl *)_controlView;
    if ([v isKindOfClass:[NSControl class]]) [v performClick:sender];
}
- (id)setUpFieldEditorAttributes:(id)editor { return editor; }
- (NSString *)description { return [NSString stringWithFormat:@"<%@ %p \"%@\">", NSStringFromClass([self class]), self, self.stringValue]; }
@end

/* ================================================================ controls */

@interface NSControl ()
@property(nonatomic) BOOL ignoresMultiClick, continuous, refusesFirstResponder, usesSingleLineMode;
@property(nonatomic, getter=isHighlighted) BOOL highlighted;
@end

@implementation NSControl {
    NSCell *_cell;
}
@synthesize tag = _tag;

+ (Class)cellClass { return [NSCell class]; }
+ (void)setCellClass:(Class)cls {}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _cell = [[[[self class] cellClass] alloc] initTextCell:@""];
        _cell.controlView = self;
    }
    return self;
}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self initWithFrame:NSZeroRect]; }
- (NSCell *)cell { return _cell; }
- (void)setCell:(NSCell *)cell {
    _cell = cell ?: [[[[self class] cellClass] alloc] initTextCell:@""];
    _cell.controlView = self;
    self.needsDisplay = YES;
}
- (NSCell *)selectedCell { return _cell; }
- (NSInteger)selectedTag { return _cell.tag; }
- (BOOL)isFlipped { return YES; }
- (void)hk_changed { self.needsDisplay = YES; }

- (id)target { return _cell.target; }
- (void)setTarget:(id)target { _cell.target = target; }
- (SEL)action { return _cell.action; }
- (void)setAction:(SEL)action { _cell.action = action; }
- (BOOL)isEnabled { return _cell.enabled; }
- (void)setEnabled:(BOOL)enabled {
    _cell.enabled = enabled;
    [self hk_changed];
}
- (id)objectValue { return _cell.objectValue; }
- (void)setObjectValue:(id)value {
    _cell.objectValue = value;
    [self hk_changed];
}
- (NSString *)stringValue { return _cell.stringValue; }
- (void)setStringValue:(NSString *)s {
    _cell.stringValue = s;
    [self hk_changed];
}
- (NSAttributedString *)attributedStringValue { return _cell.attributedStringValue; }
- (void)setAttributedStringValue:(NSAttributedString *)s {
    _cell.attributedStringValue = s;
    [self hk_changed];
}
- (int)intValue { return _cell.intValue; }
- (void)setIntValue:(int)v { self.objectValue = @(v); }
- (NSInteger)integerValue { return _cell.integerValue; }
- (void)setIntegerValue:(NSInteger)v { self.objectValue = @(v); }
- (float)floatValue { return _cell.floatValue; }
- (void)setFloatValue:(float)v { self.objectValue = @(v); }
- (double)doubleValue { return _cell.doubleValue; }
- (void)setDoubleValue:(double)v { self.objectValue = @(v); }
- (BOOL)boolValue { return _cell.boolValue; }
- (void)takeStringValueFrom:(id)sender { self.stringValue = [sender stringValue]; }
- (void)takeObjectValueFrom:(id)sender { self.objectValue = [sender objectValue]; }
- (void)takeIntValueFrom:(id)sender { self.intValue = [sender intValue]; }
- (void)takeDoubleValueFrom:(id)sender { self.doubleValue = [sender doubleValue]; }
- (NSFont *)font { return _cell.font; }
- (void)setFont:(NSFont *)font {
    _cell.font = font;
    [self hk_changed];
}
- (NSInteger)alignment { return _cell.alignment; }
- (void)setAlignment:(NSInteger)a {
    _cell.alignment = a;
    [self hk_changed];
}
- (NSInteger)lineBreakMode { return _cell.lineBreakMode; }
- (void)setLineBreakMode:(NSInteger)m { _cell.lineBreakMode = m; }
- (NSInteger)baseWritingDirection { return _cell.baseWritingDirection; }
- (void)setBaseWritingDirection:(NSInteger)d { _cell.baseWritingDirection = d; }
- (NSControlSize)controlSize { return _cell.controlSize; }
- (void)setControlSize:(NSControlSize)size { _cell.controlSize = size; }
- (NSFormatter *)formatter { return _cell.formatter; }
- (void)setFormatter:(NSFormatter *)f { _cell.formatter = f; }
- (NSInteger)sendActionOn:(NSInteger)mask { return [_cell sendActionOn:mask]; }
- (NSInteger)userInterfaceLayoutDirection { return 0; }
- (BOOL)allowsVibrancy { return NO; }
- (BOOL)allowsExpansionToolTips { return NO; }
- (void)setAllowsExpansionToolTips:(BOOL)flag {}
- (id)currentEditor { return nil; }
- (BOOL)abortEditing { return NO; }
- (void)validateEditing {}
- (void)editWithFrame:(NSRect)rect editor:(id)editor delegate:(id)delegate event:(NSEvent *)event {}
- (void)updateCell:(NSCell *)cell { [self hk_changed]; }
- (void)updateCellInside:(NSCell *)cell { [self hk_changed]; }
- (void)drawCell:(NSCell *)cell { [self hk_changed]; }
- (void)drawCellInside:(NSCell *)cell { [self hk_changed]; }
- (void)selectCell:(NSCell *)cell {}
- (BOOL)acceptsFirstResponder { return self.enabled && !_refusesFirstResponder; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (BOOL)sendAction:(SEL)action to:(id)target {
    if (!action) return NO;
    if (NSApp) return [NSApp sendAction:action to:target from:self];
    if (![target respondsToSelector:action]) return NO;
    ((void (*)(id, SEL, id))objc_msgSend)(target, action, self);
    return YES;
}
/* A click: subclasses change their state first (buttons toggle). */
- (void)hk_clicked { [self sendAction:self.action to:self.target]; }
- (void)performClick:(id)sender {
    if (self.enabled) [self hk_clicked];
}
- (void)mouseDown:(NSEvent *)event {
    if (!self.enabled) return;
    _highlighted = YES;
    _cell.highlighted = YES;
    [self hk_changed];
}
- (void)mouseUp:(NSEvent *)event {
    if (!_highlighted) return;
    _highlighted = NO;
    _cell.highlighted = NO;
    [self hk_changed];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (NSPointInRect(p, self.bounds)) [self hk_clicked];
}
- (NSSize)intrinsicContentSize { return _cell.cellSize; }
- (NSSize)sizeThatFits:(NSSize)size { return self.intrinsicContentSize; }
- (void)sizeToFit {
    NSSize s = self.intrinsicContentSize;
    [self setFrameSize:NSMakeSize(ceil(s.width), ceil(s.height))];
}
- (void)drawRect:(NSRect)rect { [_cell drawWithFrame:self.bounds inView:self]; }
@end

/* ================================================================ buttons */

@interface NSButton ()
@property(nonatomic, copy) NSString *alternateTitle;
@property(nonatomic, strong) NSImage *alternateImage;
@property(nonatomic) NSUInteger imagePosition, imageScaling;
@property(nonatomic) NSEventModifierFlags keyEquivalentModifierMask;
@property(nonatomic) BOOL showsBorderOnlyWhileMouseInside, allowsMixedState, hasDestructiveAction, imageHugsTitle, springLoaded;
@property(nonatomic, getter=isTransparent) BOOL transparent;
@property(nonatomic, strong) NSColor *contentTintColor, *bezelColor;
@property(nonatomic) NSInteger maxAcceleratorLevel;
@end

@implementation NSButton {
    NSImage *_image;
    NSUInteger _buttonType;
}
@synthesize bezelStyle = _bezelStyle, bordered = _bordered, keyEquivalent = _keyEquivalent;

+ (instancetype)buttonWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    NSButton *b = [[self alloc] initWithFrame:NSMakeRect(0, 0, 80, 22)];
    b.title = title;
    b.target = target;
    b.action = action;
    [b sizeToFit];
    return b;
}
+ (instancetype)buttonWithImage:(NSImage *)image target:(id)target action:(SEL)action {
    NSButton *b = [[self alloc] initWithFrame:NSMakeRect(0, 0, 32, 22)];
    b.title = @"";
    b.image = image;
    b.imagePosition = 1; /* NSImageOnly */
    b.target = target;
    b.action = action;
    [b sizeToFit];
    return b;
}
+ (instancetype)buttonWithTitle:(NSString *)title image:(NSImage *)image target:(id)target action:(SEL)action {
    NSButton *b = [self buttonWithTitle:title target:target action:action];
    b.image = image;
    b.imagePosition = 2; /* NSImageLeft */
    [b sizeToFit];
    return b;
}
+ (instancetype)checkboxWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    NSButton *b = [self buttonWithTitle:title target:target action:action];
    [b setButtonType:3];
    return b;
}
+ (instancetype)radioButtonWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    NSButton *b = [self buttonWithTitle:title target:target action:action];
    [b setButtonType:4];
    return b;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.cell.stringValue = @"Button";
        self.cell.alignment = HKAlignCenter;
        _bordered = YES;
        _bezelStyle = 1; /* NSBezelStyleRounded (push) */
        _keyEquivalent = @"";
    }
    return self;
}
- (NSString *)title { return self.cell.stringValue; }
- (void)setTitle:(NSString *)title {
    self.cell.stringValue = title ?: @"";
    [self hk_changed];
}
- (NSAttributedString *)attributedTitle { return self.cell.attributedStringValue; }
- (void)setAttributedTitle:(NSAttributedString *)t { self.cell.attributedStringValue = t; }
- (NSImage *)image { return _image; }
- (void)setImage:(NSImage *)image {
    _image = image;
    [self hk_changed];
}
- (NSControlStateValue)state { return self.cell.state; }
- (void)setState:(NSControlStateValue)state {
    self.cell.state = state;
    [self hk_changed];
}
- (void)setNextState { self.state = self.state ? 0 : 1; }
- (void)setButtonType:(NSUInteger)type { _buttonType = type; }
- (NSUInteger)hk_buttonType { return _buttonType; }
- (BOOL)hk_toggles { return _buttonType == 1 || _buttonType == 2 || _buttonType == 3 || _buttonType == 4 || _buttonType == 6; }
- (void)hk_clicked {
    if ([self hk_toggles]) [self setNextState];
    [super hk_clicked];
}
- (void)highlight:(BOOL)flag { self.highlighted = flag; }
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (!_keyEquivalent.length || !self.enabled || event.type != NSEventTypeKeyDown) return NO;
    NSEventModifierFlags want = self.keyEquivalentModifierMask & (NSEventModifierFlagCommand | NSEventModifierFlagOption |
                                                                  NSEventModifierFlagControl | NSEventModifierFlagShift);
    NSEventModifierFlags have = event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagOption |
                                                       NSEventModifierFlagControl | NSEventModifierFlagShift);
    if (want != have || ![event.charactersIgnoringModifiers isEqualToString:_keyEquivalent]) return NO;
    [self performClick:self];
    return YES;
}
- (NSSize)intrinsicContentSize {
    NSString *t = self.title;
    NSSize s = t.length && self.imagePosition != 1 ? text_size(t, self.font) : NSZeroSize;
    if (_image) s.width += _image.size.width + (t.length ? 4 : 0), s.height = fmax(s.height, _image.size.height);
    if (_buttonType == 3 || _buttonType == 4) return NSMakeSize(s.width + 22, fmax(s.height, 16));
    return NSMakeSize(s.width + (_bordered ? 24 : 4), fmax(s.height + 6, _bordered ? 22 : 0));
}
- (void)drawRect:(NSRect)rect {
    NSRect b = self.bounds;
    BOOL check = _buttonType == 3 || _buttonType == 4;
    if (check) {
        NSRect box = NSMakeRect(1, (b.size.height - 14) / 2, 14, 14);
        fill_rounded(box, _buttonType == 4 ? 7 : 3, self.state ? [NSColor controlAccentColor] : [NSColor controlColor],
                     [NSColor separatorColor]);
        b.origin.x += 20, b.size.width -= 20;
    } else if (_bordered && !self.transparent) {
        fill_rounded(b, 5, self.highlighted ? [NSColor selectedControlColor] : [NSColor controlColor], [NSColor separatorColor]);
    }
    if (_image && !check) {
        NSSize is = _image.size;
        NSRect ir = NSMakeRect(self.title.length && self.imagePosition != 1 ? b.origin.x + 6 : NSMidX(b) - is.width / 2,
                               NSMidY(b) - is.height / 2, is.width, is.height);
        [_image drawInRect:ir];
        if (self.title.length && self.imagePosition != 1) b.origin.x += is.width + 6, b.size.width -= is.width + 6;
    }
    if (self.title.length && self.imagePosition != 1)
        draw_text(self.title, self.font, self.enabled ? (self.contentTintColor ?: [NSColor controlTextColor]) : [NSColor disabledControlTextColor],
                  b, check ? HKAlignLeft : self.alignment, NO);
}
@end

@implementation NSStatusBarButton
@synthesize appearsDisabled = _appearsDisabled;
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.bordered = NO;
        self.title = @"";
    }
    return self;
}
@end

/* ================================================================ text fields */

@interface NSTextField : NSControl
@property(nonatomic, copy) NSString *placeholderString;
@property(nonatomic, copy) NSAttributedString *placeholderAttributedString;
@property(nonatomic, getter=isEditable) BOOL editable;
@property(nonatomic, getter=isSelectable) BOOL selectable;
@property(nonatomic, getter=isBezeled) BOOL bezeled;
@property(nonatomic, getter=isBordered) BOOL bordered;
@property(nonatomic) BOOL drawsBackground, allowsEditingTextAttributes, importsGraphics, allowsDefaultTighteningForTruncation;
@property(nonatomic, getter=isAutomaticTextCompletionEnabled) BOOL automaticTextCompletionEnabled;
@property(nonatomic) BOOL allowsCharacterPickerTouchBarItem;
@property(nonatomic, strong) NSColor *textColor, *backgroundColor;
@property(nonatomic) NSUInteger bezelStyle;
@property(nonatomic, weak) id delegate;
@property(nonatomic) CGFloat preferredMaxLayoutWidth;
@property(nonatomic) NSInteger maximumNumberOfLines, lineBreakStrategy;
@end

@implementation NSTextField
+ (instancetype)labelWithString:(NSString *)s {
    NSTextField *t = [[self alloc] initWithFrame:NSZeroRect];
    t.stringValue = s ?: @"";
    t.editable = t.selectable = t.bezeled = t.bordered = t.drawsBackground = NO;
    t.cell.wraps = NO;
    [t sizeToFit];
    return t;
}
+ (instancetype)labelWithAttributedString:(NSAttributedString *)s {
    NSTextField *t = [self labelWithString:s.string];
    t.attributedStringValue = s;
    return t;
}
+ (instancetype)wrappingLabelWithString:(NSString *)s {
    NSTextField *t = [self labelWithString:s];
    t.cell.wraps = YES;
    t.selectable = YES;
    return t;
}
+ (instancetype)textFieldWithString:(NSString *)s {
    NSTextField *t = [[self alloc] initWithFrame:NSZeroRect];
    t.stringValue = s ?: @"";
    t.cell.wraps = NO;
    [t sizeToFit];
    return t;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _editable = _selectable = _bezeled = _drawsBackground = YES;
        _textColor = [NSColor controlTextColor];
        _backgroundColor = [NSColor textBackgroundColor];
        self.cell.alignment = HKAlignNatural;
    }
    return self;
}
- (BOOL)acceptsFirstResponder { return _editable || _selectable; }
- (void)setTextColor:(NSColor *)c {
    _textColor = c;
    [self hk_changed];
}
- (void)setBackgroundColor:(NSColor *)c {
    _backgroundColor = c;
    [self hk_changed];
}
- (void)setDrawsBackground:(BOOL)flag {
    _drawsBackground = flag;
    [self hk_changed];
}
- (void)selectText:(id)sender {}
- (BOOL)textShouldBeginEditing:(id)text { return _editable; }
- (BOOL)textShouldEndEditing:(id)text { return YES; }
- (NSSize)intrinsicContentSize {
    NSString *s = self.stringValue.length ? self.stringValue : (_placeholderString ?: @"");
    NSSize t = text_size(s, self.font);
    CGFloat pad = _bezeled || _bordered ? 8 : 4;
    if (self.cell.wraps && _preferredMaxLayoutWidth > 0 && t.width > _preferredMaxLayoutWidth) {
        CFAttributedStringRef a = make_text(s, self.font, nil);
        CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(a);
        CGSize fit = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRangeMake(0, 0), NULL,
                                                                  CGSizeMake(_preferredMaxLayoutWidth - pad, CGFLOAT_MAX), NULL);
        CFRelease(fs);
        CFRelease(a);
        return NSMakeSize(_preferredMaxLayoutWidth, ceil(fit.height) + (_bezeled || _bordered ? 6 : 2));
    }
    return NSMakeSize(t.width + pad, t.height + (_bezeled || _bordered ? 6 : 2));
}
- (void)drawRect:(NSRect)rect {
    NSRect b = self.bounds;
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    if (_drawsBackground && _backgroundColor && c) {
        CGContextSetFillColorWithColor(c, _backgroundColor.CGColor);
        CGContextFillRect(c, b);
    }
    if (_bezeled || _bordered) fill_rounded(b, _bezeled ? 3 : 0, nil, [NSColor separatorColor]);
    NSRect inner = NSInsetRect(b, _bezeled || _bordered ? 4 : 2, _bezeled || _bordered ? 3 : 1);
    NSString *s = self.stringValue;
    NSColor *color = self.enabled ? (_textColor ?: [NSColor controlTextColor]) : [NSColor disabledControlTextColor];
    if (!s.length && _placeholderString.length) s = _placeholderString, color = [NSColor placeholderTextColor];
    draw_text(s, self.font, color, inner, self.alignment, self.cell.wraps && !self.cell.usesSingleLineMode);
}
@end

/* ================================================================ image views */

@interface NSImageView : NSControl
@property(nonatomic, strong) NSImage *image;
@property(nonatomic) NSUInteger imageScaling, imageAlignment, imageFrameStyle;
@property(nonatomic) BOOL animates, allowsCutCopyPaste;
@property(nonatomic, getter=isEditable) BOOL editable;
@property(nonatomic, strong) NSColor *contentTintColor;
@property(nonatomic, strong) id symbolConfiguration;
@end
@implementation NSImageView
+ (instancetype)imageViewWithImage:(NSImage *)image {
    NSSize s = image.size;
    NSImageView *v = [[self alloc] initWithFrame:NSMakeRect(0, 0, s.width, s.height)];
    v.image = image;
    return v;
}
- (BOOL)isFlipped { return NO; }
- (void)setImage:(NSImage *)image {
    _image = image;
    [self hk_changed];
}
- (void)setImageScaling:(NSUInteger)s {
    _imageScaling = s;
    [self hk_changed];
}
- (NSSize)intrinsicContentSize { return _image ? _image.size : NSZeroSize; }
- (void)drawRect:(NSRect)rect {
    if (!_image) return;
    NSRect b = self.bounds;
    NSSize s = _image.size;
    if (s.width <= 0 || s.height <= 0) return;
    NSRect dst;
    switch (_imageScaling) {
    case 1: dst = b; break; /* axes independently */
    case 2: dst = NSMakeRect(NSMidX(b) - s.width / 2, NSMidY(b) - s.height / 2, s.width, s.height); break; /* none */
    default: {
        CGFloat k = fmin(b.size.width / s.width, b.size.height / s.height);
        if (_imageScaling == 0) k = fmin(k, 1); /* proportionally down */
        dst = NSMakeRect(NSMidX(b) - s.width * k / 2, NSMidY(b) - s.height * k / 2, s.width * k, s.height * k);
    }
    }
    [_image drawInRect:dst];
}
@end

/* ================================================================ pop-up buttons: the menu and selection live in the cell */

@interface NSPopUpButtonCell : NSCell
@property(nonatomic, strong) NSMenu *menu;
@property(nonatomic) BOOL pullsDown, autoenablesItems, usesItemFromMenu, altersStateOfSelectedItem;
@property(nonatomic) NSRectEdge preferredEdge;
@property(nonatomic) NSUInteger arrowPosition;
@end
@implementation NSPopUpButtonCell {
    NSInteger _selected;
}
- (instancetype)initTextCell:(NSString *)title pullsDown:(BOOL)pullsDown {
    if ((self = [super initTextCell:title])) {
        _menu = [[NSMenu alloc] initWithTitle:@""];
        _pullsDown = pullsDown;
        _autoenablesItems = _usesItemFromMenu = _altersStateOfSelectedItem = YES;
        _preferredEdge = NSRectEdgeMaxY;
        _arrowPosition = 1;
        _selected = pullsDown ? -1 : 0;
    }
    return self;
}
- (instancetype)initTextCell:(NSString *)title { return [self initTextCell:title pullsDown:NO]; }
- (void)setMenu:(NSMenu *)menu {
    _menu = menu ?: [[NSMenu alloc] initWithTitle:@""];
    _selected = _menu.numberOfItems && !_pullsDown ? 0 : -1;
}
- (NSArray<NSMenuItem *> *)itemArray { return _menu.itemArray; }
- (NSInteger)numberOfItems { return _menu.numberOfItems; }
- (void)addItemWithTitle:(NSString *)title {
    [self removeItemWithTitle:title];
    [_menu addItemWithTitle:title ?: @"" action:NULL keyEquivalent:@""];
    if (_selected < 0 && !_pullsDown) _selected = 0;
}
- (void)addItemsWithTitles:(NSArray<NSString *> *)titles {
    for (NSString *t in titles) [self addItemWithTitle:t];
}
- (void)insertItemWithTitle:(NSString *)title atIndex:(NSInteger)index {
    [self removeItemWithTitle:title];
    [_menu insertItemWithTitle:title ?: @"" action:NULL keyEquivalent:@"" atIndex:index];
    if (_selected >= index) _selected++;
    if (_selected < 0 && !_pullsDown) _selected = 0;
}
- (void)removeItemAtIndex:(NSInteger)index {
    [_menu removeItemAtIndex:index];
    if (_selected == index) _selected = _menu.numberOfItems && !_pullsDown ? 0 : -1;
    else if (_selected > index) _selected--;
}
- (void)removeItemWithTitle:(NSString *)title {
    NSInteger i = [_menu indexOfItemWithTitle:title];
    if (i >= 0) [self removeItemAtIndex:i];
}
- (void)removeAllItems {
    [_menu removeAllItems];
    _selected = -1;
}
- (NSMenuItem *)itemAtIndex:(NSInteger)index { return [_menu itemAtIndex:index]; }
- (NSMenuItem *)itemWithTitle:(NSString *)title { return [_menu itemWithTitle:title]; }
- (NSMenuItem *)lastItem { return [_menu itemAtIndex:_menu.numberOfItems - 1]; }
- (NSInteger)indexOfItem:(NSMenuItem *)item { return [_menu indexOfItem:item]; }
- (NSInteger)indexOfItemWithTitle:(NSString *)title { return [_menu indexOfItemWithTitle:title]; }
- (NSInteger)indexOfItemWithTag:(NSInteger)tag { return [_menu indexOfItemWithTag:tag]; }
- (NSInteger)indexOfItemWithRepresentedObject:(id)object { return [_menu indexOfItemWithRepresentedObject:object]; }
- (NSInteger)indexOfItemWithTarget:(id)target andAction:(SEL)action { return [_menu indexOfItemWithTarget:target andAction:action]; }
- (NSString *)itemTitleAtIndex:(NSInteger)index { return [_menu itemAtIndex:index].title ?: @""; }
- (NSArray<NSString *> *)itemTitles { return [_menu.itemArray valueForKey:@"title"]; }
- (void)selectItemAtIndex:(NSInteger)index {
    if (index >= _menu.numberOfItems) index = -1;
    if (_altersStateOfSelectedItem && !_pullsDown) {
        [_menu itemAtIndex:_selected].state = 0;
        [_menu itemAtIndex:index].state = 1;
    }
    _selected = index;
    [(NSControl *)self.controlView setNeedsDisplay:YES];
}
- (void)selectItem:(NSMenuItem *)item { [self selectItemAtIndex:item ? [_menu indexOfItem:item] : -1]; }
- (BOOL)selectItemWithTag:(NSInteger)tag {
    NSInteger i = [_menu indexOfItemWithTag:tag];
    if (i >= 0) [self selectItemAtIndex:i];
    return i >= 0;
}
- (void)selectItemWithTitle:(NSString *)title { [self selectItemAtIndex:[_menu indexOfItemWithTitle:title]]; }
- (NSMenuItem *)selectedItem { return [_menu itemAtIndex:_selected]; }
- (NSInteger)indexOfSelectedItem { return _selected; }
- (NSInteger)selectedTag { return self.selectedItem ? self.selectedItem.tag : -1; }
- (NSString *)titleOfSelectedItem { return self.selectedItem.title; }
- (void)synchronizeTitleAndSelectedItem {}
- (NSString *)stringValue {
    if (_pullsDown) return [_menu itemAtIndex:0].title ?: @"";
    return self.selectedItem.title ?: @"";
}
- (void)setTitle:(NSString *)title {
    if (_pullsDown) {
        NSMenuItem *first = [_menu itemAtIndex:0];
        if (first) first.title = title ?: @"";
        else [self addItemWithTitle:title];
        return;
    }
    NSInteger i = [_menu indexOfItemWithTitle:title];
    if (i < 0) {
        [self addItemWithTitle:title];
        i = [_menu indexOfItemWithTitle:title];
    }
    [self selectItemAtIndex:i];
}
- (id)objectValue { return @(_selected); }
- (void)setObjectValue:(id)value {
    if ([value respondsToSelector:@selector(integerValue)]) [self selectItemAtIndex:[value integerValue]];
}
- (void)attachPopUpWithFrame:(NSRect)frame inView:(NSView *)view {}
- (void)dismissPopUp {}
- (void)performClickWithFrame:(NSRect)frame inView:(NSView *)view {
    hk_menu_cell_click(self, frame, view); /* HKMenu.m */
}
@end

@interface NSPopUpButton : NSButton
@end
@implementation NSPopUpButton
+ (Class)cellClass { return [NSPopUpButtonCell class]; }
- (instancetype)initWithFrame:(NSRect)frame pullsDown:(BOOL)pullsDown {
    if ((self = [super initWithFrame:frame])) {
        NSPopUpButtonCell *c = [[NSPopUpButtonCell alloc] initTextCell:@"" pullsDown:pullsDown];
        self.cell = c;
        self.alignment = HKAlignLeft;
    }
    return self;
}
- (instancetype)initWithFrame:(NSRect)frame { return [self initWithFrame:frame pullsDown:NO]; }
+ (instancetype)popUpButtonWithMenu:(NSMenu *)menu target:(id)target action:(SEL)action {
    NSPopUpButton *b = [[self alloc] initWithFrame:NSMakeRect(0, 0, 100, 22) pullsDown:NO];
    b.menu = menu;
    b.target = target;
    b.action = action;
    return b;
}
+ (instancetype)pullDownButtonWithTitle:(NSString *)title menu:(NSMenu *)menu {
    NSPopUpButton *b = [[self alloc] initWithFrame:NSMakeRect(0, 0, 100, 22) pullsDown:YES];
    b.menu = menu;
    b.title = title;
    return b;
}
- (NSPopUpButtonCell *)hk_cell { return (NSPopUpButtonCell *)self.cell; }
- (NSMenu *)menu { return self.hk_cell.menu; }
- (void)setMenu:(NSMenu *)menu {
    self.hk_cell.menu = menu;
    [self hk_changed];
}
- (NSString *)title { return self.hk_cell.stringValue; }
- (void)setTitle:(NSString *)title {
    [self.hk_cell setTitle:title];
    [self hk_changed];
}
- (BOOL)pullsDown { return self.hk_cell.pullsDown; }
- (void)setPullsDown:(BOOL)flag { self.hk_cell.pullsDown = flag; }
- (BOOL)autoenablesItems { return self.hk_cell.autoenablesItems; }
- (void)setAutoenablesItems:(BOOL)flag { self.hk_cell.autoenablesItems = flag; }
- (NSRectEdge)preferredEdge { return self.hk_cell.preferredEdge; }
- (void)setPreferredEdge:(NSRectEdge)edge { self.hk_cell.preferredEdge = edge; }
- (NSInteger)selectedTag { return self.hk_cell.selectedTag; }
/* Everything else a pop-up button answers (addItemWithTitle:, selectItemAtIndex:, indexOfSelectedItem...) is its cell's. */
- (id)forwardingTargetForSelector:(SEL)sel {
    return [self.hk_cell respondsToSelector:sel] ? self.hk_cell : [super forwardingTargetForSelector:sel];
}
- (BOOL)respondsToSelector:(SEL)sel { return [super respondsToSelector:sel] || [self.hk_cell respondsToSelector:sel]; }
- (void)hk_clicked { [self.hk_cell performClickWithFrame:self.bounds inView:self]; }
- (void)drawRect:(NSRect)rect {
    NSRect b = self.bounds;
    fill_rounded(b, 5, [NSColor controlColor], [NSColor separatorColor]);
    draw_text(self.title, self.font, self.enabled ? [NSColor controlTextColor] : [NSColor disabledControlTextColor],
              NSMakeRect(8, 0, b.size.width - 26, b.size.height), HKAlignLeft, NO);
    draw_text(@"⌄", self.font, [NSColor secondaryLabelColor], NSMakeRect(b.size.width - 18, 0, 12, b.size.height),
              HKAlignCenter, NO);
}
@end

/* ================================================================ scrollers: overlay style, as on a trackpad Mac */

@interface NSScroller : NSControl
@property(nonatomic) CGFloat knobProportion;
@property(nonatomic) NSInteger scrollerStyle, knobStyle;
@end
@implementation NSScroller
+ (CGFloat)scrollerWidthForControlSize:(NSControlSize)size scrollerStyle:(NSInteger)style { return 15; }
+ (CGFloat)scrollerWidth { return 15; }
+ (CGFloat)scrollerWidthForControlSize:(NSControlSize)size { return 15; }
+ (NSInteger)preferredScrollerStyle { return 1; /* NSScrollerStyleOverlay */ }
+ (BOOL)isCompatibleWithOverlayScrollers { return YES; }
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) _scrollerStyle = 1;
    return self;
}
- (NSUInteger)usableParts { return 2; /* NSAllScrollerParts */ }
- (NSUInteger)hitPart { return 0; }
- (NSUInteger)testPart:(NSPoint)point { return 0; }
- (NSRect)rectForPart:(NSUInteger)part { return self.bounds; }
- (void)drawKnob {}
- (void)drawKnobSlotInRect:(NSRect)rect highlight:(BOOL)flag {}
- (void)trackKnob:(NSEvent *)event {}
- (void)checkSpaceForParts {}
- (void)drawRect:(NSRect)rect {}
@end

/* ================================================================ effect views: plain views here */

@interface NSVisualEffectView : NSView
@property(nonatomic) NSInteger material, blendingMode, state;
@property(nonatomic, getter=isEmphasized) BOOL emphasized;
@property(nonatomic, strong) NSImage *maskImage;
@end
@implementation NSVisualEffectView
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) _material = 3; /* NSVisualEffectMaterialAppearanceBased-era default */
    return self;
}
- (BOOL)appearsDarker { return NO; }
- (BOOL)allowsVibrancy { return NO; }
@end

@interface NSGlassEffectView : NSView
@property(nonatomic, strong) NSView *contentView;
@property(nonatomic) CGFloat cornerRadius;
@property(nonatomic, strong) NSColor *tintColor;
@property(nonatomic) NSInteger style;
@end
@implementation NSGlassEffectView
- (void)setContentView:(NSView *)view {
    if (_contentView == view) return;
    [_contentView removeFromSuperview];
    _contentView = view;
    if (view) {
        view.frame = self.bounds;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [self addSubview:view];
    }
}
- (void)setCornerRadius:(CGFloat)r {
    _cornerRadius = r;
    self.layer.cornerRadius = r;
}
@end

/* ================================================================ window frame views (subclassed by apps for custom title bars) */

@interface NSNextStepFrame : NSView
- (instancetype)initWithFrame:(NSRect)frame styleMask:(NSUInteger)style owner:(id)owner;
@end
@implementation NSNextStepFrame {
  @protected
    NSUInteger _styleMask;
    __weak id _owner;
}
+ (NSRect)frameRectForContentRect:(NSRect)rect styleMask:(NSUInteger)style { return rect; }
+ (NSRect)contentRectForFrameRect:(NSRect)rect styleMask:(NSUInteger)style { return rect; }
+ (NSSize)minFrameSizeForMinContentSize:(NSSize)size styleMask:(NSUInteger)style { return size; }
+ (CGFloat)minFrameWidthWithTitle:(NSString *)title styleMask:(NSUInteger)style { return 0; }
- (instancetype)initWithFrame:(NSRect)frame styleMask:(NSUInteger)style owner:(id)owner {
    if ((self = [super initWithFrame:frame])) {
        _styleMask = style;
        _owner = owner;
    }
    return self;
}
- (instancetype)initWithFrame:(NSRect)frame { return [self initWithFrame:frame styleMask:0 owner:nil]; }
- (NSUInteger)styleMask { return _styleMask; }
- (void)setStyleMask:(NSUInteger)style { _styleMask = style; }
- (NSRect)frameRectForContentRect:(NSRect)rect styleMask:(NSUInteger)style {
    return [[self class] frameRectForContentRect:rect styleMask:style];
}
- (NSRect)contentRectForFrameRect:(NSRect)rect styleMask:(NSUInteger)style {
    return [[self class] contentRectForFrameRect:rect styleMask:style];
}
- (NSRect)contentRect { return [self contentRectForFrameRect:self.bounds styleMask:_styleMask]; }
- (CGFloat)_titlebarHeight { return 0; }
- (NSRect)titlebarRect { return NSZeroRect; }
- (NSRect)_draggableFrame { return NSZeroRect; }
- (BOOL)_shouldCenterTrafficLights { return NO; }
- (BOOL)_shouldFlipTrafficLightsForRTL { return NO; }
- (CGFloat)_minXTitlebarWidgetInset { return 0; }
- (CGFloat)_minYTitlebarButtonsOffset { return 0; }
- (NSButton *)closeButton { return nil; }
- (NSButton *)minimizeButton { return nil; }
- (NSButton *)zoomButton { return nil; }
- (NSString *)title { return @""; }
- (void)setTitle:(NSString *)title {}
- (void)setRepresentedFilename:(NSString *)name {}
- (void)tile {}
- (void)shapeWindow {}
- (void)setDocumentEdited:(BOOL)flag {}
- (BOOL)isFlipped { return NO; }
@end

@interface NSThemeFrame : NSNextStepFrame
@end
@implementation NSThemeFrame
+ (instancetype)containingThemeFrameFromView:(NSView *)view {
    for (NSView *v = view; v; v = v.superview)
        if ([v isKindOfClass:self]) return (NSThemeFrame *)v;
    return nil;
}
- (CGFloat)customTitlebarHeight { return 0; }
- (NSView *)titlebarView { return nil; }
- (NSView *)titlebarContainerView { return nil; }
- (NSArray *)titlebarAccessoryViewControllers { return @[]; }
- (CGFloat)titlebarAlphaValue { return 1; }
- (NSRect)titlebarRectIncludingToolbar { return NSZeroRect; }
- (NSButton *)toolbarButton { return nil; }
- (void)addTitlebarSubview:(NSView *)view { [self addSubview:view]; }
@end

/* AppKit's title bar button view: here an empty view. */
@interface _NSThemeWidget : NSView
@end
@implementation _NSThemeWidget
- (instancetype)initWithButtonID:(NSInteger)buttonID { return [self initWithFrame:NSMakeRect(0, 0, 14, 16)]; }
@end

/* ================================================================ accessibility elements (no assistive client here) */

@interface NSAccessibilityElement : HKShimObject
@property(nonatomic, copy) NSString *accessibilityRole, *accessibilitySubrole, *accessibilityRoleDescription;
@property(nonatomic, copy) NSString *accessibilityLabel, *accessibilityTitle, *accessibilityHelp, *accessibilityIdentifier;
@property(nonatomic, copy) NSString *accessibilityPlaceholderValue, *accessibilityValueDescription;
@property(nonatomic, strong) id accessibilityValue;
@property(nonatomic, weak) id accessibilityParent, accessibilityTopLevelUIElement, accessibilityWindow;
@property(nonatomic, copy) NSArray *accessibilityChildren, *accessibilitySelectedChildren, *accessibilityCustomActions;
@property(nonatomic) NSRect accessibilityFrame, accessibilityFrameInParentSpace;
@property(nonatomic, getter=isAccessibilityElement) BOOL accessibilityElement;
@property(nonatomic, getter=isAccessibilityEnabled) BOOL accessibilityEnabled;
@property(nonatomic, getter=isAccessibilityFocused) BOOL accessibilityFocused;
@property(nonatomic, getter=isAccessibilitySelected) BOOL accessibilitySelected;
@property(nonatomic, getter=isAccessibilityHidden) BOOL accessibilityHidden;
@property(nonatomic, getter=isAccessibilityExpanded) BOOL accessibilityExpanded;
@property(nonatomic, getter=isAccessibilityRequired) BOOL accessibilityRequired;
@property(nonatomic, strong) NSURL *accessibilityURL;
@property(nonatomic) NSInteger accessibilityIndex;
@end
@implementation NSAccessibilityElement
+ (id)accessibilityElementWithRole:(NSString *)role frame:(NSRect)frame label:(NSString *)label parent:(id)parent {
    NSAccessibilityElement *e = [[self alloc] init];
    e.accessibilityRole = role;
    e.accessibilityFrame = frame;
    e.accessibilityLabel = label;
    e.accessibilityParent = parent;
    return e;
}
- (instancetype)init {
    if ((self = [super init])) {
        _accessibilityElement = YES;
        _accessibilityEnabled = YES;
    }
    return self;
}
- (void)accessibilityAddChildElement:(NSAccessibilityElement *)child {
    if (!child) return;
    self.accessibilityChildren = [(self.accessibilityChildren ?: @[]) arrayByAddingObject:child];
    child.accessibilityParent = self;
}
- (NSPoint)accessibilityActivationPoint { return NSMakePoint(NSMidX(_accessibilityFrame), NSMidY(_accessibilityFrame)); }
- (BOOL)isAccessibilitySelectorAllowed:(SEL)selector { return YES; }
- (id)accessibilityFocusedUIElement { return self; }
- (id)accessibilityHitTest:(NSPoint)point { return self; }
- (BOOL)accessibilityPerformPress { return NO; }
- (BOOL)accessibilityPerformShowMenu { return NO; }
- (NSArray *)accessibilityChildrenInNavigationOrder { return self.accessibilityChildren; }
- (NSArray *)accessibilityAttributeNames { return @[]; }
- (id)accessibilityAttributeValue:(NSString *)attribute { return nil; }
- (BOOL)accessibilityIsIgnored { return !_accessibilityElement; }
- (NSArray *)accessibilityActionNames { return @[]; }
@end

@interface NSAccessibilityRemoteUIElement : HKShimObject
@property(nonatomic, strong) id windowUIElement, topLevelUIElement;
@property(nonatomic, weak) id accessibilityParent;
@end
@implementation NSAccessibilityRemoteUIElement {
    NSData *_token;
}
static BOOL g_remote_ui_app;
+ (void)setRemoteUIApp:(BOOL)flag { g_remote_ui_app = flag; }
+ (BOOL)isRemoteUIApp { return g_remote_ui_app; }
+ (NSData *)remoteTokenForLocalUIElement:(id)element {
    pid_t pid = getpid();
    uint64_t ptr = (uint64_t)(uintptr_t)(__bridge void *)element;
    NSMutableData *d = [NSMutableData dataWithBytes:&pid length:sizeof pid];
    [d appendBytes:&ptr length:sizeof ptr];
    return d;
}
+ (void)registerRemoteUIProcessIdentifier:(pid_t)pid {}
+ (void)unregisterRemoteUIProcessIdentifier:(pid_t)pid {}
- (instancetype)initWithRemoteToken:(NSData *)token {
    if ((self = [super init])) _token = [token copy];
    return self;
}
- (instancetype)initWithAXUIElement:(CFTypeRef)element { return [self initWithRemoteToken:nil]; }
- (pid_t)processIdentifier { return 0; }
- (NSArray *)accessibilityChildren { return @[]; }
- (BOOL)isAccessibilityElement { return NO; }
@end

@interface NSAccessibilityCustomAction : HKShimObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) BOOL (^handler)(void);
@property(nonatomic, weak) id target;
@property(nonatomic) SEL selector;
@end
@implementation NSAccessibilityCustomAction
- (instancetype)initWithName:(NSString *)name handler:(BOOL (^)(void))handler {
    if ((self = [super init])) {
        _name = [name copy];
        _handler = [handler copy];
    }
    return self;
}
- (instancetype)initWithName:(NSString *)name target:(id)target selector:(SEL)selector {
    if ((self = [super init])) {
        _name = [name copy];
        _target = target;
        _selector = selector;
    }
    return self;
}
- (NSAttributedString *)attributedName { return [[NSAttributedString alloc] initWithString:_name ?: @""]; }
- (void)setAttributedName:(NSAttributedString *)name { _name = [name.string copy]; }
@end
