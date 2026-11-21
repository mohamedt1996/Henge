/* Henge's AppKit, the rest of what a Mac app asks AppKit for: cursors, appearances, menus, pasteboards, the
   workspace and running applications, tracking areas, text input contexts, animation contexts and animations, the
   spell checker, controllers, and the panels and services iOS has no counterpart for (they answer as cancelled or
   empty). Pasteboards are in-process stores. */
#import "HKShim.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <os/lock.h>
#include <unistd.h>

/* What the program's objects may answer (AppKit's informal protocols); declared so they can be sent. */
@protocol HKAppKitClients
- (void)insertText:(id)string replacementRange:(NSRange)range;
- (void)insertText:(id)string;
- (BOOL)hasMarkedText;
- (void)unmarkText;
- (BOOL)validateMenuItem:(NSMenuItem *)item;
- (BOOL)validateUserInterfaceItem:(id)item;
- (void)menuNeedsUpdate:(NSMenu *)menu;
- (BOOL)menuHasKeyEquivalent:(NSMenu *)menu forEvent:(NSEvent *)event target:(id *)target action:(SEL *)action;
- (void)pasteboard:(NSPasteboard *)pasteboard provideDataForType:(NSString *)type;
- (void)pasteboard:(NSPasteboard *)pasteboard item:(id)item provideDataForType:(NSString *)type;
- (NSArray<NSString *> *)writableTypesForPasteboard:(NSPasteboard *)pasteboard;
- (id)pasteboardPropertyListForType:(NSString *)type;
+ (NSArray<NSString *> *)readableTypesForPasteboard:(NSPasteboard *)pasteboard;
- (id)initWithPasteboardPropertyList:(id)list ofType:(NSString *)type;
- (BOOL)animationShouldStart:(id)animation;
- (float)animation:(id)animation valueForProgress:(float)progress;
- (void)animation:(id)animation didReachProgressMark:(float)mark;
- (void)animationDidStop:(id)animation;
- (void)animationDidEnd:(id)animation;
- (id)touchBar:(id)touchBar makeItemForIdentifier:(NSString *)identifier;
- (void)speechSynthesizer:(id)sender didFinishSpeaking:(BOOL)finished;
- (void)sharingService:(id)service didFailToShareItems:(NSArray *)items error:(NSError *)error;
- (void)setWindowController:(id)controller;
- (void)popoverDidClose:(NSNotification *)notification;
@end

static void on_main(void (^block)(void)) { dispatch_async(dispatch_get_main_queue(), block); }

static NSImage *blank_icon(CGFloat side) {
    return [NSImage imageWithSize:NSMakeSize(side, side) flipped:NO drawingHandler:^BOOL(NSRect r) { return YES; }];
}

static NSError *unsupported_error(void) {
    return [NSError errorWithDomain:NSCocoaErrorDomain code:3328 /* NSFeatureUnsupportedError */ userInfo:nil];
}

/* ================================================================ cursors */

@implementation NSCursor {
    NSImage *_image;
    NSPoint _hotSpot;
    NSString *_hkName;
    BOOL _onEntered, _onExited;
}

static NSCursor *g_cursor;
static NSMutableArray<NSCursor *> *g_cursor_stack;
static NSInteger g_cursor_hides;
static BOOL g_cursor_hidden_until_moves;

static NSCursor *standard_cursor(NSString *name, NSPoint hot) {
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    static NSMutableDictionary *cache;
    os_unfair_lock_lock(&lock);
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSCursor *c = cache[name];
    if (!c) {
        c = [[NSCursor alloc] initWithImage:nil hotSpot:hot];
        c->_hkName = name;
        cache[name] = c;
    }
    os_unfair_lock_unlock(&lock);
    return c;
}
#define CURSOR(sel, x, y) \
    +(NSCursor *)sel { return standard_cursor(@ #sel, NSMakePoint(x, y)); }
CURSOR(arrowCursor, 4, 4) CURSOR(IBeamCursor, 8, 8) CURSOR(IBeamCursorForVerticalLayout, 8, 8)
CURSOR(pointingHandCursor, 6, 1) CURSOR(crosshairCursor, 8, 8) CURSOR(openHandCursor, 8, 8) CURSOR(closedHandCursor, 8, 8)
CURSOR(resizeLeftCursor, 8, 8) CURSOR(resizeRightCursor, 8, 8) CURSOR(resizeLeftRightCursor, 8, 8)
CURSOR(resizeUpCursor, 8, 8) CURSOR(resizeDownCursor, 8, 8) CURSOR(resizeUpDownCursor, 8, 8)
CURSOR(operationNotAllowedCursor, 4, 4) CURSOR(contextualMenuCursor, 4, 4) CURSOR(dragCopyCursor, 4, 4)
CURSOR(dragLinkCursor, 4, 4) CURSOR(disappearingItemCursor, 4, 4) CURSOR(columnResizeCursor, 8, 8)
CURSOR(rowResizeCursor, 8, 8) CURSOR(zoomInCursor, 8, 8) CURSOR(zoomOutCursor, 8, 8)
#undef CURSOR
+ (NSCursor *)columnResizeCursorInDirections:(NSUInteger)directions { return [self columnResizeCursor]; }
+ (NSCursor *)rowResizeCursorInDirections:(NSUInteger)directions { return [self rowResizeCursor]; }
+ (NSCursor *)frameResizeCursorFromPosition:(NSUInteger)position inDirections:(NSUInteger)directions {
    return [self resizeLeftRightCursor];
}
+ (NSCursor *)currentCursor { return g_cursor ?: [self arrowCursor]; }
+ (NSCursor *)currentSystemCursor { return [self currentCursor]; }
+ (void)hide { g_cursor_hides++; }
+ (void)unhide {
    if (g_cursor_hides > 0) g_cursor_hides--;
}
+ (void)setHiddenUntilMouseMoves:(BOOL)flag { g_cursor_hidden_until_moves = flag; }
+ (void)pop {
    if (!g_cursor_stack.count) return;
    g_cursor = g_cursor_stack.lastObject;
    [g_cursor_stack removeLastObject];
}

- (instancetype)initWithImage:(NSImage *)image hotSpot:(NSPoint)hotSpot {
    if ((self = [super init])) {
        _image = image;
        _hotSpot = hotSpot;
    }
    return self;
}
- (instancetype)init { return [self initWithImage:nil hotSpot:NSZeroPoint]; }
- (instancetype)initWithImage:(NSImage *)image foregroundColorHint:(NSColor *)fg backgroundColorHint:(NSColor *)bg
                      hotSpot:(NSPoint)hotSpot {
    return [self initWithImage:image hotSpot:hotSpot];
}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (NSImage *)image { return _image; }
- (NSPoint)hotSpot { return _hotSpot; }
- (void)set { g_cursor = self; }
- (void)push {
    if (!g_cursor_stack) g_cursor_stack = [NSMutableArray array];
    [g_cursor_stack addObject:[NSCursor currentCursor]];
    g_cursor = self;
}
- (void)pop { [NSCursor pop]; }
- (BOOL)isSetOnMouseEntered { return _onEntered; }
- (void)setOnMouseEntered:(BOOL)flag { _onEntered = flag; }
- (BOOL)isSetOnMouseExited { return _onExited; }
- (void)setOnMouseExited:(BOOL)flag { _onExited = flag; }
- (void)mouseEntered:(NSEvent *)event {
    if (_onEntered) [self set];
}
- (void)mouseExited:(NSEvent *)event {
    if (_onExited) [[NSCursor arrowCursor] set];
}
- (NSString *)accessibilityDescription { return _hkName; }
- (NSString *)description { return [NSString stringWithFormat:@"<NSCursor %p %@>", self, _hkName ?: @"custom"]; }
@end

/* ================================================================ appearances */

@implementation NSAppearance {
    NSString *_name;
    NSColor *_tint;
}

static NSArray<NSString *> *known_appearances(void) {
    return @[
        @"NSAppearanceNameAqua", @"NSAppearanceNameDarkAqua", @"NSAppearanceNameVibrantLight", @"NSAppearanceNameVibrantDark",
        @"NSAppearanceNameAccessibilityAqua", @"NSAppearanceNameAccessibilityDarkAqua",
        @"NSAppearanceNameAccessibilityVibrantLight", @"NSAppearanceNameAccessibilityVibrantDark"
    ];
}
+ (NSAppearance *)appearanceNamed:(NSAppearanceName)name {
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    static NSMutableDictionary *cache;
    if (![known_appearances() containsObject:name]) return nil;
    os_unfair_lock_lock(&lock);
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSAppearance *a = cache[name];
    if (!a) {
        a = [[NSAppearance alloc] init];
        a->_name = [name copy];
        cache[name] = a;
    }
    os_unfair_lock_unlock(&lock);
    return a;
}
- (instancetype)initWithAppearanceNamed:(NSAppearanceName)name bundle:(NSBundle *)bundle {
    NSAppearance *a = [NSAppearance appearanceNamed:name];
    if (!a || !(self = [super init])) return nil;
    _name = a->_name;
    return self;
}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self initWithAppearanceNamed:@"NSAppearanceNameAqua" bundle:nil]; }
+ (NSAppearance *)currentAppearance {
    return [NSThread currentThread].threadDictionary[@"HKCurrentAppearance"] ?: [self appearanceNamed:@"NSAppearanceNameAqua"];
}
+ (void)setCurrentAppearance:(NSAppearance *)appearance {
    NSMutableDictionary *d = [NSThread currentThread].threadDictionary;
    if (appearance) d[@"HKCurrentAppearance"] = appearance;
    else [d removeObjectForKey:@"HKCurrentAppearance"];
}
+ (NSAppearance *)currentDrawingAppearance { return [self currentAppearance]; }
- (NSAppearanceName)name { return _name; }
- (BOOL)allowsVibrancy { return NO; }
- (BOOL)supportsSecureCoding { return YES; }
- (NSColor *)tintColor { return _tint; }
- (NSAppearance *)appearanceByApplyingTintColor:(NSColor *)tint {
    NSAppearance *a = [[NSAppearance alloc] init];
    a->_name = _name;
    a->_tint = tint;
    return a;
}
- (NSAppearanceName)bestMatchFromAppearancesWithNames:(NSArray<NSAppearanceName> *)names {
    BOOL dark = [_name containsString:@"Dark"];
    NSArray *order = dark ? @[ @"NSAppearanceNameDarkAqua", @"NSAppearanceNameVibrantDark", @"NSAppearanceNameAccessibilityDarkAqua" ]
                          : @[ @"NSAppearanceNameAqua", @"NSAppearanceNameVibrantLight", @"NSAppearanceNameAccessibilityAqua" ];
    for (NSString *n in [@[ _name ] arrayByAddingObjectsFromArray:order])
        if ([names containsObject:n]) return n;
    return nil;
}
- (void)performAsCurrentDrawingAppearance:(void (^)(void))block {
    NSAppearance *saved = [NSThread currentThread].threadDictionary[@"HKCurrentAppearance"];
    [NSAppearance setCurrentAppearance:self];
    if (block) block();
    [NSAppearance setCurrentAppearance:saved];
}
- (BOOL)isEqual:(id)other {
    return self == other || ([other isKindOfClass:[NSAppearance class]] && [_name isEqual:((NSAppearance *)other)->_name]);
}
- (NSUInteger)hash { return _name.hash; }
- (NSString *)description { return [NSString stringWithFormat:@"<NSAppearance %@>", _name]; }
@end

/* ================================================================ menus */

static const NSEventModifierFlags kKeyMods =
    NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl | NSEventModifierFlagShift;

@implementation NSMenuItem {
    NSMenu *_submenu;
    __weak NSMenu *_menu;
    BOOL _separator;
    NSAttributedString *_attributedTitle;
    NSImage *_onStateImage, *_offStateImage, *_mixedStateImage;
    NSInteger _indentationLevel, _preferredImageVisibility;
    NSString *_toolTip, *_identifier, *_subtitle;
    BOOL _alternate, _allowsKeyEquivalentWhenHidden, _sectionHeader, _allowsAutoLocalization, _allowsAutoMirroring;
}
@synthesize title = _title, action = _action, target = _target, tag = _tag, keyEquivalent = _keyEquivalent,
            keyEquivalentModifierMask = _keyEquivalentModifierMask, representedObject = _representedObject, state = _state,
            enabled = _enabled, hidden = _hidden, image = _image, view = _view;

static BOOL g_uses_user_key_equivalents;
+ (BOOL)usesUserKeyEquivalents { return g_uses_user_key_equivalents; }
+ (void)setUsesUserKeyEquivalents:(BOOL)flag { g_uses_user_key_equivalents = flag; }
+ (NSMenuItem *)separatorItem {
    NSMenuItem *m = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
    m->_separator = YES;
    m.enabled = NO;
    return m;
}
+ (NSMenuItem *)sectionHeaderWithTitle:(NSString *)title {
    NSMenuItem *m = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
    m->_sectionHeader = YES;
    m.enabled = NO;
    return m;
}
- (instancetype)initWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key {
    if ((self = [super init])) {
        _title = [title copy] ?: @"";
        _action = action;
        _keyEquivalent = [key copy] ?: @"";
        _keyEquivalentModifierMask = NSEventModifierFlagCommand;
        _enabled = YES;
        _allowsAutoLocalization = _allowsAutoMirroring = YES;
    }
    return self;
}
- (instancetype)init { return [self initWithTitle:@"" action:NULL keyEquivalent:@""]; }
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (id)copyWithZone:(NSZone *)zone {
    NSMenuItem *m = [[NSMenuItem alloc] initWithTitle:_title action:_action keyEquivalent:_keyEquivalent];
    m->_separator = _separator;
    m.target = self.target;
    m.tag = _tag;
    m.keyEquivalentModifierMask = _keyEquivalentModifierMask;
    m.representedObject = _representedObject;
    m.state = _state;
    m.enabled = _enabled;
    m.hidden = _hidden;
    m.image = _image;
    m->_attributedTitle = _attributedTitle;
    m->_toolTip = _toolTip;
    m->_indentationLevel = _indentationLevel;
    m->_alternate = _alternate;
    m->_identifier = _identifier;
    m.submenu = [_submenu copy];
    return m;
}
- (NSMenu *)submenu { return _submenu; }
- (void)setSubmenu:(NSMenu *)submenu {
    if (_submenu.supermenu == _menu) _submenu.supermenu = nil;
    _submenu = submenu;
    submenu.supermenu = _menu;
}
- (NSMenu *)menu { return _menu; }
- (void)setMenu:(NSMenu *)menu {
    _menu = menu;
    _submenu.supermenu = menu;
}
- (BOOL)hasSubmenu { return _submenu != nil; }
- (BOOL)isSeparatorItem { return _separator; }
- (BOOL)isSectionHeader { return _sectionHeader; }
- (void)setSectionHeader:(BOOL)flag { _sectionHeader = flag; }
- (NSMenuItem *)parentItem {
    NSMenu *super_ = _menu.supermenu;
    for (NSMenuItem *i in super_.itemArray)
        if (i.submenu == _menu) return i;
    return nil;
}
- (BOOL)isHighlighted { return NO; }
- (BOOL)isHiddenOrHasHiddenAncestor { return _hidden || self.parentItem.isHiddenOrHasHiddenAncestor; }
- (NSAttributedString *)attributedTitle { return _attributedTitle; }
- (void)setAttributedTitle:(NSAttributedString *)t {
    _attributedTitle = [t copy];
    if (t.string) _title = t.string;
}
- (void)setTitleWithMnemonic:(NSString *)s { self.title = [s stringByReplacingOccurrencesOfString:@"&" withString:@""]; }
- (NSString *)userKeyEquivalent { return @""; }
- (NSEventModifierFlags)userKeyEquivalentModifierMask { return 0; }
- (NSImage *)onStateImage { return _onStateImage; }
- (void)setOnStateImage:(NSImage *)i { _onStateImage = i; }
- (NSImage *)offStateImage { return _offStateImage; }
- (void)setOffStateImage:(NSImage *)i { _offStateImage = i; }
- (NSImage *)mixedStateImage { return _mixedStateImage; }
- (void)setMixedStateImage:(NSImage *)i { _mixedStateImage = i; }
- (NSInteger)indentationLevel { return _indentationLevel; }
- (void)setIndentationLevel:(NSInteger)n { _indentationLevel = n; }
- (NSString *)toolTip { return _toolTip; }
- (void)setToolTip:(NSString *)t { _toolTip = [t copy]; }
- (NSString *)subtitle { return _subtitle; }
- (void)setSubtitle:(NSString *)s { _subtitle = [s copy]; }
- (BOOL)isAlternate { return _alternate; }
- (void)setAlternate:(BOOL)flag { _alternate = flag; }
- (BOOL)allowsKeyEquivalentWhenHidden { return _allowsKeyEquivalentWhenHidden; }
- (void)setAllowsKeyEquivalentWhenHidden:(BOOL)flag { _allowsKeyEquivalentWhenHidden = flag; }
- (BOOL)allowsAutomaticKeyEquivalentLocalization { return _allowsAutoLocalization; }
- (void)setAllowsAutomaticKeyEquivalentLocalization:(BOOL)flag { _allowsAutoLocalization = flag; }
- (BOOL)allowsAutomaticKeyEquivalentMirroring { return _allowsAutoMirroring; }
- (void)setAllowsAutomaticKeyEquivalentMirroring:(BOOL)flag { _allowsAutoMirroring = flag; }
- (NSInteger)preferredImageVisibility { return _preferredImageVisibility; }
- (void)setPreferredImageVisibility:(NSInteger)v { _preferredImageVisibility = v; }
- (NSUserInterfaceItemIdentifier)identifier { return _identifier; }
- (void)setIdentifier:(NSUserInterfaceItemIdentifier)identifier { _identifier = [identifier copy]; }
- (id)badge { return nil; }
- (void)setBadge:(id)badge {}
- (NSString *)description {
    return [NSString stringWithFormat:@"<NSMenuItem %p %@%@>", self, _separator ? @"(separator)" : _title,
                                      _submenu ? @" >" : @""];
}
@end

@implementation NSMenu {
    NSMutableArray<NSMenuItem *> *_items;
    NSFont *_font;
    CGFloat _minimumWidth;
    BOOL _showsStateColumn, _allowsContextMenuPlugIns, _updating, _preparing;
    NSInteger _layoutDirection;
}
@synthesize title = _title, delegate = _delegate, autoenablesItems = _autoenablesItems, supermenu = _supermenu;

static BOOL g_menu_bar_visible = YES;
+ (BOOL)menuBarVisible { return g_menu_bar_visible; }
+ (void)setMenuBarVisible:(BOOL)visible { g_menu_bar_visible = visible; }
+ (CGFloat)menuBarHeight { return 0; }
+ (void)popUpContextMenu:(NSMenu *)menu withEvent:(NSEvent *)event forView:(NSView *)view {
    hk_menu_context(menu, event, view); /* HKMenu.m */
}
+ (void)popUpContextMenu:(NSMenu *)menu withEvent:(NSEvent *)event forView:(NSView *)view withFont:(NSFont *)font {
    [self popUpContextMenu:menu withEvent:event forView:view];
}
- (instancetype)initWithTitle:(NSString *)title {
    if ((self = [super init])) {
        _title = [title copy] ?: @"";
        _items = [NSMutableArray array];
        _autoenablesItems = YES;
        _showsStateColumn = YES;
        _allowsContextMenuPlugIns = YES;
    }
    return self;
}
- (instancetype)init { return [self initWithTitle:@""]; }
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (id)copyWithZone:(NSZone *)zone {
    NSMenu *m = [[NSMenu alloc] initWithTitle:_title];
    for (NSMenuItem *i in _items) [m addItem:[i copy]];
    m.autoenablesItems = _autoenablesItems;
    m->_font = _font;
    return m;
}

- (NSArray<NSMenuItem *> *)itemArray { return [_items copy]; }
- (void)setItemArray:(NSArray<NSMenuItem *> *)items {
    [self removeAllItems];
    for (NSMenuItem *i in items) [self addItem:i];
}
- (NSInteger)numberOfItems { return (NSInteger)_items.count; }
- (void)insertItem:(NSMenuItem *)item atIndex:(NSInteger)index {
    if (!item) return;
    if (item.menu && item.menu != self) [item.menu removeItem:item];
    if (index < 0 || index > (NSInteger)_items.count) index = (NSInteger)_items.count;
    [_items insertObject:item atIndex:(NSUInteger)index];
    item.menu = self;
}
- (void)addItem:(NSMenuItem *)item { [self insertItem:item atIndex:(NSInteger)_items.count]; }
- (NSMenuItem *)addItemWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key {
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    [self addItem:i];
    return i;
}
- (NSMenuItem *)insertItemWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key atIndex:(NSInteger)index {
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    [self insertItem:i atIndex:index];
    return i;
}
- (void)removeItemAtIndex:(NSInteger)index {
    if (index < 0 || index >= (NSInteger)_items.count) return;
    NSMenuItem *i = _items[(NSUInteger)index];
    [_items removeObjectAtIndex:(NSUInteger)index];
    if (i.menu == self) i.menu = nil;
}
- (void)removeItem:(NSMenuItem *)item { [self removeItemAtIndex:[self indexOfItem:item]]; }
- (void)removeAllItems {
    for (NSMenuItem *i in _items)
        if (i.menu == self) i.menu = nil;
    [_items removeAllObjects];
}
- (NSMenuItem *)itemAtIndex:(NSInteger)index {
    return index >= 0 && index < (NSInteger)_items.count ? _items[(NSUInteger)index] : nil;
}
- (NSInteger)indexOfItem:(NSMenuItem *)item {
    NSUInteger i = [_items indexOfObjectIdenticalTo:item];
    return i == NSNotFound ? -1 : (NSInteger)i;
}
- (NSInteger)indexOfFirst:(BOOL (^)(NSMenuItem *))test {
    for (NSUInteger i = 0; i < _items.count; i++)
        if (test(_items[i])) return (NSInteger)i;
    return -1;
}
- (NSInteger)indexOfItemWithTag:(NSInteger)tag {
    return [self indexOfFirst:^BOOL(NSMenuItem *i) { return i.tag == tag; }];
}
- (NSInteger)indexOfItemWithTitle:(NSString *)title {
    return [self indexOfFirst:^BOOL(NSMenuItem *i) { return [i.title isEqualToString:title]; }];
}
- (NSInteger)indexOfItemWithSubmenu:(NSMenu *)submenu {
    return [self indexOfFirst:^BOOL(NSMenuItem *i) { return i.submenu == submenu; }];
}
- (NSInteger)indexOfItemWithRepresentedObject:(id)object {
    return [self indexOfFirst:^BOOL(NSMenuItem *i) { return i.representedObject == object || [i.representedObject isEqual:object]; }];
}
- (NSInteger)indexOfItemWithTarget:(id)target andAction:(SEL)action {
    return [self indexOfFirst:^BOOL(NSMenuItem *i) { return i.target == target && (!action || i.action == action); }];
}
- (NSMenuItem *)itemWithTag:(NSInteger)tag { return [self itemAtIndex:[self indexOfItemWithTag:tag]]; }
- (NSMenuItem *)itemWithTitle:(NSString *)title { return [self itemAtIndex:[self indexOfItemWithTitle:title]]; }
- (void)setSubmenu:(NSMenu *)menu forItem:(NSMenuItem *)item { item.submenu = menu; }
- (void)submenuAction:(id)sender {}
- (void)itemChanged:(NSMenuItem *)item {}
- (BOOL)isMainMenu { return NSApp.mainMenu == self; }
- (NSMenuItem *)highlightedItem { return hk_menu_highlighted(self); }
- (NSFont *)font { return _font ?: [NSFont menuFontOfSize:0]; }
- (void)setFont:(NSFont *)font { _font = font; }
- (CGFloat)minimumWidth { return _minimumWidth; }
- (void)setMinimumWidth:(CGFloat)w { _minimumWidth = w; }
- (NSSize)size { return NSMakeSize(fmax(_minimumWidth, 100), 22 * _items.count); }
- (BOOL)showsStateColumn { return _showsStateColumn; }
- (void)setShowsStateColumn:(BOOL)flag { _showsStateColumn = flag; }
- (BOOL)allowsContextMenuPlugIns { return _allowsContextMenuPlugIns; }
- (void)setAllowsContextMenuPlugIns:(BOOL)flag { _allowsContextMenuPlugIns = flag; }
- (CGFloat)menuBarHeight { return 0; }
- (NSInteger)userInterfaceLayoutDirection { return _layoutDirection; }
- (void)setUserInterfaceLayoutDirection:(NSInteger)d { _layoutDirection = d; }
- (BOOL)isTornOff { return NO; }
- (NSArray<NSMenuItem *> *)selectedItems { return @[]; }
- (void)cancelTracking { hk_menu_cancel(self); }
- (void)cancelTrackingWithoutAnimation { hk_menu_cancel(self); }
- (BOOL)popUpMenuPositioningItem:(NSMenuItem *)item atLocation:(NSPoint)location inView:(NSView *)view {
    return hk_menu_positioned(self, item, location, view);
}

/* Whether an item's action can be sent now: its own enabled flag, or (autoenabling) whether some target takes the
   action and agrees in validateMenuItem: or validateUserInterfaceItem:. */
- (BOOL)hk_validate:(NSMenuItem *)item {
    if (!_autoenablesItems) return item.enabled;
    if (item.submenu) return YES;
    if (!item.action) return NO;
    id target = NSApp ? [NSApp targetForAction:item.action to:item.target from:item]
                      : ([item.target respondsToSelector:item.action] ? item.target : nil);
    if (!target) return NO;
    if ([target respondsToSelector:@selector(validateMenuItem:)]) return [target validateMenuItem:item];
    if ([target respondsToSelector:@selector(validateUserInterfaceItem:)]) return [target validateUserInterfaceItem:item];
    return YES;
}
/* As AppKit's: the items' enabling only (validation can call back into update: once at a time). */
- (void)update {
    if (!_autoenablesItems || _updating) return;
    _updating = YES;
    for (NSMenuItem *i in [_items copy])
        if (!i.isSeparatorItem) i.enabled = [self hk_validate:i];
    _updating = NO;
}
/* Before a menu shows: the delegate fills it (menuNeedsUpdate:, once at a time), then its items are enabled. */
- (void)hk_prepare {
    if (_preparing) return;
    _preparing = YES;
    id d = _delegate;
    if ([d respondsToSelector:@selector(menuNeedsUpdate:)]) [d menuNeedsUpdate:self];
    _preparing = NO;
    [self update];
}
- (void)performActionForItemAtIndex:(NSInteger)index {
    NSMenuItem *item = [self itemAtIndex:index];
    if (!item.action) return;
    if (NSApp) [NSApp sendAction:item.action to:item.target from:item];
    else if ([item.target respondsToSelector:item.action]) ((void (*)(id, SEL, id))objc_msgSend)(item.target, item.action, item);
}
static BOOL key_matches(NSMenuItem *item, NSString *chars, NSEventModifierFlags mods) {
    NSString *key = item.keyEquivalent;
    if (!key.length || !chars.length) return NO;
    NSEventModifierFlags want = item.keyEquivalentModifierMask & kKeyMods;
    NSString *lower = key.lowercaseString;
    if (![lower isEqualToString:key]) want |= NSEventModifierFlagShift; /* "W" means shift-command-W */
    return (mods & kKeyMods) == want && [lower isEqualToString:chars.lowercaseString];
}
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (event.type != NSEventTypeKeyDown) return NO;
    id d = _delegate;
    if ([d respondsToSelector:@selector(menuHasKeyEquivalent:forEvent:target:action:)]) {
        id target = nil;
        SEL action = NULL;
        if (![d menuHasKeyEquivalent:self forEvent:event target:&target action:&action]) return NO;
    }
    NSString *chars = event.charactersIgnoringModifiers;
    NSEventModifierFlags mods = event.modifierFlags;
    for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
        NSMenuItem *item = _items[(NSUInteger)i];
        if (item.submenu) {
            if ([item.submenu performKeyEquivalent:event]) return YES;
            continue;
        }
        if (item.isSeparatorItem || (item.hidden && !item.allowsKeyEquivalentWhenHidden)) continue;
        if (!key_matches(item, chars, mods)) continue;
        if ([d respondsToSelector:@selector(menuNeedsUpdate:)]) [d menuNeedsUpdate:self];
        if (_autoenablesItems) item.enabled = [self hk_validate:item];
        if (!item.enabled) return NO;
        [self performActionForItemAtIndex:i];
        return YES;
    }
    return NO;
}
- (NSString *)description {
    return [NSString stringWithFormat:@"<NSMenu %p \"%@\" %lu items>", self, _title, (unsigned long)_items.count];
}
@end

/* ================================================================ pasteboards */

@interface NSPasteboardItem : HKShimObject
- (void)hk_setBoard:(NSPasteboard *)board;
@end
@interface NSPasteboard : HKShimObject
- (id)hk_ownerForType:(NSString *)type;
@end

static NSString *const kUTF8Text = @"public.utf8-plain-text";

@implementation NSPasteboardItem {
    NSMutableArray<NSString *> *_types;
    NSMutableDictionary<NSString *, id> *_values;
    NSMutableDictionary<NSString *, id> *_providers;
    __weak NSPasteboard *_board;
}
- (instancetype)init {
    if ((self = [super init])) {
        _types = [NSMutableArray array];
        _values = [NSMutableDictionary dictionary];
        _providers = [NSMutableDictionary dictionary];
    }
    return self;
}
- (void)hk_setBoard:(NSPasteboard *)board { _board = board; }
- (void)hk_addType:(NSString *)type {
    if (type && ![_types containsObject:type]) [_types addObject:type];
}
- (NSArray<NSString *> *)types { return [_types copy]; }
- (NSString *)availableTypeFromArray:(NSArray<NSString *> *)types {
    for (NSString *t in types)
        if ([_types containsObject:t]) return t;
    return nil;
}
- (BOOL)hk_set:(id)value forType:(NSString *)type {
    if (!type) return NO;
    [self hk_addType:type];
    if (value) _values[type] = value;
    else [_values removeObjectForKey:type];
    return YES;
}
- (BOOL)setData:(NSData *)data forType:(NSString *)type { return [self hk_set:[data copy] forType:type]; }
- (BOOL)setString:(NSString *)string forType:(NSString *)type { return [self hk_set:[string copy] forType:type]; }
- (BOOL)setPropertyList:(id)list forType:(NSString *)type { return [self hk_set:list forType:type]; }
- (BOOL)setDataProvider:(id)provider forTypes:(NSArray<NSString *> *)types {
    for (NSString *t in types) {
        [self hk_addType:t];
        if (provider) _providers[t] = provider;
    }
    return YES;
}
/* The value for a type, asking a promised type's provider (or the pasteboard's owner) for it first. */
- (id)hk_valueForType:(NSString *)type {
    if (!type || ![_types containsObject:type]) return nil;
    id v = _values[type];
    if (v) return v;
    NSPasteboard *board = _board;
    id provider = _providers[type];
    if ([provider respondsToSelector:@selector(pasteboard:item:provideDataForType:)])
        [provider pasteboard:board item:self provideDataForType:type];
    id owner = [board hk_ownerForType:type];
    if (!_values[type] && [owner respondsToSelector:@selector(pasteboard:provideDataForType:)])
        [owner pasteboard:board provideDataForType:type];
    return _values[type];
}
- (NSData *)dataForType:(NSString *)type {
    id v = [self hk_valueForType:type];
    if ([v isKindOfClass:[NSData class]]) return v;
    if ([v isKindOfClass:[NSString class]])
        return [v dataUsingEncoding:[type isEqualToString:@"public.utf16-plain-text"] ? NSUTF16LittleEndianStringEncoding : NSUTF8StringEncoding];
    if (v) return [NSPropertyListSerialization dataWithPropertyList:v format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    return nil;
}
- (NSString *)stringForType:(NSString *)type {
    id v = [self hk_valueForType:type];
    if ([v isKindOfClass:[NSString class]]) return v;
    if ([v isKindOfClass:[NSData class]]) {
        NSStringEncoding enc = [type isEqualToString:@"public.utf16-plain-text"] ? NSUTF16LittleEndianStringEncoding : NSUTF8StringEncoding;
        return [[NSString alloc] initWithData:v encoding:enc];
    }
    return nil;
}
- (id)propertyListForType:(NSString *)type {
    id v = [self hk_valueForType:type];
    if ([v isKindOfClass:[NSData class]]) {
        id plist = [NSPropertyListSerialization propertyListWithData:v options:0 format:NULL error:NULL];
        return plist ?: v;
    }
    return v;
}
- (NSArray<NSString *> *)writableTypesForPasteboard:(NSPasteboard *)pasteboard { return self.types; }
- (id)pasteboardPropertyListForType:(NSString *)type { return [self propertyListForType:type]; }
@end

@implementation NSPasteboard {
    NSString *_name;
    NSInteger _changeCount;
    NSMutableArray<NSPasteboardItem *> *_items;
    NSMutableDictionary<NSString *, id> *_owners;
}

static NSMutableDictionary<NSString *, NSPasteboard *> *g_boards;
static os_unfair_lock g_boards_lock = OS_UNFAIR_LOCK_INIT;

+ (NSPasteboard *)pasteboardWithName:(NSPasteboardName)name {
    if (!name) return nil;
    os_unfair_lock_lock(&g_boards_lock);
    if (!g_boards) g_boards = [NSMutableDictionary dictionary];
    NSPasteboard *p = g_boards[name];
    if (!p) {
        p = [[NSPasteboard alloc] init];
        p->_name = [name copy];
        p->_items = [NSMutableArray array];
        p->_owners = [NSMutableDictionary dictionary];
        g_boards[name] = p;
    }
    os_unfair_lock_unlock(&g_boards_lock);
    return p;
}
+ (NSPasteboard *)generalPasteboard { return [self pasteboardWithName:@"Apple CFPasteboard general"]; }
+ (NSPasteboard *)pasteboardWithUniqueName {
    return [self pasteboardWithName:[@"Apple CFPasteboard " stringByAppendingString:NSUUID.UUID.UUIDString]];
}
+ (NSArray<NSString *> *)typesFilterableTo:(NSString *)type { return type ? @[ type ] : @[]; }
- (NSPasteboardName)name { return _name; }
- (NSInteger)changeCount { return _changeCount; }
- (void)releaseGlobally {
    os_unfair_lock_lock(&g_boards_lock);
    if (g_boards[_name] == self) [g_boards removeObjectForKey:_name];
    os_unfair_lock_unlock(&g_boards_lock);
}
- (id)hk_ownerForType:(NSString *)type { return type ? _owners[type] : nil; }
- (NSInteger)clearContents {
    for (NSPasteboardItem *i in _items) [i hk_setBoard:nil];
    [_items removeAllObjects];
    [_owners removeAllObjects];
    return ++_changeCount;
}
- (NSInteger)prepareForNewContentsWithOptions:(NSUInteger)options { return [self clearContents]; }
- (NSPasteboardItem *)hk_first {
    if (!_items.count) {
        NSPasteboardItem *i = [[NSPasteboardItem alloc] init];
        [i hk_setBoard:self];
        [_items addObject:i];
    }
    return _items[0];
}
- (NSInteger)addTypes:(NSArray<NSPasteboardType> *)types owner:(id)owner {
    NSPasteboardItem *first = [self hk_first];
    for (NSString *t in types) {
        [first hk_addType:t];
        if (owner) _owners[t] = owner;
    }
    return _changeCount;
}
- (NSInteger)declareTypes:(NSArray<NSPasteboardType> *)types owner:(id)owner {
    [self clearContents];
    return [self addTypes:types owner:owner];
}
- (NSArray<NSPasteboardType> *)types {
    NSMutableArray *all = [NSMutableArray array];
    for (NSPasteboardItem *i in _items)
        for (NSString *t in i.types)
            if (![all containsObject:t]) [all addObject:t];
    return all;
}
- (NSPasteboardType)availableTypeFromArray:(NSArray<NSPasteboardType> *)types {
    NSArray *have = self.types;
    for (NSString *t in types)
        if ([have containsObject:t]) return t;
    return nil;
}
- (NSArray<NSPasteboardItem *> *)pasteboardItems { return [_items copy]; }
- (NSUInteger)indexOfPasteboardItem:(NSPasteboardItem *)item { return [_items indexOfObjectIdenticalTo:item]; }
- (BOOL)setString:(NSString *)string forType:(NSPasteboardType)type { return [[self hk_first] setString:string forType:type]; }
- (BOOL)setData:(NSData *)data forType:(NSPasteboardType)type { return [[self hk_first] setData:data forType:type]; }
- (BOOL)setPropertyList:(id)list forType:(NSPasteboardType)type { return [[self hk_first] setPropertyList:list forType:type]; }
- (NSPasteboardItem *)hk_itemWithType:(NSString *)type {
    for (NSPasteboardItem *i in _items)
        if ([i.types containsObject:type]) return i;
    return nil;
}
- (NSString *)stringForType:(NSPasteboardType)type { return [[self hk_itemWithType:type] stringForType:type]; }
- (NSData *)dataForType:(NSPasteboardType)type { return [[self hk_itemWithType:type] dataForType:type]; }
- (id)propertyListForType:(NSPasteboardType)type { return [[self hk_itemWithType:type] propertyListForType:type]; }

- (BOOL)writeObjects:(NSArray *)objects {
    BOOL wrote = NO;
    for (id o in objects) {
        NSPasteboardItem *item;
        if ([o isKindOfClass:[NSPasteboardItem class]]) {
            item = o;
        } else {
            item = [[NSPasteboardItem alloc] init];
            if ([o isKindOfClass:[NSString class]]) {
                [item setString:o forType:kUTF8Text];
            } else if ([o isKindOfClass:[NSAttributedString class]]) {
                [item setString:[o string] forType:kUTF8Text];
            } else if ([o isKindOfClass:[NSURL class]]) {
                NSURL *u = o;
                [item setString:u.absoluteString forType:u.isFileURL ? @"public.file-url" : @"public.url"];
                if (!u.isFileURL) [item setString:u.absoluteString forType:kUTF8Text];
            } else if ([o respondsToSelector:@selector(writableTypesForPasteboard:)]) {
                for (NSString *t in [o writableTypesForPasteboard:self]) {
                    id v = [o respondsToSelector:@selector(pasteboardPropertyListForType:)] ? [o pasteboardPropertyListForType:t] : nil;
                    if (v) [item setPropertyList:v forType:t];
                }
            } else {
                continue;
            }
        }
        [item hk_setBoard:self];
        [_items addObject:item];
        wrote = YES;
    }
    return wrote;
}
- (NSArray *)readObjectsForClasses:(NSArray<Class> *)classes options:(NSDictionary *)options {
    BOOL filesOnly = [options[@"NSPasteboardURLReadingFileURLsOnlyKey"] boolValue];
    NSMutableArray *out = [NSMutableArray array];
    for (NSPasteboardItem *item in _items) {
        for (Class cls in classes) {
            id obj = nil;
            if (cls == [NSPasteboardItem class]) {
                obj = item;
            } else if ([cls isSubclassOfClass:[NSURL class]]) {
                NSString *s = [item stringForType:@"public.file-url"];
                if (!s && !filesOnly) s = [item stringForType:@"public.url"];
                NSURL *u = s ? [NSURL URLWithString:s] : nil;
                if (u && (!filesOnly || u.isFileURL)) obj = u;
            } else if ([cls isSubclassOfClass:[NSString class]]) {
                obj = [item stringForType:kUTF8Text] ?: [item stringForType:@"public.utf16-plain-text"];
            } else if ([cls isSubclassOfClass:[NSAttributedString class]]) {
                NSString *s = [item stringForType:kUTF8Text];
                if (s) obj = [[NSAttributedString alloc] initWithString:s];
            } else if ([cls respondsToSelector:@selector(readableTypesForPasteboard:)]) {
                NSString *t = [item availableTypeFromArray:[(id)cls readableTypesForPasteboard:self]];
                if (t && [cls instancesRespondToSelector:@selector(initWithPasteboardPropertyList:ofType:)])
                    obj = [[cls alloc] initWithPasteboardPropertyList:[item propertyListForType:t] ofType:t];
            }
            if (obj) {
                [out addObject:obj];
                break;
            }
        }
    }
    return out;
}
- (BOOL)canReadObjectForClasses:(NSArray<Class> *)classes options:(NSDictionary *)options {
    return [self readObjectsForClasses:classes options:options].count > 0;
}
- (BOOL)canReadItemWithDataConformingToTypes:(NSArray<NSString *> *)types {
    for (NSString *have in self.types) {
        UTType *h = [UTType typeWithIdentifier:have];
        for (NSString *want in types) {
            if ([have isEqualToString:want]) return YES;
            UTType *w = [UTType typeWithIdentifier:want];
            if (h && w && [h conformsToType:w]) return YES;
        }
    }
    return NO;
}
- (NSString *)description {
    return [NSString stringWithFormat:@"<NSPasteboard %@ change %ld, %@>", _name, (long)_changeCount, self.types];
}
@end

/* ================================================================ the workspace and running applications */

@interface NSRunningApplication : HKShimObject
+ (instancetype)currentApplication;
@end
@implementation NSRunningApplication
static NSDate *g_launch_date;
+ (void)load { g_launch_date = [NSDate date]; }
+ (instancetype)currentApplication {
    static NSRunningApplication *app;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ app = [[NSRunningApplication alloc] init]; });
    return app;
}
+ (instancetype)runningApplicationWithProcessIdentifier:(pid_t)pid { return pid == getpid() ? [self currentApplication] : nil; }
+ (NSArray *)runningApplicationsWithBundleIdentifier:(NSString *)identifier {
    return [identifier isEqualToString:[self currentApplication].bundleIdentifier] ? @[ [self currentApplication] ] : @[];
}
+ (void)terminateAutomaticallyTerminableApplications {}
- (pid_t)processIdentifier { return getpid(); }
- (NSString *)bundleIdentifier { return hk_app_bundle().bundleIdentifier; }
- (NSURL *)bundleURL { return hk_app_bundle().bundleURL; }
- (NSURL *)executableURL { return hk_app_bundle().executableURL; }
- (NSString *)localizedName {
    NSDictionary *info = hk_app_bundle().infoDictionary;
    return info[@"CFBundleDisplayName"] ?: info[@"CFBundleName"] ?: NSProcessInfo.processInfo.processName;
}
- (NSImage *)icon { return NSApp.applicationIconImage; }
- (NSDate *)launchDate { return g_launch_date; }
- (NSApplicationActivationPolicy)activationPolicy { return NSApp ? NSApp.activationPolicy : NSApplicationActivationPolicyRegular; }
- (NSInteger)executableArchitecture { return 0x0100000c; /* CPU_TYPE_ARM64 */ }
- (BOOL)isActive { return YES; }
- (BOOL)isHidden { return NO; }
- (BOOL)isTerminated { return NO; }
- (BOOL)isFinishedLaunching { return YES; }
- (BOOL)ownsMenuBar { return YES; }
- (BOOL)activateWithOptions:(NSUInteger)options { return YES; }
- (BOOL)activateFromApplication:(NSRunningApplication *)app options:(NSUInteger)options { return YES; }
- (BOOL)hide { return NO; }
- (BOOL)unhide { return YES; }
- (BOOL)terminate { return NO; }
- (BOOL)forceTerminate { return NO; }
@end

@interface NSWorkspaceOpenConfiguration : HKShimObject
@property(nonatomic) BOOL activates, promptsUserIfNeeded, addsToRecentItems, hides, hidesOthers, createsNewApplicationInstance;
@property(nonatomic) BOOL allowsRunningApplicationSubstitution, requiresUniversalLinks, forPrinting, editing;
@property(nonatomic, copy) NSArray<NSString *> *arguments;
@property(nonatomic, copy) NSDictionary<NSString *, NSString *> *environment;
@property(nonatomic) id appleEvent;
@property(nonatomic) NSInteger architecture;
@end
@implementation NSWorkspaceOpenConfiguration
+ (instancetype)configuration { return [[self alloc] init]; }
- (instancetype)init {
    if ((self = [super init])) {
        _activates = _promptsUserIfNeeded = _addsToRecentItems = _allowsRunningApplicationSubstitution = YES;
        _arguments = @[];
        _environment = @{};
    }
    return self;
}
@end

@interface NSWorkspace : HKShimObject
@end
@implementation NSWorkspace {
    NSNotificationCenter *_center;
}
+ (NSWorkspace *)sharedWorkspace {
    static NSWorkspace *w;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        w = [[NSWorkspace alloc] init];
        w->_center = [[NSNotificationCenter alloc] init];
    });
    return w;
}
- (NSNotificationCenter *)notificationCenter { return _center; }
- (NSRunningApplication *)frontmostApplication { return [NSRunningApplication currentApplication]; }
- (NSRunningApplication *)menuBarOwningApplication { return [NSRunningApplication currentApplication]; }
- (NSArray<NSRunningApplication *> *)runningApplications { return @[ [NSRunningApplication currentApplication] ]; }
- (BOOL)accessibilityDisplayShouldReduceMotion { return NO; }
- (BOOL)accessibilityDisplayShouldIncreaseContrast { return NO; }
- (BOOL)accessibilityDisplayShouldReduceTransparency { return NO; }
- (BOOL)accessibilityDisplayShouldDifferentiateWithoutColor { return NO; }
- (BOOL)accessibilityDisplayShouldInvertColors { return NO; }
- (BOOL)accessibilityDisplayShouldShowBorders { return NO; }
- (BOOL)accessibilityDisplayShouldShowToolbarButtonShapes { return NO; }
- (BOOL)isVoiceOverEnabled { return NO; }
- (BOOL)isSwitchControlEnabled { return NO; }
- (BOOL)isAccessibilityFullKeyboardAccessEnabled { return NO; }
- (BOOL)isAccessibilityFullKeyboardAccessDrawingFocusRing { return NO; }

- (BOOL)openURL:(NSURL *)url {
    hk_log("NSWorkspace: openURL %s refused", url.absoluteString.UTF8String);
    return NO;
}
- (void)openURL:(NSURL *)url configuration:(NSWorkspaceOpenConfiguration *)configuration
    completionHandler:(void (^)(NSRunningApplication *, NSError *))handler {
    [self openURL:url];
    if (handler) on_main(^{ handler(nil, unsupported_error()); });
}
- (void)openURLs:(NSArray<NSURL *> *)urls withApplicationAtURL:(NSURL *)app configuration:(NSWorkspaceOpenConfiguration *)configuration
    completionHandler:(void (^)(NSRunningApplication *, NSError *))handler {
    if (handler) on_main(^{ handler(nil, unsupported_error()); });
}
- (void)openApplicationAtURL:(NSURL *)app configuration:(NSWorkspaceOpenConfiguration *)configuration
           completionHandler:(void (^)(NSRunningApplication *, NSError *))handler {
    if (handler) on_main(^{ handler(nil, unsupported_error()); });
}
- (BOOL)openFile:(NSString *)path { return NO; }
- (BOOL)openFile:(NSString *)path withApplication:(NSString *)app { return NO; }
- (BOOL)launchApplication:(NSString *)app { return NO; }
- (NSURL *)URLForApplicationWithBundleIdentifier:(NSString *)identifier { return nil; }
- (NSArray<NSURL *> *)URLsForApplicationsWithBundleIdentifier:(NSString *)identifier { return @[]; }
- (NSURL *)URLForApplicationToOpenURL:(NSURL *)url { return nil; }
- (NSURL *)URLForApplicationToOpenContentType:(UTType *)type { return nil; }
- (NSArray<NSURL *> *)URLsForApplicationsToOpenURL:(NSURL *)url { return @[]; }
- (NSArray<NSURL *> *)URLsForApplicationsToOpenContentType:(UTType *)type { return @[]; }
- (NSString *)fullPathForApplication:(NSString *)name { return nil; }
- (NSString *)absolutePathForAppBundleWithIdentifier:(NSString *)identifier { return nil; }
- (NSImage *)iconForFile:(NSString *)path { return blank_icon(32); }
- (NSImage *)iconForFiles:(NSArray<NSString *> *)paths { return blank_icon(32); }
- (NSImage *)iconForFileType:(NSString *)type { return blank_icon(32); }
- (NSImage *)iconForContentType:(UTType *)type { return blank_icon(32); }
- (BOOL)setIcon:(NSImage *)image forFile:(NSString *)path options:(NSUInteger)options { return NO; }
- (BOOL)selectFile:(NSString *)path inFileViewerRootedAtPath:(NSString *)root { return NO; }
- (void)activateFileViewerSelectingURLs:(NSArray<NSURL *> *)urls {}
- (void)recycleURLs:(NSArray<NSURL *> *)urls completionHandler:(void (^)(NSDictionary<NSURL *, NSURL *> *, NSError *))handler {
    if (handler) on_main(^{ handler(@{}, unsupported_error()); });
}
- (NSURL *)desktopImageURLForScreen:(NSScreen *)screen { return nil; }
- (NSDictionary *)desktopImageOptionsForScreen:(NSScreen *)screen { return nil; }
- (BOOL)setDesktopImageURL:(NSURL *)url forScreen:(NSScreen *)screen options:(NSDictionary *)options error:(NSError **)error {
    if (error) *error = unsupported_error();
    return NO;
}
- (BOOL)isFilePackageAtPath:(NSString *)path {
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] || !dir) return NO;
    return [@[ @"app", @"bundle", @"framework", @"plugin", @"kext", @"pkg", @"rtfd", @"xpc", @"appex" ]
        containsObject:path.pathExtension.lowercaseString];
}
- (NSString *)typeOfFile:(NSString *)path error:(NSError **)error {
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir]) {
        if (error) *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadNoSuchFileError userInfo:nil];
        return nil;
    }
    if (dir && ![self isFilePackageAtPath:path]) return UTTypeFolder.identifier;
    return [UTType typeWithFilenameExtension:path.pathExtension].identifier ?: UTTypeData.identifier;
}
- (NSString *)localizedDescriptionForType:(NSString *)type { return [UTType typeWithIdentifier:type].localizedDescription; }
- (NSString *)preferredFilenameExtensionForType:(NSString *)type { return [UTType typeWithIdentifier:type].preferredFilenameExtension; }
- (BOOL)filenameExtension:(NSString *)ext isValidForType:(NSString *)type {
    UTType *t = [UTType typeWithIdentifier:type];
    return t && [t.tags[UTTagClassFilenameExtension] containsObject:ext.lowercaseString];
}
- (BOOL)type:(NSString *)a conformsToType:(NSString *)b {
    UTType *x = [UTType typeWithIdentifier:a], *y = [UTType typeWithIdentifier:b];
    return [a isEqualToString:b] || (x && y && [x conformsToType:y]);
}
- (BOOL)getInfoForFile:(NSString *)path application:(NSString **)app type:(NSString **)type { return NO; }
- (void)noteFileSystemChanged:(NSString *)path {}
- (void)hideOtherApplications {}
- (NSInteger)extendPowerOffBy:(NSInteger)ms { return 0; }
- (NSArray<NSString *> *)mountedLocalVolumePaths { return @[]; }
- (NSArray<NSString *> *)mountedRemovableMedia { return @[]; }
- (BOOL)unmountAndEjectDeviceAtPath:(NSString *)path { return NO; }
- (void)requestAuthorizationOfType:(NSInteger)type completionHandler:(void (^)(id, NSError *))handler {
    if (handler) on_main(^{ handler(nil, unsupported_error()); });
}
@end

/* ================================================================ tracking areas */

@implementation NSTrackingArea {
    NSRect _rect;
    NSTrackingAreaOptions _options;
    __weak id _owner;
    NSDictionary *_userInfo;
}
- (instancetype)initWithRect:(NSRect)rect options:(NSTrackingAreaOptions)options owner:(id)owner userInfo:(NSDictionary *)userInfo {
    if ((self = [super init])) {
        _rect = rect;
        _options = options;
        _owner = owner;
        _userInfo = userInfo;
    }
    return self;
}
- (instancetype)initWithRect:(NSRect)rect options:(NSTrackingAreaOptions)options pressureConfigurations:(NSDictionary *)configs
                       owner:(id)owner userInfo:(NSDictionary *)userInfo {
    return [self initWithRect:rect options:options owner:owner userInfo:userInfo];
}
- (instancetype)init { return [self initWithRect:NSZeroRect options:0 owner:nil userInfo:nil]; }
- (id)copyWithZone:(NSZone *)zone {
    return [[[self class] alloc] initWithRect:_rect options:_options owner:_owner userInfo:_userInfo];
}
- (NSRect)rect { return _rect; }
- (void)_setRect:(NSRect)rect { _rect = rect; }
- (NSTrackingAreaOptions)options { return _options; }
- (id)owner { return _owner; }
- (NSDictionary *)userInfo { return _userInfo; }
- (NSString *)description {
    return [NSString stringWithFormat:@"<%@ %p %@ options 0x%lx owner %@>", NSStringFromClass([self class]), self,
                                      NSStringFromRect(_rect), (unsigned long)_options, [_owner class]];
}
@end

/* ================================================================ text input contexts */

@implementation NSTextInputContext {
    __weak id _client;
    NSArray<NSString *> *_allowedLocales;
}

static __weak NSTextInputContext *g_active_context;

+ (NSTextInputContext *)currentInputContext {
    NSTextInputContext *c = g_active_context;
    if (c) return c;
    id responder = NSApp.keyWindow.firstResponder;
    return [responder respondsToSelector:@selector(inputContext)] ? [responder inputContext] : nil;
}
+ (NSString *)localizedNameForInputSource:(NSString *)source { return @"U.S."; }
- (instancetype)initWithClient:(id)client {
    if ((self = [super init])) _client = client;
    return self;
}
- (instancetype)init { return [self initWithClient:nil]; }
- (id)client { return _client; }
- (void)activate { g_active_context = self; }
- (void)deactivate {
    if (g_active_context == self) g_active_context = nil;
}
- (BOOL)acceptsGlyphInfo { return NO; }
- (void)setAcceptsGlyphInfo:(BOOL)flag {}
- (NSString *)selectedKeyboardInputSource { return @"com.apple.keylayout.US"; }
- (void)setSelectedKeyboardInputSource:(NSString *)source {}
- (NSArray<NSString *> *)keyboardInputSources { return @[ @"com.apple.keylayout.US" ]; }
- (NSArray<NSString *> *)allowedInputSourceLocales { return _allowedLocales; }
- (void)setAllowedInputSourceLocales:(NSArray<NSString *> *)locales { _allowedLocales = [locales copy]; }
- (void)discardMarkedText {
    id c = _client;
    if ([c respondsToSelector:@selector(hasMarkedText)] && [c hasMarkedText] && [c respondsToSelector:@selector(unmarkText)])
        [c unmarkText];
}
- (void)invalidateCharacterCoordinates {}
- (void)textInputClientWillStartScrollingOrZooming {}
- (void)textInputClientDidEndScrollingOrZooming {}
- (void)textInputClientDidUpdateSelection {}
- (void)textInputClientDidScroll {}
- (BOOL)handleEventByKeyboardLayout:(NSEvent *)event { return [self handleEvent:event]; }

typedef enum { K_NONE, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_BACKSPACE, K_DELETE, K_RETURN, K_TAB, K_ESC, K_HOME, K_END, K_PGUP, K_PGDN } HKKey;

static HKKey key_of(NSEvent *e) {
    switch (e.keyCode) {
    case 123: return K_LEFT;
    case 124: return K_RIGHT;
    case 126: return K_UP;
    case 125: return K_DOWN;
    case 51: return K_BACKSPACE;
    case 117: return K_DELETE;
    case 36: case 76: return K_RETURN;
    case 48: return K_TAB;
    case 53: return K_ESC;
    case 115: return K_HOME;
    case 119: return K_END;
    case 116: return K_PGUP;
    case 121: return K_PGDN;
    default: break;
    }
    NSString *s = e.charactersIgnoringModifiers.length ? e.charactersIgnoringModifiers : e.characters;
    switch (s.length ? [s characterAtIndex:0] : 0) {
    case 0xF702: return K_LEFT;
    case 0xF703: return K_RIGHT;
    case 0xF700: return K_UP;
    case 0xF701: return K_DOWN;
    case 0x7F: case 0x08: return K_BACKSPACE;
    case 0xF728: return K_DELETE;
    case '\r': case '\n': case 0x03: return K_RETURN;
    case '\t': case 0x19: return K_TAB;
    case 0x1B: return K_ESC;
    case 0xF729: return K_HOME;
    case 0xF72B: return K_END;
    case 0xF72C: return K_PGUP;
    case 0xF72D: return K_PGDN;
    default: return K_NONE;
    }
}

/* The standard key bindings (AppKit's StandardKeyBinding.dict) for the keys that are commands, not text. */
static SEL command_for(NSEvent *e) {
    NSEventModifierFlags m = e.modifierFlags;
    BOOL shift = m & NSEventModifierFlagShift, cmd = m & NSEventModifierFlagCommand, opt = m & NSEventModifierFlagOption,
         ctrl = m & NSEventModifierFlagControl;
#define PICK(plain, sel_shift) (shift ? @selector(sel_shift) : @selector(plain))
    switch (key_of(e)) {
    case K_LEFT:
        return cmd ? PICK(moveToBeginningOfLine:, moveToBeginningOfLineAndModifySelection:)
               : opt ? PICK(moveWordLeft:, moveWordLeftAndModifySelection:)
                     : PICK(moveLeft:, moveLeftAndModifySelection:);
    case K_RIGHT:
        return cmd ? PICK(moveToEndOfLine:, moveToEndOfLineAndModifySelection:)
               : opt ? PICK(moveWordRight:, moveWordRightAndModifySelection:)
                     : PICK(moveRight:, moveRightAndModifySelection:);
    case K_UP:
        return cmd ? PICK(moveToBeginningOfDocument:, moveToBeginningOfDocumentAndModifySelection:)
               : opt ? PICK(moveToBeginningOfParagraph:, moveParagraphBackwardAndModifySelection:)
                     : PICK(moveUp:, moveUpAndModifySelection:);
    case K_DOWN:
        return cmd ? PICK(moveToEndOfDocument:, moveToEndOfDocumentAndModifySelection:)
               : opt ? PICK(moveToEndOfParagraph:, moveParagraphForwardAndModifySelection:)
                     : PICK(moveDown:, moveDownAndModifySelection:);
    case K_BACKSPACE:
        return cmd ? @selector(deleteToBeginningOfLine:) : opt ? @selector(deleteWordBackward:)
               : ctrl ? @selector(deleteBackwardByDecomposingPreviousCharacter:) : @selector(deleteBackward:);
    case K_DELETE: return opt ? @selector(deleteWordForward:) : @selector(deleteForward:);
    case K_RETURN: return opt || ctrl ? @selector(insertNewlineIgnoringFieldEditor:) : @selector(insertNewline:);
    case K_TAB:
        return shift ? @selector(insertBacktab:) : opt || ctrl ? @selector(insertTabIgnoringFieldEditor:) : @selector(insertTab:);
    case K_ESC: return @selector(cancelOperation:);
    case K_HOME: return PICK(scrollToBeginningOfDocument:, moveToBeginningOfDocumentAndModifySelection:);
    case K_END: return PICK(scrollToEndOfDocument:, moveToEndOfDocumentAndModifySelection:);
    case K_PGUP: return opt ? @selector(pageUp:) : PICK(scrollPageUp:, pageUpAndModifySelection:);
    case K_PGDN: return opt ? @selector(pageDown:) : PICK(scrollPageDown:, pageDownAndModifySelection:);
    case K_NONE: break;
    }
#undef PICK
    if (ctrl && !cmd && !opt) {
        NSString *s = e.charactersIgnoringModifiers.lowercaseString;
        switch (s.length ? [s characterAtIndex:0] : 0) {
        case 'a': return shift ? @selector(moveToBeginningOfParagraphAndModifySelection:) : @selector(moveToBeginningOfParagraph:);
        case 'e': return shift ? @selector(moveToEndOfParagraphAndModifySelection:) : @selector(moveToEndOfParagraph:);
        case 'b': return shift ? @selector(moveBackwardAndModifySelection:) : @selector(moveBackward:);
        case 'f': return shift ? @selector(moveForwardAndModifySelection:) : @selector(moveForward:);
        case 'p': return shift ? @selector(moveUpAndModifySelection:) : @selector(moveUp:);
        case 'n': return shift ? @selector(moveDownAndModifySelection:) : @selector(moveDown:);
        case 'd': return @selector(deleteForward:);
        case 'h': return @selector(deleteBackward:);
        case 'k': return @selector(deleteToEndOfParagraph:);
        case 'y': return @selector(yank:);
        case 't': return @selector(transpose:);
        case 'o': return @selector(insertNewlineIgnoringFieldEditor:);
        case 'l': return @selector(centerSelectionInVisibleArea:);
        case 'v': return @selector(pageDown:);
        default: break;
        }
    }
    return NULL;
}

static BOOL printable(NSString *s) {
    if (!s.length) return NO;
    unichar c = [s characterAtIndex:0];
    return c >= 0x20 && c != 0x7F && !(c >= 0xF700 && c <= 0xF8FF);
}

- (BOOL)handleEvent:(NSEvent *)event {
    if (event.type != NSEventTypeKeyDown) return NO;
    id client = _client;
    if (!client) return NO;
    SEL command = command_for(event);
    if (command) {
        if ([client respondsToSelector:@selector(doCommandBySelector:)]) [client doCommandBySelector:command];
        else if ([client respondsToSelector:@selector(tryToPerform:with:)]) [client tryToPerform:command with:nil];
        return YES;
    }
    if (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return NO;
    NSString *text = event.characters;
    if (!printable(text)) return NO;
    if ([client respondsToSelector:@selector(insertText:replacementRange:)]) [client insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
    else if ([client respondsToSelector:@selector(insertText:)]) [client insertText:text];
    else return NO;
    return YES;
}
@end

/* ================================================================ animation */

@interface NSAnimationContext : HKShimObject
@property(nonatomic) NSTimeInterval duration;
@property(nonatomic) BOOL allowsImplicitAnimation;
@property(nonatomic, strong) CAMediaTimingFunction *timingFunction;
@property(nonatomic, copy) void (^completionHandler)(void);
@end
@implementation NSAnimationContext

static NSMutableArray<NSAnimationContext *> *animation_stack(void) {
    NSMutableDictionary *d = [NSThread currentThread].threadDictionary;
    NSMutableArray *s = d[@"HKAnimationContexts"];
    if (!s) d[@"HKAnimationContexts"] = s = [NSMutableArray array];
    return s;
}
+ (NSAnimationContext *)currentContext {
    NSMutableArray *s = animation_stack();
    if (!s.count) [s addObject:[[NSAnimationContext alloc] init]]; /* the thread's outermost, never popped */
    return s.lastObject;
}
- (instancetype)init {
    if ((self = [super init])) _duration = 0.25;
    return self;
}
+ (void)beginGrouping {
    NSAnimationContext *outer = [self currentContext], *c = [[NSAnimationContext alloc] init];
    c.allowsImplicitAnimation = outer.allowsImplicitAnimation;
    [animation_stack() addObject:c];
    [CATransaction begin];
    [CATransaction setAnimationDuration:c.duration];
}
+ (void)endGrouping {
    NSMutableArray *s = animation_stack();
    if (s.count < 2) return;
    NSAnimationContext *c = s.lastObject;
    [s removeLastObject];
    [CATransaction commit];
    void (^done)(void) = c.completionHandler;
    if (done) on_main(done);
}
+ (void)runAnimationGroup:(void (^)(NSAnimationContext *))changes completionHandler:(void (^)(void))completion {
    [self beginGrouping];
    NSAnimationContext *c = [self currentContext];
    if (changes) changes(c);
    if (completion) c.completionHandler = completion;
    [self endGrouping];
}
+ (void)runAnimationGroup:(void (^)(NSAnimationContext *))changes { [self runAnimationGroup:changes completionHandler:nil]; }
- (void)setDuration:(NSTimeInterval)duration {
    _duration = duration;
    if (animation_stack().lastObject == self) [CATransaction setAnimationDuration:duration];
}
- (void)setTimingFunction:(CAMediaTimingFunction *)f {
    _timingFunction = f;
    if (animation_stack().lastObject == self) [CATransaction setAnimationTimingFunction:f];
}
@end

/* NSAnimation: progress driven by a timer on the run loop (non-blocking modes) or stepped in place (blocking). */
@interface NSAnimation : HKShimObject <NSCopying>
@property float frameRate;
@property NSUInteger animationBlockingMode;
@property(copy) NSArray<NSNumber *> *progressMarks;
@end
@implementation NSAnimation {
    NSTimeInterval _duration;
    NSUInteger _curve, _blockingMode;
    float _frameRate, _progress;
    __weak id _delegate;
    NSTimer *_timer;
    CFAbsoluteTime _start;
    NSMutableArray<NSNumber *> *_marks;
    BOOL _animating;
}
- (instancetype)initWithDuration:(NSTimeInterval)duration animationCurve:(NSUInteger)curve {
    if ((self = [super init])) {
        _duration = duration;
        _curve = curve;
        _marks = [NSMutableArray array];
    }
    return self;
}
- (instancetype)init { return [self initWithDuration:0.25 animationCurve:0]; }
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (void)dealloc { [_timer invalidate]; }
- (id)copyWithZone:(NSZone *)zone {
    NSAnimation *a = [[[self class] alloc] initWithDuration:_duration animationCurve:_curve];
    a.frameRate = _frameRate;
    a.animationBlockingMode = _blockingMode;
    a.progressMarks = self.progressMarks;
    return a;
}
- (NSTimeInterval)duration { return _duration; }
- (void)setDuration:(NSTimeInterval)d { _duration = d; }
- (NSUInteger)animationCurve { return _curve; }
- (void)setAnimationCurve:(NSUInteger)c { _curve = c; }
- (float)frameRate { return _frameRate; }
- (void)setFrameRate:(float)r { _frameRate = r; }
- (NSUInteger)animationBlockingMode { return _blockingMode; }
- (void)setAnimationBlockingMode:(NSUInteger)m { _blockingMode = m; }
- (id)delegate { return _delegate; }
- (void)setDelegate:(id)d { _delegate = d; }
- (NSArray<NSString *> *)runLoopModesForAnimating { return nil; }
- (BOOL)isAnimating { return _animating; }
- (float)currentProgress { return _progress; }
- (void)setCurrentProgress:(float)progress {
    float old = _progress;
    _progress = fmaxf(0, fminf(1, progress));
    id d = _delegate;
    if ([d respondsToSelector:@selector(animation:didReachProgressMark:)])
        for (NSNumber *m in [_marks copy])
            if (old < m.floatValue && _progress >= m.floatValue) [d animation:self didReachProgressMark:m.floatValue];
}
- (float)currentValue {
    id d = _delegate;
    if ([d respondsToSelector:@selector(animation:valueForProgress:)]) return [d animation:self valueForProgress:_progress];
    float p = _progress;
    switch (_curve) {
    case 1: return p * p;                                 /* ease in */
    case 2: return 1 - (1 - p) * (1 - p);                 /* ease out */
    case 3: return p;                                     /* linear */
    default: return (float)((1 - cos(M_PI * p)) / 2);     /* ease in-out */
    }
}
- (NSArray<NSNumber *> *)progressMarks { return [_marks copy]; }
- (void)setProgressMarks:(NSArray<NSNumber *> *)marks { _marks = [marks mutableCopy] ?: [NSMutableArray array]; }
- (void)addProgressMark:(float)mark { [_marks addObject:@(mark)]; }
- (void)removeProgressMark:(float)mark { [_marks removeObject:@(mark)]; }
- (void)startWhenAnimation:(NSAnimation *)a reachesProgress:(float)p {}
- (void)stopWhenAnimation:(NSAnimation *)a reachesProgress:(float)p {}
- (void)clearStartAnimation {}
- (void)clearStopAnimation {}
/* Ends on the time elapsed, not on _progress: a subclass's setCurrentProgress: need not call super. */
- (BOOL)hk_step {
    double elapsed = CFAbsoluteTimeGetCurrent() - _start;
    float p = _duration > 0 ? (float)fmin(1, elapsed / _duration) : 1;
    [self setCurrentProgress:p];
    if (p < 1 || !_animating) return _animating;
    _animating = NO;
    [_timer invalidate];
    _timer = nil;
    id d = _delegate;
    if ([d respondsToSelector:@selector(animationDidEnd:)]) [d animationDidEnd:self];
    return NO;
}
- (void)startAnimation {
    if (_animating) return;
    id d = _delegate;
    if ([d respondsToSelector:@selector(animationShouldStart:)] && ![d animationShouldStart:self]) return;
    if (_progress >= 1) _progress = 0;
    _animating = YES;
    _start = CFAbsoluteTimeGetCurrent() - _progress * _duration;
    NSTimeInterval frame = 1.0 / (_frameRate > 0 ? _frameRate : 60);
    if (_blockingMode == 0) {
        while ([self hk_step]) usleep((useconds_t)(frame * 1e6));
        return;
    }
    _timer = [NSTimer timerWithTimeInterval:frame repeats:YES block:^(NSTimer *t) { [self hk_step]; }];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}
- (void)stopAnimation {
    if (!_animating) return;
    _animating = NO;
    [_timer invalidate];
    _timer = nil;
    id d = _delegate;
    if ([d respondsToSelector:@selector(animationDidStop:)]) [d animationDidStop:self];
}
@end

/* ================================================================ spell checking: nothing is misspelled */

@interface NSSpellChecker : HKShimObject
@end
@implementation NSSpellChecker {
    NSString *_language;
    NSMutableDictionary<NSNumber *, NSArray *> *_ignored;
}
static NSInteger g_spell_tag;
+ (NSSpellChecker *)sharedSpellChecker {
    static NSSpellChecker *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        s = [[NSSpellChecker alloc] init];
        s->_language = @"en";
        s->_ignored = [NSMutableDictionary dictionary];
    });
    return s;
}
+ (BOOL)sharedSpellCheckerExists { return YES; }
+ (NSInteger)uniqueSpellDocumentTag { return ++g_spell_tag; }
+ (BOOL)isAutomaticSpellingCorrectionEnabled { return NO; }
+ (BOOL)isAutomaticTextReplacementEnabled { return NO; }
+ (BOOL)isAutomaticQuoteSubstitutionEnabled { return NO; }
+ (BOOL)isAutomaticDashSubstitutionEnabled { return NO; }
+ (BOOL)isAutomaticCapitalizationEnabled { return NO; }
+ (BOOL)isAutomaticPeriodSubstitutionEnabled { return NO; }
+ (BOOL)isAutomaticTextCompletionEnabled { return NO; }
+ (BOOL)isAutomaticInlinePredictionEnabled { return NO; }
- (NSString *)language { return _language; }
- (BOOL)setLanguage:(NSString *)language {
    _language = [language copy] ?: @"en";
    return YES;
}
- (NSArray<NSString *> *)availableLanguages { return @[ @"en" ]; }
- (NSArray<NSString *> *)userPreferredLanguages { return @[ @"en" ]; }
- (BOOL)automaticallyIdentifiesLanguages { return NO; }
- (void)setAutomaticallyIdentifiesLanguages:(BOOL)flag {}
- (void)setCompletionLanguage:(NSString *)language {}
- (NSRange)checkSpellingOfString:(NSString *)string startingAt:(NSInteger)start { return NSMakeRange(NSNotFound, 0); }
- (NSRange)checkSpellingOfString:(NSString *)string startingAt:(NSInteger)start language:(NSString *)language wrap:(BOOL)wrap
          inSpellDocumentWithTag:(NSInteger)tag wordCount:(NSInteger *)count {
    if (count) *count = 0;
    return NSMakeRange(NSNotFound, 0);
}
- (NSRange)checkGrammarOfString:(NSString *)string startingAt:(NSInteger)start language:(NSString *)language wrap:(BOOL)wrap
         inSpellDocumentWithTag:(NSInteger)tag details:(NSArray **)details {
    if (details) *details = @[];
    return NSMakeRange(NSNotFound, 0);
}
- (NSArray *)checkString:(NSString *)string range:(NSRange)range types:(uint64_t)types options:(NSDictionary *)options
    inSpellDocumentWithTag:(NSInteger)tag orthography:(id *)orthography wordCount:(NSInteger *)count {
    if (orthography) *orthography = nil;
    if (count) *count = 0;
    return @[];
}
- (NSInteger)requestCheckingOfString:(NSString *)string range:(NSRange)range types:(uint64_t)types options:(NSDictionary *)options
              inSpellDocumentWithTag:(NSInteger)tag
                   completionHandler:(void (^)(NSInteger, NSArray *, id, NSInteger))handler {
    NSInteger seq = ++g_spell_tag;
    if (handler) on_main(^{ handler(seq, @[], nil, 0); });
    return seq;
}
- (NSArray<NSString *> *)guessesForWordRange:(NSRange)range inString:(NSString *)string language:(NSString *)language
                      inSpellDocumentWithTag:(NSInteger)tag {
    return @[];
}
- (NSString *)correctionForWordRange:(NSRange)range inString:(NSString *)string language:(NSString *)language
              inSpellDocumentWithTag:(NSInteger)tag {
    return nil;
}
- (NSArray<NSString *> *)completionsForPartialWordRange:(NSRange)range inString:(NSString *)string language:(NSString *)language
                                 inSpellDocumentWithTag:(NSInteger)tag {
    return @[];
}
- (void)ignoreWord:(NSString *)word inSpellDocumentWithTag:(NSInteger)tag {
    if (!word) return;
    NSArray *old = _ignored[@(tag)] ?: @[];
    _ignored[@(tag)] = [old arrayByAddingObject:word];
}
- (NSArray<NSString *> *)ignoredWordsInSpellDocumentWithTag:(NSInteger)tag { return _ignored[@(tag)]; }
- (void)setIgnoredWords:(NSArray<NSString *> *)words inSpellDocumentWithTag:(NSInteger)tag { _ignored[@(tag)] = [words copy]; }
- (void)closeSpellDocumentWithTag:(NSInteger)tag { [_ignored removeObjectForKey:@(tag)]; }
- (void)learnWord:(NSString *)word {}
- (void)unlearnWord:(NSString *)word {}
- (BOOL)hasLearnedWord:(NSString *)word { return NO; }
- (id)spellingPanel { return nil; }
- (id)substitutionsPanel { return nil; }
- (void)updateSpellingPanelWithMisspelledWord:(NSString *)word {}
- (void)updateSpellingPanelWithGrammarString:(NSString *)string detail:(NSDictionary *)detail {}
- (void)dismissCorrectionIndicatorForView:(NSView *)view {}
@end

/* ================================================================ controllers */

@interface NSViewController : NSResponder
- (instancetype)initWithNibName:(NSString *)nibName bundle:(NSBundle *)bundle;
@property(nonatomic, strong) NSView *view;
@property(nonatomic, strong) id representedObject;
@property(nonatomic, copy) NSString *title;
@property(nonatomic) NSSize preferredContentSize;
@property(nonatomic, readonly) NSString *nibName;
@property(nonatomic, readonly) NSBundle *nibBundle;
@property(nonatomic, readonly, weak) NSViewController *parentViewController;
@property(nonatomic, copy) NSArray<NSViewController *> *childViewControllers;
@end
@implementation NSViewController {
    NSView *_view;
    NSString *_nibName;
    NSBundle *_nibBundle;
    NSMutableArray<NSViewController *> *_children;
    __weak NSViewController *_parent;
}
+ (BOOL)requiresConstraintBasedLayout { return NO; }
- (instancetype)initWithNibName:(NSString *)nibName bundle:(NSBundle *)bundle {
    if ((self = [super init])) {
        _nibName = [nibName copy];
        _nibBundle = bundle;
        _children = [NSMutableArray array];
    }
    return self;
}
- (instancetype)init { return [self initWithNibName:nil bundle:nil]; }
- (instancetype)initWithCoder:(NSCoder *)coder { return [self init]; }
- (NSString *)nibName { return _nibName; }
- (NSBundle *)nibBundle { return _nibBundle; }
/* There are no nibs here: a view controller that does not make its own view gets an empty one. */
- (void)loadView {
    if (_nibName) hk_log("NSViewController: nib \"%s\" not loaded", _nibName.UTF8String);
    self.view = [[NSView alloc] initWithFrame:NSZeroRect];
}
- (NSView *)view {
    if (!_view) {
        [self loadView];
        if (_view) [self viewDidLoad];
    }
    return _view;
}
- (void)setView:(NSView *)view {
    if (_view.nextResponder == self) _view.nextResponder = self.nextResponder;
    _view = view;
    if (view && view.nextResponder != self) {
        self.nextResponder = view.nextResponder;
        view.nextResponder = self;
    }
}
- (BOOL)isViewLoaded { return _view != nil; }
- (void)loadViewIfNeeded { (void)self.view; }
- (NSView *)viewIfLoaded { return _view; }
- (void)viewDidLoad {}
- (void)viewWillAppear {}
- (void)viewDidAppear {}
- (void)viewWillDisappear {}
- (void)viewDidDisappear {}
- (void)viewWillLayout {}
- (void)viewDidLayout {}
- (void)updateViewConstraints {}
- (void)viewWillTransitionToSize:(NSSize)size {}
- (NSSize)preferredMinimumSize { return NSZeroSize; }
- (NSSize)preferredMaximumSize { return NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX); }
- (NSPoint)preferredScreenOrigin { return NSZeroPoint; }
- (void)setPreferredScreenOrigin:(NSPoint)p {}
- (NSEdgeInsets)additionalSafeAreaInsets { return NSEdgeInsetsZero; }
- (void)setAdditionalSafeAreaInsets:(NSEdgeInsets)insets {}
- (id)storyboard { return nil; }
- (id)extensionContext { return nil; }
- (NSView *)sourceItemView { return nil; }
- (NSArray *)presentedViewControllers { return nil; }
- (NSViewController *)presentingViewController { return nil; }
- (NSArray<NSViewController *> *)childViewControllers { return [_children copy]; }
- (void)setChildViewControllers:(NSArray<NSViewController *> *)children {
    for (NSViewController *c in [_children copy]) [c removeFromParentViewController];
    for (NSViewController *c in children) [self addChildViewController:c];
}
- (NSViewController *)parentViewController { return _parent; }
- (void)insertChildViewController:(NSViewController *)child atIndex:(NSInteger)index {
    if (!child) return;
    [child removeFromParentViewController];
    if (index < 0 || index > (NSInteger)_children.count) index = (NSInteger)_children.count;
    [_children insertObject:child atIndex:(NSUInteger)index];
    child->_parent = self;
}
- (void)addChildViewController:(NSViewController *)child { [self insertChildViewController:child atIndex:(NSInteger)_children.count]; }
- (void)removeChildViewControllerAtIndex:(NSInteger)index {
    if (index < 0 || index >= (NSInteger)_children.count) return;
    _children[(NSUInteger)index]->_parent = nil;
    [_children removeObjectAtIndex:(NSUInteger)index];
}
- (void)removeFromParentViewController {
    NSViewController *p = _parent;
    if (p) [p removeChildViewControllerAtIndex:(NSInteger)[p->_children indexOfObjectIdenticalTo:self]];
}
- (void)transitionFromViewController:(NSViewController *)from toViewController:(NSViewController *)to
                             options:(NSUInteger)options completionHandler:(void (^)(void))completion {
    NSView *old = from.view, *superview = old.superview;
    if (superview) [superview replaceSubview:old with:to.view];
    if (completion) completion();
}
- (void)presentViewController:(NSViewController *)vc animator:(id)animator {
    hk_log("NSViewController: presenting %s not supported", object_getClassName(vc));
}
- (void)presentViewControllerAsSheet:(NSViewController *)vc { [self presentViewController:vc animator:nil]; }
- (void)presentViewControllerAsModalWindow:(NSViewController *)vc { [self presentViewController:vc animator:nil]; }
- (void)presentViewController:(NSViewController *)vc asPopoverRelativeToRect:(NSRect)rect ofView:(NSView *)view
                preferredEdge:(NSRectEdge)edge behavior:(NSInteger)behavior {
    [self presentViewController:vc animator:nil];
}
- (void)dismissViewController:(NSViewController *)vc {}
- (void)dismissController:(id)sender {}
- (BOOL)commitEditing { return YES; }
- (void)discardEditing {}
@end

@interface NSTitlebarAccessoryViewController : NSViewController
@property(nonatomic) NSInteger layoutAttribute; /* NSLayoutAttribute: bottom 4 */
@property(nonatomic) CGFloat fullScreenMinHeight;
@property(nonatomic, getter=isHidden) BOOL hidden;
@property(nonatomic) BOOL automaticallyAdjustsSize;
@end
@implementation NSTitlebarAccessoryViewController
- (instancetype)initWithNibName:(NSString *)nibName bundle:(NSBundle *)bundle {
    if ((self = [super initWithNibName:nibName bundle:bundle])) {
        _layoutAttribute = 4;
        _automaticallyAdjustsSize = YES;
    }
    return self;
}
@end

@interface NSWindowController : NSResponder
- (instancetype)initWithWindow:(NSWindow *)window;
@property(nonatomic, strong) NSWindow *window;
@end
@implementation NSWindowController {
    NSWindow *_window;
    NSString *_nibName, *_autosaveName;
    __weak id _owner, _document;
    NSViewController *_contentViewController;
    BOOL _cascade, _closesDocument;
}
- (instancetype)initWithWindow:(NSWindow *)window {
    if ((self = [super init])) {
        _cascade = YES;
        self.window = window;
    }
    return self;
}
- (instancetype)init { return [self initWithWindow:nil]; }
- (instancetype)initWithCoder:(NSCoder *)coder { return [self initWithWindow:nil]; }
- (instancetype)initWithWindowNibName:(NSString *)name owner:(id)owner {
    if ((self = [self initWithWindow:nil])) {
        _nibName = [name copy];
        _owner = owner;
    }
    return self;
}
- (instancetype)initWithWindowNibName:(NSString *)name { return [self initWithWindowNibName:name owner:nil]; }
- (instancetype)initWithWindowNibPath:(NSString *)path owner:(id)owner { return [self initWithWindowNibName:path owner:owner]; }
- (NSWindow *)window {
    if (!_window && _nibName) {
        NSString *nib = _nibName;
        _nibName = nil; /* tried once */
        [self windowWillLoad];
        [self loadWindow];
        hk_log("NSWindowController: nib \"%s\" not loaded", nib.UTF8String);
        if (_window) [self windowDidLoad];
    }
    return _window;
}
- (void)setWindow:(NSWindow *)window {
    _window = window;
    if ([window respondsToSelector:@selector(setWindowController:)]) [(id)window setWindowController:self];
}
- (BOOL)isWindowLoaded { return _window != nil; }
- (void)loadWindow {}
- (void)windowWillLoad {}
- (void)windowDidLoad {}
- (void)showWindow:(id)sender { [self.window makeKeyAndOrderFront:sender]; }
- (void)close { [_window close]; }
- (NSString *)windowNibName { return _nibName; }
- (NSString *)windowNibPath { return nil; }
- (id)owner { return _owner ?: self; }
- (id)document { return _document; }
- (void)setDocument:(id)document { _document = document; }
- (void)setDocumentEdited:(BOOL)flag {}
- (BOOL)shouldCascadeWindows { return _cascade; }
- (void)setShouldCascadeWindows:(BOOL)flag { _cascade = flag; }
- (BOOL)shouldCloseDocument { return _closesDocument; }
- (void)setShouldCloseDocument:(BOOL)flag { _closesDocument = flag; }
- (NSString *)windowFrameAutosaveName { return _autosaveName ?: @""; }
- (void)setWindowFrameAutosaveName:(NSString *)name { _autosaveName = [name copy]; }
- (NSViewController *)contentViewController { return _contentViewController; }
- (void)setContentViewController:(NSViewController *)vc {
    _contentViewController = vc;
    if (vc) self.window.contentView = vc.view;
}
- (void)synchronizeWindowTitleWithDocumentName {}
- (NSString *)windowTitleForDocumentDisplayName:(NSString *)name { return name; }
- (void)dismissController:(id)sender { [self close]; }
@end

/* ================================================================ panels, alerts, printing: nothing to show them on */

@interface NSAlert : HKShimObject
@property(nonatomic, copy) NSString *messageText, *informativeText, *helpAnchor;
@property(nonatomic, strong) NSImage *icon;
@property(nonatomic) NSUInteger alertStyle;
@property(nonatomic) BOOL showsHelp, showsSuppressionButton;
@property(nonatomic, weak) id delegate;
@property(nonatomic, strong) NSView *accessoryView;
@end
@implementation NSAlert {
    NSMutableArray<NSButton *> *_buttons;
    NSButton *_suppression;
}
+ (NSAlert *)alertWithError:(NSError *)error {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = error.localizedDescription ?: @"";
    a.informativeText = error.localizedRecoverySuggestion ?: @"";
    return a;
}
+ (NSAlert *)alertWithMessageText:(NSString *)message defaultButton:(NSString *)def alternateButton:(NSString *)alt
                      otherButton:(NSString *)other informativeTextWithFormat:(NSString *)format, ... {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = message ?: @"";
    if (format) {
        va_list ap;
        va_start(ap, format);
        a.informativeText = [[NSString alloc] initWithFormat:format arguments:ap];
        va_end(ap);
    }
    for (NSString *t in @[ def ?: @"OK", alt ?: @"", other ?: @"" ])
        if (t.length) [a addButtonWithTitle:t];
    return a;
}
- (instancetype)init {
    if ((self = [super init])) {
        _buttons = [NSMutableArray array];
        _messageText = _informativeText = @"";
    }
    return self;
}
- (NSArray<NSButton *> *)buttons { return [_buttons copy]; }
- (NSButton *)addButtonWithTitle:(NSString *)title {
    NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 80, 24)];
    b.title = title ?: @"";
    b.tag = 1000 + (NSInteger)_buttons.count;
    if (!_buttons.count) b.keyEquivalent = @"\r";
    [_buttons addObject:b];
    return b;
}
- (NSButton *)suppressionButton {
    if (!_suppression) {
        _suppression = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 200, 18)];
        [_suppression setButtonType:3 /* NSButtonTypeSwitch */];
        _suppression.title = @"Do not show this message again";
    }
    return _suppression;
}
- (NSWindow *)window { return nil; }
- (void)layout {}
/* Answered as if its first button were pressed. */
- (NSModalResponse)runModal {
    hk_log("NSAlert: \"%s\" (%s) answered with its first button", _messageText.UTF8String, _informativeText.UTF8String);
    return 1000;
}
- (void)beginSheetModalForWindow:(NSWindow *)window completionHandler:(void (^)(NSModalResponse))handler {
    NSModalResponse r = [self runModal];
    if (handler) on_main(^{ handler(r); });
}
- (void)beginSheetModalForWindow:(NSWindow *)window modalDelegate:(id)delegate didEndSelector:(SEL)sel contextInfo:(void *)info {
    NSModalResponse r = [self runModal];
    if (delegate && sel)
        on_main(^{ ((void (*)(id, SEL, id, NSModalResponse, void *))objc_msgSend)(delegate, sel, self, r, info); });
}
@end

@interface NSSavePanel : HKShimObject
@property(nonatomic, copy) NSURL *directoryURL;
@property(nonatomic, copy) NSString *nameFieldStringValue, *nameFieldLabel, *message, *prompt, *title, *identifier;
@property(nonatomic, copy) NSArray *allowedContentTypes, *allowedFileTypes;
@property(nonatomic) BOOL allowsOtherFileTypes, canCreateDirectories, canSelectHiddenExtension, extensionHidden;
@property(nonatomic) BOOL treatsFilePackagesAsDirectories, showsHiddenFiles, showsTagField, showsContentTypes;
@property(nonatomic, weak) id delegate;
@property(nonatomic, strong) NSView *accessoryView;
@property(nonatomic, readonly) NSURL *URL;
@end
@implementation NSSavePanel
+ (instancetype)savePanel { return [[self alloc] init]; }
- (NSModalResponse)runModal {
    hk_log("%s: answered as cancelled", object_getClassName(self));
    return 0;
}
- (NSModalResponse)runModalForDirectory:(NSString *)path file:(NSString *)name { return [self runModal]; }
- (void)beginSheetModalForWindow:(NSWindow *)window completionHandler:(void (^)(NSModalResponse))handler {
    NSModalResponse r = [self runModal];
    if (handler) on_main(^{ handler(r); });
}
- (void)beginWithCompletionHandler:(void (^)(NSModalResponse))handler { [self beginSheetModalForWindow:nil completionHandler:handler]; }
- (void)ok:(id)sender {}
- (void)cancel:(id)sender {}
- (void)validateVisibleColumns {}
- (BOOL)isExpanded { return NO; }
@end

@interface NSOpenPanel : NSSavePanel
@property(nonatomic) BOOL canChooseFiles, canChooseDirectories, resolvesAliases, allowsMultipleSelection;
@property(nonatomic) BOOL canDownloadUbiquitousContents, canResolveUbiquitousConflicts, accessoryViewDisclosed;
@property(nonatomic, readonly) NSArray<NSURL *> *URLs;
@end
@implementation NSOpenPanel
+ (instancetype)openPanel { return [[self alloc] init]; }
- (instancetype)init {
    if ((self = [super init])) {
        _canChooseFiles = YES;
        _resolvesAliases = YES;
    }
    return self;
}
- (NSArray<NSURL *> *)URLs { return @[]; }
@end

@interface NSPrintInfo : HKShimObject <NSCopying>
@end
@implementation NSPrintInfo {
    NSMutableDictionary *_dict;
}
static NSPrintInfo *g_shared_print_info;
+ (NSPrintInfo *)sharedPrintInfo {
    if (!g_shared_print_info) g_shared_print_info = [[NSPrintInfo alloc] init];
    return g_shared_print_info;
}
+ (void)setSharedPrintInfo:(NSPrintInfo *)info { g_shared_print_info = info; }
+ (id)defaultPrinter { return nil; }
- (instancetype)initWithDictionary:(NSDictionary *)attributes {
    if ((self = [super init])) {
        _dict = [@{
            @"NSPaperSize" : [NSValue valueWithBytes:&(CGSize){612, 792} objCType:@encode(CGSize)],
            @"NSPaperName" : @"na-letter",
            @"NSOrientation" : @0,
            @"NSLeftMargin" : @72, @"NSRightMargin" : @72, @"NSTopMargin" : @90, @"NSBottomMargin" : @90,
            @"NSScalingFactor" : @1.0, @"NSCopies" : @1, @"NSAllPages" : @YES,
        } mutableCopy];
        if (attributes) [_dict addEntriesFromDictionary:attributes];
    }
    return self;
}
- (instancetype)init { return [self initWithDictionary:nil]; }
- (id)copyWithZone:(NSZone *)zone { return [[NSPrintInfo alloc] initWithDictionary:_dict]; }
- (NSMutableDictionary *)dictionary { return _dict; }
- (NSSize)paperSize {
    CGSize s = {612, 792};
    [_dict[@"NSPaperSize"] getValue:&s size:sizeof s];
    return s;
}
- (void)setPaperSize:(NSSize)size { _dict[@"NSPaperSize"] = [NSValue valueWithBytes:&size objCType:@encode(CGSize)]; }
- (NSString *)paperName { return _dict[@"NSPaperName"]; }
- (void)setPaperName:(NSString *)name { _dict[@"NSPaperName"] = name ?: @""; }
- (NSInteger)orientation { return [_dict[@"NSOrientation"] integerValue]; }
- (void)setOrientation:(NSInteger)o { _dict[@"NSOrientation"] = @(o); }
#define MARGIN(get, set, key) \
    -(CGFloat)get { return [_dict[@key] doubleValue]; } \
    -(void)set:(CGFloat)v { _dict[@key] = @(v); }
MARGIN(leftMargin, setLeftMargin, "NSLeftMargin") MARGIN(rightMargin, setRightMargin, "NSRightMargin")
MARGIN(topMargin, setTopMargin, "NSTopMargin") MARGIN(bottomMargin, setBottomMargin, "NSBottomMargin")
MARGIN(scalingFactor, setScalingFactor, "NSScalingFactor")
#undef MARGIN
- (NSRect)imageablePageBounds {
    NSSize s = self.paperSize;
    return NSMakeRect(18, 18, s.width - 36, s.height - 36);
}
- (NSInteger)horizontalPagination { return 0; }
- (void)setHorizontalPagination:(NSInteger)p {}
- (NSInteger)verticalPagination { return 0; }
- (void)setVerticalPagination:(NSInteger)p {}
- (BOOL)isHorizontallyCentered { return YES; }
- (void)setHorizontallyCentered:(BOOL)flag {}
- (BOOL)isVerticallyCentered { return YES; }
- (void)setVerticallyCentered:(BOOL)flag {}
- (BOOL)isSelectionOnly { return NO; }
- (void)setSelectionOnly:(BOOL)flag {}
- (id)printer { return nil; }
- (void)setPrinter:(id)printer {}
- (NSString *)jobDisposition { return @"NSPrintSpoolJob"; }
- (void)setJobDisposition:(NSString *)d {}
- (void *)PMPrintSession { return NULL; }
- (void *)PMPageFormat { return NULL; }
- (void *)PMPrintSettings { return NULL; }
- (void)updateFromPMPageFormat {}
- (void)updateFromPMPrintSettings {}
- (void)setUpPrintOperationDefaultValues {}
- (NSString *)localizedPaperName { return self.paperName; }
@end

@interface NSPrintPanel : HKShimObject
@property(nonatomic) NSUInteger options;
@property(nonatomic, copy) NSString *helpAnchor, *jobStyleHint;
@end
@implementation NSPrintPanel
+ (NSPrintPanel *)printPanel { return [[self alloc] init]; }
- (NSArray *)accessoryControllers { return @[]; }
- (void)addAccessoryController:(id)controller {}
- (void)removeAccessoryController:(id)controller {}
- (NSInteger)runModalWithPrintInfo:(NSPrintInfo *)info { return 0; }
- (NSInteger)runModal { return 0; }
- (NSPrintInfo *)printInfo { return [NSPrintInfo sharedPrintInfo]; }
- (void)beginSheetWithPrintInfo:(NSPrintInfo *)info modalForWindow:(NSWindow *)window delegate:(id)delegate
                 didEndSelector:(SEL)sel contextInfo:(void *)context {
    if (delegate && sel) on_main(^{ ((void (*)(id, SEL, id, NSInteger, void *))objc_msgSend)(delegate, sel, self, 0, context); });
}
- (void)beginSheetUsingPrintInfo:(NSPrintInfo *)info onWindow:(NSWindow *)window completionHandler:(void (^)(NSInteger))handler {
    if (handler) on_main(^{ handler(0); });
}
@end

/* ================================================================ the status bar (items exist but are not shown) */

@interface NSStatusBar : HKShimObject
@end
@interface NSStatusItem : HKShimObject
@property(nonatomic, weak) NSStatusBar *statusBar;
@property(nonatomic) CGFloat length;
@property(nonatomic, strong) NSMenu *menu;
@property(nonatomic, strong) NSView *view;
@property(nonatomic, getter=isVisible) BOOL visible;
@property(nonatomic) NSUInteger behavior;
@property(nonatomic, copy) NSString *autosaveName;
@property(nonatomic, strong) NSAppearance *appearance;
@end
@implementation NSStatusItem {
    NSStatusBarButton *_button;
}
- (instancetype)init {
    if ((self = [super init])) {
        _visible = YES;
        _autosaveName = @"";
    }
    return self;
}
- (NSStatusBarButton *)button {
    if (!_button) _button = [[NSStatusBarButton alloc] initWithFrame:NSMakeRect(0, 0, _length > 0 ? _length : 22, 22)];
    return _button;
}
- (NSImage *)image { return self.button.image; }
- (void)setImage:(NSImage *)image { self.button.image = image; }
- (NSString *)title { return self.button.title; }
- (void)setTitle:(NSString *)title { self.button.title = title; }
- (NSString *)toolTip { return self.button.toolTip; }
- (void)setToolTip:(NSString *)t { self.button.toolTip = t; }
- (id)target { return self.button.target; }
- (void)setTarget:(id)t { self.button.target = t; }
- (SEL)action { return self.button.action; }
- (void)setAction:(SEL)a { self.button.action = a; }
- (BOOL)highlightMode { return NO; }
- (void)setHighlightMode:(BOOL)flag {}
- (NSAppearance *)effectiveAppearance { return _appearance ?: [NSAppearance currentDrawingAppearance]; }
- (BOOL)allowsVibrancy { return NO; }
- (void)popUpStatusItemMenu:(NSMenu *)menu {}
- (void)drawStatusBarBackgroundInRect:(NSRect)rect withHighlight:(BOOL)highlight {}
- (NSInteger)sendActionOn:(NSInteger)mask { return 0; }
@end
@implementation NSStatusBar {
    NSMutableArray<NSStatusItem *> *_items;
}
+ (NSStatusBar *)systemStatusBar {
    static NSStatusBar *bar;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        bar = [[NSStatusBar alloc] init];
        bar->_items = [NSMutableArray array];
    });
    return bar;
}
- (NSStatusItem *)statusItemWithLength:(CGFloat)length {
    NSStatusItem *i = [[NSStatusItem alloc] init];
    i.length = length;
    i.statusBar = self;
    [_items addObject:i];
    return i;
}
- (void)removeStatusItem:(NSStatusItem *)item { [_items removeObjectIdenticalTo:item]; }
- (BOOL)isVertical { return NO; }
- (CGFloat)thickness { return 22; }
@end

/* ================================================================ the Touch Bar (there is none) */

@interface NSTouchBar : HKShimObject
@property(nonatomic, weak) id delegate;
@property(nonatomic, copy) NSString *customizationIdentifier, *principalItemIdentifier, *escapeKeyReplacementItemIdentifier;
@property(nonatomic, copy) NSArray<NSString *> *defaultItemIdentifiers, *customizationAllowedItemIdentifiers, *customizationRequiredItemIdentifiers;
@property(nonatomic, copy) NSSet *templateItems;
@end
@interface NSTouchBarItem : HKShimObject
- (instancetype)initWithIdentifier:(NSString *)identifier;
@property(nonatomic, readonly, copy) NSString *identifier;
@property(nonatomic) float visibilityPriority;
@property(nonatomic, copy) NSString *customizationLabel;
@end
@implementation NSTouchBar
static BOOL g_customize_touch_bar_item;
+ (BOOL)isAutomaticCustomizeTouchBarMenuItemEnabled { return g_customize_touch_bar_item; }
+ (void)setAutomaticCustomizeTouchBarMenuItemEnabled:(BOOL)flag { g_customize_touch_bar_item = flag; }
+ (BOOL)allowsTouchesDuringTrackingLoops { return NO; }
- (NSArray<NSString *> *)itemIdentifiers { return self.defaultItemIdentifiers ?: @[]; }
- (BOOL)isVisible { return NO; }
- (id)itemForIdentifier:(NSString *)identifier {
    for (NSTouchBarItem *i in self.templateItems)
        if ([i.identifier isEqualToString:identifier]) return i;
    id d = self.delegate;
    return [d respondsToSelector:@selector(touchBar:makeItemForIdentifier:)] ? [d touchBar:self makeItemForIdentifier:identifier] : nil;
}
@end
@implementation NSTouchBarItem
- (instancetype)initWithIdentifier:(NSString *)identifier {
    if ((self = [super init])) _identifier = [identifier copy];
    return self;
}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self initWithIdentifier:@""]; }
- (NSView *)view { return nil; }
- (NSViewController *)viewController { return nil; }
- (BOOL)isVisible { return NO; }
@end

@interface NSCustomTouchBarItem : NSTouchBarItem
@property(nonatomic, strong) NSView *view;
@property(nonatomic, strong) NSViewController *viewController;
@end
@implementation NSCustomTouchBarItem
@synthesize view = _view, viewController = _viewController;
@end

@interface NSGroupTouchBarItem : NSTouchBarItem
@property(nonatomic, strong) NSTouchBar *groupTouchBar;
@property(nonatomic) NSInteger groupUserInterfaceLayoutDirection;
@property(nonatomic) BOOL prefersEqualWidths;
@property(nonatomic) CGFloat preferredItemWidth;
@end
@implementation NSGroupTouchBarItem
+ (instancetype)groupItemWithIdentifier:(NSString *)identifier items:(NSArray<NSTouchBarItem *> *)items {
    NSGroupTouchBarItem *g = [[self alloc] initWithIdentifier:identifier];
    g.groupTouchBar = [[NSTouchBar alloc] init];
    g.groupTouchBar.templateItems = [NSSet setWithArray:items ?: @[]];
    g.groupTouchBar.defaultItemIdentifiers = [items valueForKey:@"identifier"];
    return g;
}
+ (instancetype)groupItemWithIdentifier:(NSString *)identifier items:(NSArray<NSTouchBarItem *> *)items
              allowedCompressionOptions:(id)options {
    return [self groupItemWithIdentifier:identifier items:items];
}
+ (instancetype)alertStyleGroupItemWithIdentifier:(NSString *)identifier { return [self groupItemWithIdentifier:identifier items:@[]]; }
@end

@interface NSCandidateListTouchBarItem : NSTouchBarItem
@property(nonatomic, weak) id client, delegate;
@property(nonatomic, getter=isCollapsed) BOOL collapsed;
@property(nonatomic) BOOL allowsCollapsing, allowsTextInputContextCandidates;
@property(nonatomic, copy) NSAttributedString * (^attributedStringForCandidate)(id candidate, NSInteger index);
@property(nonatomic, readonly, copy) NSArray *candidates;
@end
@implementation NSCandidateListTouchBarItem {
    NSArray *_candidates;
}
- (instancetype)initWithIdentifier:(NSString *)identifier {
    if ((self = [super initWithIdentifier:identifier])) _allowsCollapsing = _allowsTextInputContextCandidates = YES;
    return self;
}
- (NSArray *)candidates { return _candidates ?: @[]; }
- (void)setCandidates:(NSArray *)candidates forSelectedRange:(NSRange)range inString:(NSString *)string { _candidates = [candidates copy]; }
- (BOOL)isCandidateListVisible { return NO; }
- (void)updateWithInsertionPointVisibility:(BOOL)visible {}
@end

/* ================================================================ sharing, speech, color sampling, dragging, toolbars, popovers */

@interface NSSharingService : HKShimObject
@property(nonatomic, weak) id delegate;
@property(nonatomic, readonly, copy) NSString *title;
@property(nonatomic, readonly, strong) NSImage *image, *alternateImage;
@property(nonatomic, copy) NSString *menuItemTitle, *subject, *messageBody, *accountName;
@property(nonatomic, copy) NSArray *recipients, *attachmentFileURLs;
@property(nonatomic, readonly, copy) NSURL *permanentLink;
@end
@implementation NSSharingService
+ (NSArray<NSSharingService *> *)sharingServicesForItems:(NSArray *)items { return @[]; }
+ (NSSharingService *)sharingServiceNamed:(NSString *)name { return nil; }
- (instancetype)initWithTitle:(NSString *)title image:(NSImage *)image alternateImage:(NSImage *)alternate handler:(void (^)(void))block {
    if ((self = [super init])) {
        _title = [title copy];
        _menuItemTitle = [title copy];
        _image = image;
        _alternateImage = alternate;
    }
    return self;
}
- (BOOL)canPerformWithItems:(NSArray *)items { return NO; }
- (void)performWithItems:(NSArray *)items {
    id d = self.delegate;
    if ([d respondsToSelector:@selector(sharingService:didFailToShareItems:error:)])
        on_main(^{ [d sharingService:self didFailToShareItems:items error:unsupported_error()]; });
}
@end

@interface NSSharingServicePicker : HKShimObject
@property(nonatomic, weak) id delegate;
@end
@implementation NSSharingServicePicker {
    NSArray *_items;
}
- (instancetype)initWithItems:(NSArray *)items {
    if ((self = [super init])) _items = [items copy];
    return self;
}
- (void)showRelativeToRect:(NSRect)rect ofView:(NSView *)view preferredEdge:(NSRectEdge)edge {
    hk_log("NSSharingServicePicker: not shown");
}
- (void)close {}
- (NSMenuItem *)standardShareMenuItem {
    NSMenuItem *m = [[NSMenuItem alloc] initWithTitle:@"Share" action:NULL keyEquivalent:@""];
    m.enabled = NO;
    return m;
}
@end

@interface NSSpeechSynthesizer : HKShimObject
@property(nonatomic, weak) id delegate;
@property(nonatomic) float rate, volume;
@property(nonatomic) BOOL usesFeedbackWindow;
@end
@implementation NSSpeechSynthesizer {
    NSString *_voice;
}
+ (BOOL)isAnyApplicationSpeaking { return NO; }
+ (NSArray<NSString *> *)availableVoices { return @[]; }
+ (NSString *)defaultVoice { return nil; }
+ (NSDictionary *)attributesForVoice:(NSString *)voice { return @{}; }
- (instancetype)initWithVoice:(NSString *)voice {
    if ((self = [super init])) {
        _voice = [voice copy];
        _rate = 180;
        _volume = 1;
    }
    return self;
}
- (instancetype)init { return [self initWithVoice:nil]; }
- (NSString *)voice { return _voice; }
- (BOOL)setVoice:(NSString *)voice {
    _voice = [voice copy];
    return YES;
}
- (BOOL)isSpeaking { return NO; }
- (BOOL)startSpeakingString:(NSString *)string {
    id d = self.delegate;
    if ([d respondsToSelector:@selector(speechSynthesizer:didFinishSpeaking:)]) on_main(^{ [d speechSynthesizer:self didFinishSpeaking:NO]; });
    return NO;
}
- (BOOL)startSpeakingString:(NSString *)string toURL:(NSURL *)url { return NO; }
- (void)stopSpeaking {}
- (void)stopSpeakingAtBoundary:(NSInteger)boundary {}
- (void)pauseSpeakingAtBoundary:(NSInteger)boundary {}
- (void)continueSpeaking {}
- (id)objectForProperty:(NSString *)property error:(NSError **)error { return nil; }
- (BOOL)setObject:(id)object forProperty:(NSString *)property error:(NSError **)error { return NO; }
@end

@interface NSColorSampler : HKShimObject
@end
@implementation NSColorSampler
- (void)showSamplerWithSelectionHandler:(void (^)(NSColor *))handler {
    if (handler) on_main(^{ handler(nil); });
}
@end

@interface NSDraggingItem : HKShimObject
@property(nonatomic) NSRect draggingFrame;
@property(nonatomic, copy) NSArray * (^imageComponentsProvider)(void);
@end
@implementation NSDraggingItem {
    id _item;
    id _contents;
}
- (instancetype)initWithPasteboardWriter:(id)writer {
    if ((self = [super init])) _item = writer;
    return self;
}
- (id)item { return _item; }
- (NSArray *)imageComponents { return self.imageComponentsProvider ? self.imageComponentsProvider() : @[]; }
- (void)setDraggingFrame:(NSRect)frame contents:(id)contents {
    self.draggingFrame = frame;
    _contents = contents;
}
@end

@interface NSPreviewRepresentingActivityItem : HKShimObject
@property(nonatomic, readonly, strong) id item;
@property(nonatomic, readonly, copy) NSString *title;
@property(nonatomic, readonly, strong) id imageProvider, iconProvider;
@end
@implementation NSPreviewRepresentingActivityItem
- (instancetype)initWithItem:(id)item title:(NSString *)title imageProvider:(id)image iconProvider:(id)icon {
    if ((self = [super init])) {
        _item = item;
        _title = [title copy];
        _imageProvider = image;
        _iconProvider = icon;
    }
    return self;
}
- (instancetype)initWithItem:(id)item title:(NSString *)title image:(NSImage *)image icon:(NSImage *)icon {
    return [self initWithItem:item title:title imageProvider:image iconProvider:icon];
}
@end

@interface NSToolbar : HKShimObject
@property(nonatomic, weak) id delegate;
@property(nonatomic, getter=isVisible) BOOL visible;
@property(nonatomic) BOOL showsBaselineSeparator, allowsUserCustomization, autosavesConfiguration, allowsExtensionItems;
@property(nonatomic) NSUInteger displayMode, sizeMode;
@property(nonatomic, copy) NSString *selectedItemIdentifier, *centeredItemIdentifier;
@property(nonatomic, copy) NSSet *centeredItemIdentifiers;
@end
@implementation NSToolbar {
    NSString *_identifier;
    NSMutableArray *_items;
}
- (instancetype)initWithIdentifier:(NSString *)identifier {
    if ((self = [super init])) {
        _identifier = [identifier copy] ?: @"";
        _items = [NSMutableArray array];
        _visible = YES;
        _showsBaselineSeparator = YES;
    }
    return self;
}
- (instancetype)init { return [self initWithIdentifier:@""]; }
- (NSString *)identifier { return _identifier; }
- (NSArray *)items { return [_items copy]; }
- (NSArray *)visibleItems { return [_items copy]; }
- (void)insertItemWithItemIdentifier:(NSString *)identifier atIndex:(NSInteger)index {}
- (void)removeItemAtIndex:(NSInteger)index {
    if (index >= 0 && index < (NSInteger)_items.count) [_items removeObjectAtIndex:(NSUInteger)index];
}
- (void)validateVisibleItems {}
- (void)runCustomizationPalette:(id)sender {}
- (BOOL)customizationPaletteIsRunning { return NO; }
- (NSDictionary *)configurationDictionary { return @{}; }
- (void)setConfigurationFromDictionary:(NSDictionary *)d {}
@end

@interface NSPopover : NSResponder
@property(nonatomic, strong) NSViewController *contentViewController;
@property(nonatomic) NSInteger behavior;
@property(nonatomic) BOOL animates, hasFullSizeContent;
@property(nonatomic) NSSize contentSize;
@property(nonatomic) NSRect positioningRect;
@property(nonatomic, weak) id delegate;
@property(nonatomic, strong) NSAppearance *appearance;
@property(nonatomic, readonly, getter=isShown) BOOL shown;
@end
@implementation NSPopover
- (instancetype)init {
    if ((self = [super init])) _animates = YES;
    return self;
}
- (NSAppearance *)effectiveAppearance { return _appearance ?: [NSAppearance currentDrawingAppearance]; }
- (BOOL)isDetached { return NO; }
- (void)showRelativeToRect:(NSRect)rect ofView:(NSView *)view preferredEdge:(NSRectEdge)edge {
    _positioningRect = rect;
    hk_log("NSPopover: %s not shown", object_getClassName(_contentViewController));
}
- (void)close {
    if (!_shown) return;
    _shown = NO;
    id d = _delegate;
    if ([d respondsToSelector:@selector(popoverDidClose:)])
        [d popoverDidClose:[NSNotification notificationWithName:@"NSPopoverDidCloseNotification" object:self]];
}
- (void)performClose:(id)sender { [self close]; }
@end
