/* Henge's AppKit: the AppKit a Mac app sees when it runs inside Henge on iOS, without macOS.
   Built only on what iOS has too (Foundation, CoreGraphics, CoreText, QuartzCore, ImageIO, IOSurface), never on
   UIKit or AppKit, so the same code runs in the Mac test runner (tl/tools/tlios) and in the iOS app.

   Classes carry AppKit's own names (NSWindow, NSView...): the translation layer binds a program's references to
   AppKit's classes, functions and constants to these (hk_lookup). A process using it must never load the real
   AppKit. Where a Mac app needs a screen, the windows are handed to a presenter (HKPresenter below): the iOS app
   hosts each window's layer in a UIView; the Mac runner draws them into PNG files.

   Unimplemented messages to these classes are logged once and answer nil/0 (HKShimObject), so an app gets as far as
   it can and the log says what to write next. */
#ifndef HENGE_HK_SHIM_H
#define HENGE_HK_SHIM_H

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

/* ---- AppKit's geometry and basic types (on iOS Foundation lacks the NS names) ---- */
#if !TARGET_OS_OSX
typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;
typedef NSPoint *NSPointPointer;
typedef NSRect *NSRectPointer;
typedef NS_ENUM(NSUInteger, NSRectEdge) { NSRectEdgeMinX = 0, NSRectEdgeMinY = 1, NSRectEdgeMaxX = 2, NSRectEdgeMaxY = 3 };
typedef struct NSEdgeInsets { CGFloat top, left, bottom, right; } NSEdgeInsets;
#define NSEdgeInsetsZero ((NSEdgeInsets){0, 0, 0, 0})
#define NSZeroPoint CGPointZero
#define NSZeroSize CGSizeZero
#define NSZeroRect CGRectZero
#define NSMakePoint CGPointMake
#define NSMakeSize CGSizeMake
#define NSMakeRect CGRectMake
#define NSMinX CGRectGetMinX
#define NSMinY CGRectGetMinY
#define NSMaxX CGRectGetMaxX
#define NSMaxY CGRectGetMaxY
#define NSMidX CGRectGetMidX
#define NSMidY CGRectGetMidY
#define NSWidth CGRectGetWidth
#define NSHeight CGRectGetHeight
#define NSIsEmptyRect CGRectIsEmpty
#define NSPointInRect(p, r) CGRectContainsPoint(r, p)
#define NSEqualRects CGRectEqualToRect
#define NSEqualSizes CGSizeEqualToSize
#define NSEqualPoints CGPointEqualToPoint
#define NSIntersectionRect CGRectIntersection
#define NSUnionRect CGRectUnion
#define NSInsetRect CGRectInset
#define NSOffsetRect CGRectOffset
#define NSStringFromRect(r) [NSString stringWithFormat:@"{{%g, %g}, {%g, %g}}", (r).origin.x, (r).origin.y, (r).size.width, (r).size.height]
#define NSStringFromPoint(p) [NSString stringWithFormat:@"{%g, %g}", (p).x, (p).y]
#define NSStringFromSize(s) [NSString stringWithFormat:@"{%g, %g}", (s).width, (s).height]
#endif

/* ---- enums with AppKit's values ---- */
typedef NS_ENUM(NSUInteger, NSEventType) {
    NSEventTypeLeftMouseDown = 1, NSEventTypeLeftMouseUp = 2, NSEventTypeRightMouseDown = 3,
    NSEventTypeRightMouseUp = 4, NSEventTypeMouseMoved = 5, NSEventTypeLeftMouseDragged = 6,
    NSEventTypeRightMouseDragged = 7, NSEventTypeMouseEntered = 8, NSEventTypeMouseExited = 9,
    NSEventTypeKeyDown = 10, NSEventTypeKeyUp = 11, NSEventTypeFlagsChanged = 12, NSEventTypeAppKitDefined = 13,
    NSEventTypeSystemDefined = 14, NSEventTypeApplicationDefined = 15, NSEventTypePeriodic = 16,
    NSEventTypeCursorUpdate = 17, NSEventTypeScrollWheel = 22, NSEventTypeTabletPoint = 23,
    NSEventTypeTabletProximity = 24, NSEventTypeOtherMouseDown = 25, NSEventTypeOtherMouseUp = 26,
    NSEventTypeOtherMouseDragged = 27, NSEventTypeGesture = 29, NSEventTypeMagnify = 30, NSEventTypeSwipe = 31,
    NSEventTypeRotate = 18, NSEventTypeBeginGesture = 19, NSEventTypeEndGesture = 20, NSEventTypeSmartMagnify = 32,
    NSEventTypeQuickLook = 33, NSEventTypePressure = 34, NSEventTypeDirectTouch = 37, NSEventTypeChangeMode = 38,
};
typedef NS_OPTIONS(unsigned long long, NSEventMask) { NSEventMaskAny = ~0ULL };
#define NSEventMaskFromType(type) (1ULL << (type))
typedef NS_OPTIONS(NSUInteger, NSEventModifierFlags) {
    NSEventModifierFlagCapsLock = 1 << 16, NSEventModifierFlagShift = 1 << 17, NSEventModifierFlagControl = 1 << 18,
    NSEventModifierFlagOption = 1 << 19, NSEventModifierFlagCommand = 1 << 20, NSEventModifierFlagNumericPad = 1 << 21,
    NSEventModifierFlagHelp = 1 << 22, NSEventModifierFlagFunction = 1 << 23,
    NSEventModifierFlagDeviceIndependentFlagsMask = 0xffff0000UL,
};
typedef NS_OPTIONS(NSUInteger, NSEventPhase) {
    NSEventPhaseNone = 0, NSEventPhaseBegan = 1, NSEventPhaseStationary = 2, NSEventPhaseChanged = 4,
    NSEventPhaseEnded = 8, NSEventPhaseCancelled = 16, NSEventPhaseMayBegin = 32,
};
typedef NS_OPTIONS(NSUInteger, NSWindowStyleMask) {
    NSWindowStyleMaskBorderless = 0, NSWindowStyleMaskTitled = 1, NSWindowStyleMaskClosable = 2,
    NSWindowStyleMaskMiniaturizable = 4, NSWindowStyleMaskResizable = 8, NSWindowStyleMaskTexturedBackground = 256,
    NSWindowStyleMaskUnifiedTitleAndToolbar = 4096, NSWindowStyleMaskFullScreen = 16384,
    NSWindowStyleMaskFullSizeContentView = 32768, NSWindowStyleMaskUtilityWindow = 16, NSWindowStyleMaskDocModalWindow = 64,
    NSWindowStyleMaskNonactivatingPanel = 128, NSWindowStyleMaskHUDWindow = 8192,
};
typedef NS_ENUM(NSUInteger, NSBackingStoreType) { NSBackingStoreRetained = 0, NSBackingStoreNonretained = 1, NSBackingStoreBuffered = 2 };
typedef NS_ENUM(NSInteger, NSWindowOrderingMode) { NSWindowAbove = 1, NSWindowBelow = -1, NSWindowOut = 0 };
typedef NSInteger NSWindowLevel;
typedef NS_OPTIONS(NSUInteger, NSAutoresizingMaskOptions) {
    NSViewNotSizable = 0, NSViewMinXMargin = 1, NSViewWidthSizable = 2, NSViewMaxXMargin = 4,
    NSViewMinYMargin = 8, NSViewHeightSizable = 16, NSViewMaxYMargin = 32,
};
typedef NS_ENUM(NSInteger, NSApplicationActivationPolicy) {
    NSApplicationActivationPolicyRegular = 0, NSApplicationActivationPolicyAccessory = 1, NSApplicationActivationPolicyProhibited = 2,
};
typedef NS_ENUM(NSUInteger, NSApplicationTerminateReply) { NSTerminateCancel = 0, NSTerminateNow = 1, NSTerminateLater = 2 };
typedef NS_OPTIONS(NSUInteger, NSWindowOcclusionState) { NSWindowOcclusionStateVisible = 1UL << 1 };
typedef NS_OPTIONS(NSUInteger, NSTrackingAreaOptions) {
    NSTrackingMouseEnteredAndExited = 0x01, NSTrackingMouseMoved = 0x02, NSTrackingCursorUpdate = 0x04,
    NSTrackingActiveWhenFirstResponder = 0x10, NSTrackingActiveInKeyWindow = 0x20, NSTrackingActiveInActiveApp = 0x40,
    NSTrackingActiveAlways = 0x80, NSTrackingAssumeInside = 0x100, NSTrackingInVisibleRect = 0x200,
    NSTrackingEnabledDuringMouseDrag = 0x400,
};
typedef NSInteger NSModalResponse;
typedef NSString *NSAppearanceName;
typedef NSString *NSPasteboardType;
typedef NSString *NSPasteboardName;
typedef NSString *NSAccessibilityRole;
typedef CGFloat NSFontWeight;

/* ---- the base of every class here: unknown messages are logged once and answer nil/0 ---- */
@interface HKShimObject : NSObject
@end
/* IOSurfaces set as layer contents shown as images of their pixels (iOS; HKSurface.m). */
void hk_surfaces_as_images(void);
/* CALayer autoresizing masks honoured (iOS ignores them; HKSurface.m). */
void hk_layer_autoresizing(void);
void hk_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Adds the same "say once, answer 0" fallback to a class that cannot inherit from HKShimObject (NSView descends from
   NSResponder, which must descend from NSObject directly for apps that test it). */
void hk_add_fallback(Class cls);

@class NSWindow, NSView, NSEvent, NSScreen, NSMenu, NSMenuItem, NSColor, NSFont, NSImage, NSCursor, NSAppearance,
    NSTrackingArea, NSTextInputContext, NSGraphicsContext, NSColorSpace, NSPasteboard;

/* ---- the responder chain ---- */
@interface NSResponder : NSObject
@property(nonatomic, assign) NSResponder *nextResponder;
@property(nonatomic, strong) NSMenu *menu;
@property(nonatomic, strong) NSUserActivity *userActivity;
- (BOOL)acceptsFirstResponder;
- (BOOL)becomeFirstResponder;
- (BOOL)resignFirstResponder;
- (BOOL)tryToPerform:(SEL)action with:(id)object;
- (void)mouseDown:(NSEvent *)event;
- (void)mouseUp:(NSEvent *)event;
- (void)mouseDragged:(NSEvent *)event;
- (void)mouseMoved:(NSEvent *)event;
- (void)mouseEntered:(NSEvent *)event;
- (void)mouseExited:(NSEvent *)event;
- (void)rightMouseDown:(NSEvent *)event;
- (void)rightMouseUp:(NSEvent *)event;
- (void)rightMouseDragged:(NSEvent *)event;
- (void)otherMouseDown:(NSEvent *)event;
- (void)otherMouseUp:(NSEvent *)event;
- (void)otherMouseDragged:(NSEvent *)event;
- (void)scrollWheel:(NSEvent *)event;
- (void)keyDown:(NSEvent *)event;
- (void)keyUp:(NSEvent *)event;
- (void)flagsChanged:(NSEvent *)event;
- (void)cursorUpdate:(NSEvent *)event;
- (void)magnifyWithEvent:(NSEvent *)event;
- (void)noResponderFor:(SEL)selector;
- (BOOL)performKeyEquivalent:(NSEvent *)event;
- (void)interpretKeyEvents:(NSArray<NSEvent *> *)events;
- (void)doCommandBySelector:(SEL)selector;
- (id)validRequestorForSendType:(NSString *)sendType returnType:(NSString *)returnType;
@end

/* ---- views: always layer-backed; flipped or not as the subclass says ---- */
@interface NSView : NSResponder
- (instancetype)initWithFrame:(NSRect)frame;
@property(nonatomic) NSRect frame;
@property(nonatomic) NSRect bounds;
@property(nonatomic, readonly, assign) NSView *superview;
@property(nonatomic, copy) NSArray<NSView *> *subviews;
@property(nonatomic, readonly, assign) NSWindow *window;
@property(nonatomic, strong) CALayer *layer;
@property(nonatomic) BOOL wantsLayer;
@property(nonatomic, getter=isHidden) BOOL hidden;
@property(nonatomic) CGFloat alphaValue;
@property(nonatomic) NSAutoresizingMaskOptions autoresizingMask;
@property(nonatomic) BOOL autoresizesSubviews;
@property(nonatomic) BOOL postsFrameChangedNotifications;
@property(nonatomic) BOOL postsBoundsChangedNotifications;
@property(nonatomic, copy) NSString *identifier;
@property(nonatomic, copy) NSString *toolTip;
@property(nonatomic, strong) NSAppearance *appearance;
@property(nonatomic, readonly) NSAppearance *effectiveAppearance;
@property(nonatomic) BOOL needsDisplay;
@property(nonatomic) BOOL needsLayout;
@property(nonatomic) BOOL translatesAutoresizingMaskIntoConstraints;
@property(nonatomic, readonly) NSArray<NSTrackingArea *> *trackingAreas;
- (BOOL)isFlipped;
- (BOOL)isOpaque;
- (void)addSubview:(NSView *)view;
- (void)addSubview:(NSView *)view positioned:(NSWindowOrderingMode)place relativeTo:(NSView *)other;
- (void)removeFromSuperview;
- (void)replaceSubview:(NSView *)old with:(NSView *)replacement;
- (BOOL)isDescendantOf:(NSView *)view;
- (void)setFrameOrigin:(NSPoint)origin;
- (void)setFrameSize:(NSSize)size;
- (void)setBoundsOrigin:(NSPoint)origin;
- (void)setBoundsSize:(NSSize)size;
- (void)resizeSubviewsWithOldSize:(NSSize)size;
- (void)resizeWithOldSuperviewSize:(NSSize)size;
- (NSPoint)convertPoint:(NSPoint)point fromView:(NSView *)view;
- (NSPoint)convertPoint:(NSPoint)point toView:(NSView *)view;
- (NSRect)convertRect:(NSRect)rect fromView:(NSView *)view;
- (NSRect)convertRect:(NSRect)rect toView:(NSView *)view;
- (NSSize)convertSize:(NSSize)size fromView:(NSView *)view;
- (NSSize)convertSize:(NSSize)size toView:(NSView *)view;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSRect)convertRectFromBacking:(NSRect)rect;
- (NSPoint)convertPointToBacking:(NSPoint)point;
- (NSSize)convertSizeToBacking:(NSSize)size;
- (NSRect)convertRectToLayer:(NSRect)rect;
- (NSView *)hitTest:(NSPoint)point;
- (void)setNeedsDisplayInRect:(NSRect)rect;
- (void)display;
- (void)displayIfNeeded;
- (void)drawRect:(NSRect)rect;
- (BOOL)wantsUpdateLayer;
- (void)updateLayer;
- (CALayer *)makeBackingLayer;
- (void)layout;
- (void)layoutSubtreeIfNeeded;
- (void)viewWillMoveToWindow:(NSWindow *)window;
- (void)viewDidMoveToWindow;
- (void)viewWillMoveToSuperview:(NSView *)superview;
- (void)viewDidMoveToSuperview;
- (void)viewDidChangeBackingProperties;
- (void)viewDidChangeEffectiveAppearance;
- (void)didAddSubview:(NSView *)view;
- (void)willRemoveSubview:(NSView *)view;
- (void)addTrackingArea:(NSTrackingArea *)area;
- (void)removeTrackingArea:(NSTrackingArea *)area;
- (void)updateTrackingAreas;
- (NSRect)visibleRect;
- (BOOL)isHiddenOrHasHiddenAncestor;
- (NSTextInputContext *)inputContext;
- (BOOL)mouse:(NSPoint)point inRect:(NSRect)rect;
- (void)setFrame:(NSRect)frame;
@end

/* ---- windows ---- */
@interface NSWindow : NSResponder
- (instancetype)initWithContentRect:(NSRect)rect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing defer:(BOOL)defer;
- (instancetype)initWithContentRect:(NSRect)rect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing defer:(BOOL)defer screen:(NSScreen *)screen;
@property(nonatomic, strong) NSView *contentView;
@property(nonatomic, weak) id delegate;
@property(nonatomic, copy) NSString *title;
@property(nonatomic) NSWindowStyleMask styleMask;
@property(nonatomic) NSWindowLevel level;
@property(nonatomic, readonly) NSRect frame;
@property(nonatomic, readonly) NSInteger windowNumber;
@property(nonatomic, readonly, getter=isVisible) BOOL visible;
@property(nonatomic, readonly, getter=isKeyWindow) BOOL keyWindow;
@property(nonatomic, readonly, getter=isMainWindow) BOOL mainWindow;
@property(nonatomic, readonly) NSResponder *firstResponder;
@property(nonatomic, readonly) CGFloat backingScaleFactor;
@property(nonatomic, readonly) NSScreen *screen;
@property(nonatomic, strong) NSColor *backgroundColor;
@property(nonatomic, getter=isOpaque) BOOL opaque;
@property(nonatomic) BOOL hasShadow;
@property(nonatomic) CGFloat alphaValue;
@property(nonatomic) BOOL ignoresMouseEvents;
@property(nonatomic) BOOL acceptsMouseMovedEvents;
@property(nonatomic) BOOL releasedWhenClosed;
@property(nonatomic) NSUInteger collectionBehavior;
@property(nonatomic) NSSize minSize, maxSize, contentMinSize, contentMaxSize;
@property(nonatomic, weak) NSWindow *parentWindow;
@property(nonatomic, readonly) NSArray<NSWindow *> *childWindows;
@property(nonatomic, readonly) NSWindowOcclusionState occlusionState;
@property(nonatomic, strong) NSAppearance *appearance;
@property(nonatomic, readonly) NSAppearance *effectiveAppearance;
@property(nonatomic, readonly) NSEvent *currentEvent;
- (void)setFrame:(NSRect)frame display:(BOOL)display;
- (void)setFrame:(NSRect)frame display:(BOOL)display animate:(BOOL)animate;
- (void)setFrameOrigin:(NSPoint)origin;
- (void)setContentSize:(NSSize)size;
- (NSRect)contentRectForFrameRect:(NSRect)frame;
- (NSRect)frameRectForContentRect:(NSRect)content;
+ (NSRect)contentRectForFrameRect:(NSRect)frame styleMask:(NSWindowStyleMask)style;
+ (NSRect)frameRectForContentRect:(NSRect)content styleMask:(NSWindowStyleMask)style;
- (void)makeKeyAndOrderFront:(id)sender;
- (void)makeKeyWindow;
- (void)makeMainWindow;
- (void)orderFront:(id)sender;
- (void)orderBack:(id)sender;
- (void)orderOut:(id)sender;
- (void)orderFrontRegardless;
- (void)orderWindow:(NSWindowOrderingMode)place relativeTo:(NSInteger)other;
- (void)close;
- (void)performClose:(id)sender;
- (BOOL)makeFirstResponder:(NSResponder *)responder;
- (void)sendEvent:(NSEvent *)event;
- (BOOL)canBecomeKeyWindow;
- (BOOL)canBecomeMainWindow;
- (void)becomeKeyWindow;
- (void)resignKeyWindow;
- (void)becomeMainWindow;
- (void)resignMainWindow;
- (NSPoint)convertPointToScreen:(NSPoint)point;
- (NSPoint)convertPointFromScreen:(NSPoint)point;
- (NSRect)convertRectToScreen:(NSRect)rect;
- (NSRect)convertRectFromScreen:(NSRect)rect;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSPoint)mouseLocationOutsideOfEventStream;
- (void)addChildWindow:(NSWindow *)child ordered:(NSWindowOrderingMode)place;
- (void)removeChildWindow:(NSWindow *)child;
- (void)invalidateShadow;
- (void)displayIfNeeded;
- (void)display;
- (BOOL)isOnActiveSpace;
- (BOOL)isZoomed;
- (BOOL)isMiniaturized;
- (BOOL)inLiveResize;
- (void)center;
- (void)toggleFullScreen:(id)sender;
- (CALayer *)hk_rootLayer; /* the layer the presenter shows: the content view's layer tree */
@end

/* A window that floats over others (inspectors, palettes, quick-entry windows). */
@interface NSPanel : NSWindow
@property(getter=isFloatingPanel) BOOL floatingPanel;
@property BOOL becomesKeyOnlyIfNeeded;
@property BOOL worksWhenModal;
@end

/* ---- events ---- */
@interface NSEvent : NSObject <NSCopying>
@property(readonly) NSEventType type;
@property(readonly) NSEventModifierFlags modifierFlags;
@property(readonly) NSTimeInterval timestamp;
@property(readonly, weak) NSWindow *window;
@property(readonly) NSInteger windowNumber;
@property(readonly) NSPoint locationInWindow;
@property(readonly) NSInteger clickCount;
@property(readonly) NSInteger buttonNumber;
@property(readonly) NSInteger eventNumber;
@property(readonly) float pressure;
@property(readonly) CGFloat deltaX, deltaY, deltaZ;
@property(readonly) CGFloat scrollingDeltaX, scrollingDeltaY;
@property(readonly) BOOL hasPreciseScrollingDeltas;
@property(readonly) NSEventPhase phase, momentumPhase;
@property(readonly) BOOL isDirectionInvertedFromDevice;
@property(readonly) unsigned short keyCode;
@property(readonly, copy) NSString *characters;
@property(readonly, copy) NSString *charactersIgnoringModifiers;
@property(readonly, getter=isARepeat) BOOL ARepeat;
@property(readonly) short subtype;
@property(readonly) NSInteger data1, data2;
@property(readonly) CGFloat magnification;
@property(readonly) NSTrackingArea *trackingArea;
+ (NSEvent *)otherEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                      timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context subtype:(short)subtype
                          data1:(NSInteger)d1 data2:(NSInteger)d2;
+ (NSEvent *)mouseEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                      timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context eventNumber:(NSInteger)en
                     clickCount:(NSInteger)clicks pressure:(float)pressure;
+ (NSEvent *)keyEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                    timestamp:(NSTimeInterval)time windowNumber:(NSInteger)wn context:(id)context characters:(NSString *)chars
  charactersIgnoringModifiers:(NSString *)unmod isARepeat:(BOOL)repeat keyCode:(unsigned short)code;
+ (NSPoint)mouseLocation;
+ (NSUInteger)pressedMouseButtons;
+ (NSEventModifierFlags)modifierFlags;
+ (NSTimeInterval)doubleClickInterval;
+ (NSTimeInterval)keyRepeatDelay;
+ (NSTimeInterval)keyRepeatInterval;
+ (id)addLocalMonitorForEventsMatchingMask:(NSEventMask)mask handler:(NSEvent * (^)(NSEvent *))handler;
+ (id)addGlobalMonitorForEventsMatchingMask:(NSEventMask)mask handler:(void (^)(NSEvent *))handler;
+ (void)removeMonitor:(id)monitor;
/* Henge: an event with every field given (the presenter's input). */
+ (NSEvent *)hk_eventWithType:(NSEventType)type window:(NSWindow *)window location:(NSPoint)location
                    modifiers:(NSEventModifierFlags)flags clickCount:(NSInteger)clicks button:(NSInteger)button;
- (NSEvent *)hk_withDeltaX:(CGFloat)dx deltaY:(CGFloat)dy precise:(BOOL)precise phase:(NSEventPhase)phase;
- (NSEvent *)hk_withKeyCode:(unsigned short)code characters:(NSString *)chars unmodified:(NSString *)unmod repeat:(BOOL)repeat;
/* NSApplication's local monitors run over an event first; NO means one of them swallowed it. */
+ (NSEvent *)hk_runLocalMonitors:(NSEvent *)event;
@end

/* ---- the application ---- */
@interface NSApplication : NSResponder
+ (instancetype)sharedApplication;
@property(nonatomic, weak) id delegate;
@property(nonatomic, strong) NSMenu *mainMenu;
@property(nonatomic, strong) NSMenu *windowsMenu, *servicesMenu, *helpMenu;
@property(nonatomic, readonly) NSArray<NSWindow *> *windows;
@property(nonatomic, readonly) NSWindow *keyWindow;
@property(nonatomic, readonly) NSWindow *mainWindow;
@property(nonatomic, readonly) NSWindow *modalWindow;
@property(nonatomic, readonly) NSEvent *currentEvent;
@property(nonatomic, readonly, getter=isActive) BOOL active;
@property(nonatomic, readonly, getter=isRunning) BOOL running;
@property(nonatomic, strong) NSAppearance *appearance;
@property(nonatomic, readonly) NSAppearance *effectiveAppearance;
@property(nonatomic) NSUInteger presentationOptions;
@property(nonatomic, strong) NSImage *applicationIconImage;
- (void)run;
- (void)stop:(id)sender;
- (void)terminate:(id)sender;
- (void)finishLaunching;
- (NSEvent *)nextEventMatchingMask:(NSEventMask)mask untilDate:(NSDate *)date inMode:(NSRunLoopMode)mode dequeue:(BOOL)dequeue;
- (void)discardEventsMatchingMask:(NSEventMask)mask beforeEvent:(NSEvent *)event;
- (void)postEvent:(NSEvent *)event atStart:(BOOL)atStart;
- (void)sendEvent:(NSEvent *)event;
- (BOOL)sendAction:(SEL)action to:(id)target from:(id)sender;
- (id)targetForAction:(SEL)action;
- (id)targetForAction:(SEL)action to:(id)target from:(id)sender;
- (void)activateIgnoringOtherApps:(BOOL)flag;
- (void)activate;
- (void)deactivate;
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)policy;
- (NSApplicationActivationPolicy)activationPolicy;
- (NSWindow *)windowWithWindowNumber:(NSInteger)number;
- (NSArray<NSWindow *> *)orderedWindows;
- (void)updateWindows;
- (void)hide:(id)sender;
- (void)unhide:(id)sender;
- (BOOL)isHidden;
- (NSInteger)requestUserAttention:(NSInteger)type;
- (NSModalResponse)runModalForWindow:(NSWindow *)window;
- (void)stopModal;
- (void)abortModal;
- (void)replyToApplicationShouldTerminate:(BOOL)reply;
- (void)registerServicesMenuSendTypes:(NSArray *)send returnTypes:(NSArray *)ret;
@end
extern NSApplication *NSApp;
int NSApplicationMain(int argc, const char *argv[]);

/* ---- the screen: one, the host's ---- */
@interface NSScreen : NSObject
+ (NSScreen *)mainScreen;
+ (NSArray<NSScreen *> *)screens;
+ (NSScreen *)deepestScreen;
+ (BOOL)screensHaveSeparateSpaces;
@property(readonly) NSRect frame;
@property(readonly) NSRect visibleFrame;
@property(readonly) CGFloat backingScaleFactor;
@property(readonly) NSDictionary *deviceDescription;
@property(readonly, copy) NSString *localizedName;
@property(readonly) NSColorSpace *colorSpace;
@property(readonly) NSInteger maximumFramesPerSecond;
@property(readonly) CGFloat maximumExtendedDynamicRangeColorComponentValue;
@property(readonly) CGFloat maximumPotentialExtendedDynamicRangeColorComponentValue;
@property(readonly) CGFloat maximumReferenceExtendedDynamicRangeColorComponentValue;
@property(readonly) NSTimeInterval minimumRefreshInterval, maximumRefreshInterval;
@property(readonly) NSEdgeInsets safeAreaInsets;
- (NSRect)convertRectToBacking:(NSRect)rect;
@end

/* The app's bundle (the loader adopts it for the app's own code only): what Henge's AppKit reads for the app. */
NSBundle *hk_app_bundle(void);

/* ---- the presenter: who shows the windows and feeds input (iOS app, or the Mac runner) ---- */
typedef struct HKPresenter {
    /* The screen in points, and its scale. */
    CGSize screen_size;
    CGFloat scale;
    /* A window was ordered in (visible = 1), ordered out or closed (0), or changed size or its layer tree. Called on
       the main thread. */
    void (*window_changed)(NSWindow *window, int visible);
} HKPresenter;
void hk_set_presenter(const HKPresenter *presenter);
const HKPresenter *hk_presenter(void);

/* Input from the presenter, in the window's coordinates (points, origin bottom left as AppKit's). Any thread: the
   event is posted to the application's queue. */
/* Menus that pop up, drawn and tracked by Henge (HKMenu.m). */
void hk_menu_context(NSMenu *menu, NSEvent *event, NSView *view);
BOOL hk_menu_positioned(NSMenu *menu, NSMenuItem *item, NSPoint location, NSView *view);
void hk_menu_cell_click(id cell, NSRect frame, NSView *view);
void hk_menu_cancel(NSMenu *menu);
NSMenuItem *hk_menu_highlighted(NSMenu *menu);
void hk_post_mouse(NSWindow *window, NSEventType type, CGPoint location, NSEventModifierFlags flags, NSInteger clicks);
void hk_post_scroll(NSWindow *window, CGPoint location, CGFloat dx, CGFloat dy, NSEventPhase phase);
void hk_post_key(NSWindow *window, NSEventType type, unsigned short keycode, NSString *chars, NSString *unmod, NSEventModifierFlags flags);

/* The application's windows that are on screen, back to front. */
NSArray<NSWindow *> *hk_visible_windows(void);

/* ---- binding: what Henge's AppKit answers for a symbol a program imports from AppKit (and the Mac-only C
   libraries handled here: Carbon, ApplicationServices, CoreVideo's display link, CoreGraphics's displays) ---- */
void *hk_lookup(const char *install_name, const char *symbol);

/* The table of every C symbol (function or constant) these files define, generated at build time
   (tools/hk_symbols.sh) so lookups do not depend on the host binary's symbol table. */
typedef struct HKSymbol {
    const char *name; /* with the leading underscore, as Mach-O names it */
    void *address;
} HKSymbol;
extern const HKSymbol hk_symbols[];
extern const int hk_nsymbols;

/* ================= Added for HKDraw.m, HKMisc.m, HKControls.m, HKConstants.m, HKMacC.m =================
   What those files share. Fonts are CoreText's own objects, as on the Mac (NSFont is toll-free bridged with
   CTFontRef, NSFontDescriptor with CTFontDescriptorRef): NSFont's class methods answer CTFonts, and the class those
   have at run time is given NSFont's instance methods. */
typedef NS_ENUM(NSUInteger, NSCompositingOperation) {
    NSCompositingOperationClear = 0, NSCompositingOperationCopy = 1, NSCompositingOperationSourceOver = 2,
    NSCompositingOperationSourceIn = 3, NSCompositingOperationSourceOut = 4, NSCompositingOperationSourceAtop = 5,
    NSCompositingOperationDestinationOver = 6, NSCompositingOperationDestinationIn = 7,
    NSCompositingOperationDestinationOut = 8, NSCompositingOperationDestinationAtop = 9, NSCompositingOperationXOR = 10,
    NSCompositingOperationPlusDarker = 11, NSCompositingOperationPlusLighter = 13, NSCompositingOperationMultiply = 14,
};
typedef NS_ENUM(NSUInteger, NSControlSize) { NSControlSizeRegular = 0, NSControlSizeSmall = 1, NSControlSizeMini = 2, NSControlSizeLarge = 3 };
typedef NSInteger NSControlStateValue; /* off 0, on 1, mixed -1 */
typedef NSString *NSColorSpaceName;
typedef NSString *NSImageName;
typedef NSString *NSUserInterfaceItemIdentifier;

@interface NSColorSpace : HKShimObject
+ (NSColorSpace *)sRGBColorSpace;
+ (NSColorSpace *)extendedSRGBColorSpace;
+ (NSColorSpace *)displayP3ColorSpace;
+ (NSColorSpace *)genericRGBColorSpace;
+ (NSColorSpace *)deviceRGBColorSpace;
+ (NSColorSpace *)genericGrayColorSpace;
+ (NSColorSpace *)deviceGrayColorSpace;
- (instancetype)initWithCGColorSpace:(CGColorSpaceRef)space;
@property(readonly) CGColorSpaceRef CGColorSpace;
@property(readonly) NSInteger colorSpaceModel; /* NSColorSpaceModel: gray 0, RGB 1, CMYK 2... */
@property(readonly) NSInteger numberOfColorComponents;
@end

/* iOS has classes of its own called NSColor (UIKitCore) and NSFont (UIFoundation): these go by other names in the
   Objective-C runtime, and programs reach them through hk_lookup, which maps AppKit's names onto them. */
__attribute__((objc_runtime_name("HKColor"))) @interface NSColor : HKShimObject <NSCopying>
+ (NSColor *)colorWithCGColor:(CGColorRef)color;
+ (NSColor *)colorWithSRGBRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (NSColor *)colorWithRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (NSColor *)colorWithDeviceRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (NSColor *)colorWithCalibratedRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (NSColor *)colorWithWhite:(CGFloat)w alpha:(CGFloat)a;
+ (NSColor *)colorWithColorSpace:(NSColorSpace *)space components:(const CGFloat *)components count:(NSInteger)count;
+ (NSColor *)blackColor;
+ (NSColor *)whiteColor;
+ (NSColor *)clearColor;
+ (NSColor *)textColor;
+ (NSColor *)labelColor;
+ (NSColor *)secondaryLabelColor;
+ (NSColor *)controlTextColor;
+ (NSColor *)disabledControlTextColor;
+ (NSColor *)controlColor;
+ (NSColor *)controlBackgroundColor;
+ (NSColor *)textBackgroundColor;
+ (NSColor *)windowBackgroundColor;
+ (NSColor *)controlAccentColor;
+ (NSColor *)separatorColor;
@property(readonly) CGColorRef CGColor;
@property(readonly) NSColorSpace *colorSpace;
@property(readonly) NSInteger numberOfComponents;
@property(readonly) CGFloat redComponent, greenComponent, blueComponent, alphaComponent, whiteComponent;
- (void)getComponents:(CGFloat *)components;
- (void)getRed:(CGFloat *)r green:(CGFloat *)g blue:(CGFloat *)b alpha:(CGFloat *)a;
- (NSColor *)colorUsingColorSpace:(NSColorSpace *)space;
- (NSColor *)colorUsingColorSpaceName:(NSColorSpaceName)name;
- (NSColor *)colorWithAlphaComponent:(CGFloat)alpha;
- (NSColor *)blendedColorWithFraction:(CGFloat)fraction ofColor:(NSColor *)color;
- (void)set;
- (void)setFill;
- (void)setStroke;
@end

__attribute__((objc_runtime_name("HKFont"))) @interface NSFont : HKShimObject
+ (NSFont *)systemFontOfSize:(CGFloat)size;
+ (NSFont *)systemFontOfSize:(CGFloat)size weight:(NSFontWeight)weight;
+ (NSFont *)boldSystemFontOfSize:(CGFloat)size;
+ (NSFont *)monospacedSystemFontOfSize:(CGFloat)size weight:(NSFontWeight)weight;
+ (NSFont *)fontWithName:(NSString *)name size:(CGFloat)size;
+ (NSFont *)labelFontOfSize:(CGFloat)size;
+ (NSFont *)menuFontOfSize:(CGFloat)size;
+ (NSFont *)messageFontOfSize:(CGFloat)size;
+ (NSFont *)toolTipsFontOfSize:(CGFloat)size;
+ (CGFloat)systemFontSize;
+ (CGFloat)smallSystemFontSize;
+ (CGFloat)labelFontSize;
+ (CGFloat)systemFontSizeForControlSize:(NSControlSize)size;
@end
/* Answered by the CTFont class at run time (HKDraw.m), not by NSFont itself. */
@interface NSFont (HKBridged)
@property(readonly) CGFloat pointSize, ascender, descender, leading, capHeight, xHeight;
@property(readonly, copy) NSString *fontName, *familyName, *displayName;
@property(readonly) NSRect boundingRectForFont;
@property(readonly, getter=isFixedPitch) BOOL fixedPitch;
@end
/* Gives the class of a CTFont or CTFontDescriptor (whatever CoreFoundation bridges it to in this process) NSFont's
   or NSFontDescriptor's instance methods, if it lacks them; answers obj. */
id hk_font_adopt(id obj);

@interface NSGraphicsContext : HKShimObject
+ (NSGraphicsContext *)graphicsContextWithCGContext:(CGContextRef)context flipped:(BOOL)flipped;
+ (NSGraphicsContext *)currentContext; /* per thread */
+ (void)setCurrentContext:(NSGraphicsContext *)context;
+ (void)saveGraphicsState; /* pushes the current context (nil too) and its state */
+ (void)restoreGraphicsState;
@property(readonly) CGContextRef CGContext;
@property(readonly, getter=isFlipped) BOOL flipped;
- (void)saveGraphicsState;
- (void)restoreGraphicsState;
@end

@interface NSImage : HKShimObject <NSCopying>
+ (NSImage *)imageNamed:(NSImageName)name;
+ (NSImage *)imageWithSize:(NSSize)size flipped:(BOOL)flipped drawingHandler:(BOOL (^)(NSRect dstRect))handler;
- (instancetype)initWithSize:(NSSize)size;
- (instancetype)initWithCGImage:(CGImageRef)image size:(NSSize)size;
- (instancetype)initWithData:(NSData *)data;
- (instancetype)initWithContentsOfFile:(NSString *)path;
@property NSSize size;
@property(getter=isTemplate) BOOL template;
@property(copy) NSString *accessibilityDescription;
@property(readonly, getter=isValid) BOOL valid;
- (CGImageRef)CGImageForProposedRect:(NSRect *)rect context:(NSGraphicsContext *)context hints:(NSDictionary *)hints;
- (void)drawInRect:(NSRect)rect;
- (void)drawInRect:(NSRect)dst fromRect:(NSRect)src operation:(NSCompositingOperation)op fraction:(CGFloat)fraction;
- (void)drawInRect:(NSRect)dst fromRect:(NSRect)src operation:(NSCompositingOperation)op fraction:(CGFloat)fraction
    respectFlipped:(BOOL)respect hints:(NSDictionary *)hints;
@end

@interface NSAppearance : HKShimObject
+ (NSAppearance *)appearanceNamed:(NSAppearanceName)name;
+ (NSAppearance *)currentAppearance; /* per thread */
+ (void)setCurrentAppearance:(NSAppearance *)appearance;
+ (NSAppearance *)currentDrawingAppearance;
@property(readonly, copy) NSAppearanceName name;
- (NSAppearanceName)bestMatchFromAppearancesWithNames:(NSArray<NSAppearanceName> *)names;
- (void)performAsCurrentDrawingAppearance:(void (^)(void))block;
@end

@interface NSCursor : HKShimObject
+ (NSCursor *)arrowCursor;
+ (NSCursor *)IBeamCursor;
+ (NSCursor *)pointingHandCursor;
+ (NSCursor *)currentCursor; /* the one last set */
- (instancetype)initWithImage:(NSImage *)image hotSpot:(NSPoint)hotSpot;
@property(readonly, strong) NSImage *image;
@property(readonly) NSPoint hotSpot;
- (void)set;
@end

@interface NSMenuItem : HKShimObject <NSCopying>
+ (NSMenuItem *)separatorItem;
- (instancetype)initWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key;
@property(copy) NSString *title;
@property SEL action;
@property(weak) id target;
@property NSInteger tag;
@property(copy) NSString *keyEquivalent;
@property NSEventModifierFlags keyEquivalentModifierMask;
@property(strong) NSMenu *submenu;
@property(readonly) BOOL hasSubmenu;
@property(weak) NSMenu *menu;
@property(strong) id representedObject;
@property NSControlStateValue state;
@property(getter=isEnabled) BOOL enabled;
@property(getter=isHidden) BOOL hidden;
@property(readonly, getter=isSeparatorItem) BOOL separatorItem;
@property(strong) NSImage *image;
@property(strong) NSView *view;
@end

@interface NSMenu : HKShimObject <NSCopying>
- (instancetype)initWithTitle:(NSString *)title;
@property(copy) NSString *title;
@property(copy) NSArray<NSMenuItem *> *itemArray;
@property(readonly) NSInteger numberOfItems;
@property(weak) id delegate;
@property BOOL autoenablesItems;
@property(weak) NSMenu *supermenu;
- (void)addItem:(NSMenuItem *)item;
- (NSMenuItem *)addItemWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key;
- (void)insertItem:(NSMenuItem *)item atIndex:(NSInteger)index;
- (void)removeItem:(NSMenuItem *)item;
- (void)removeItemAtIndex:(NSInteger)index;
- (void)removeAllItems;
- (NSMenuItem *)insertItemWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key atIndex:(NSInteger)index;
- (NSMenuItem *)itemAtIndex:(NSInteger)index;
- (NSMenuItem *)itemWithTitle:(NSString *)title;
- (NSMenuItem *)itemWithTag:(NSInteger)tag;
- (NSInteger)indexOfItem:(NSMenuItem *)item;
- (NSInteger)indexOfItemWithTitle:(NSString *)title;
- (NSInteger)indexOfItemWithTag:(NSInteger)tag;
- (NSInteger)indexOfItemWithRepresentedObject:(id)object;
- (NSInteger)indexOfItemWithTarget:(id)target andAction:(SEL)action;
- (void)update;
/* Before the menu shows: the delegate's menuNeedsUpdate:, then update. */
- (void)hk_prepare;
/* Finds the enabled item whose key equivalent the key-down event is, and sends its action. */
- (BOOL)performKeyEquivalent:(NSEvent *)event;
- (void)performActionForItemAtIndex:(NSInteger)index;
@end

@interface NSTrackingArea : HKShimObject <NSCopying>
- (instancetype)initWithRect:(NSRect)rect options:(NSTrackingAreaOptions)options owner:(id)owner userInfo:(NSDictionary *)userInfo;
@property(readonly) NSRect rect;
@property(readonly) NSTrackingAreaOptions options;
@property(readonly, weak) id owner;
@property(readonly) NSDictionary *userInfo;
@end

@interface NSTextInputContext : HKShimObject
+ (NSTextInputContext *)currentInputContext;
- (instancetype)initWithClient:(id)client;
@property(readonly, weak) id client;
- (void)activate;
- (void)deactivate;
/* A key-down becomes insertText:replacementRange: or doCommandBySelector: on the client. */
- (BOOL)handleEvent:(NSEvent *)event;
- (void)discardMarkedText;
- (void)invalidateCharacterCoordinates;
@end

@interface NSCell : HKShimObject <NSCopying>
- (instancetype)initTextCell:(NSString *)string;
- (instancetype)initImageCell:(NSImage *)image;
@property(weak) NSView *controlView;
@property(strong) id objectValue;
@property(copy) NSString *stringValue;
@property(copy) NSString *title;
@property(strong) NSImage *image;
@property(strong) NSFont *font;
@property(getter=isEnabled) BOOL enabled;
@property NSControlStateValue state;
@property NSInteger tag;
@property(weak) id target;
@property SEL action;
@end

@interface NSControl : NSView
@property(weak) id target;
@property SEL action;
@property NSInteger tag;
@property(getter=isEnabled) BOOL enabled;
@property(copy) NSString *stringValue;
@property(strong) id objectValue;
@property int intValue;
@property NSInteger integerValue;
@property double doubleValue;
@property(strong) NSFont *font;
@property NSInteger alignment; /* NSTextAlignment */
@property NSControlSize controlSize;
@property(strong) NSCell *cell;
- (BOOL)sendAction:(SEL)action to:(id)target;
- (void)performClick:(id)sender;
- (void)sizeToFit;
@end

@interface NSButton : NSControl
+ (NSButton *)buttonWithTitle:(NSString *)title target:(id)target action:(SEL)action;
+ (NSButton *)buttonWithImage:(NSImage *)image target:(id)target action:(SEL)action;
@property(copy) NSString *title;
@property(strong) NSImage *image;
@property NSControlStateValue state;
@property NSUInteger bezelStyle;
@property(getter=isBordered) BOOL bordered;
@property(copy) NSString *keyEquivalent;
- (void)setButtonType:(NSUInteger)type;
@end

@interface NSStatusBarButton : NSButton
@property BOOL appearsDisabled;
@end

#endif
