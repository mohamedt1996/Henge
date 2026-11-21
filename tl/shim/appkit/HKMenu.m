/* Menus that pop up: context menus (+[NSMenu popUpContextMenu:withEvent:forView:]), -popUpMenuPositioningItem:
   atLocation:inView:, and pop-up buttons' lists (NSPopUpButtonCell, which Chromium uses for a page's <select>).
   Henge draws such a menu in a borderless window of its own over the app's and tracks it as AppKit does, in a loop of
   its own until an item is chosen or the menu is dismissed: the pointer (a finger's tap, the trackpad's pointer) or
   the arrow keys highlight items, a press and release on an item chooses it, an item with a submenu opens that beside
   it, a press outside or Escape closes the menu with nothing chosen. The app's delegate hears menuNeedsUpdate:,
   menuWillOpen:, menu:willHighlightItem: and menuDidClose:, and the chosen item's action is sent once the menu is
   gone, as in AppKit. */
#import "HKShim.h"
#import <CoreText/CoreText.h>
#import <objc/message.h>

@interface NSMenuItem (HKMenuLook)
- (BOOL)isAlternate;
- (BOOL)isSectionHeader;
- (NSAttributedString *)attributedTitle;
- (NSInteger)indentationLevel;
@end

@interface NSPopUpButtonCell : NSCell
@property(nonatomic, strong) NSMenu *menu;
@property(nonatomic) BOOL pullsDown;
- (void)selectItem:(NSMenuItem *)item;
- (NSMenuItem *)selectedItem;
@end

static const CGFloat kRowH = 22, kSepH = 11, kPad = 5, kStateW = 22, kRightPad = 22, kRadius = 8;

static CGColorRef rgba(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGFloat c[4] = {r, g, b, a};
    return CGColorCreate(cs, c);
}

static CTFontRef menu_font(BOOL small) {
    static CTFontRef f[2];
    int i = small ? 1 : 0;
    if (!f[i]) f[i] = CTFontCreateUIFontForLanguage(small ? kCTFontUIFontSmallSystem : kCTFontUIFontMenuItem, small ? 11 : 13, NULL);
    if (!f[i]) f[i] = CTFontCreateWithName(CFSTR("Helvetica"), small ? 11 : 13, NULL);
    return f[i];
}

static CTLineRef make_line(NSString *s, CGColorRef color, BOOL small) {
    NSDictionary *a = @{(__bridge id)kCTFontAttributeName : (__bridge id)menu_font(small),
                        (__bridge id)kCTForegroundColorAttributeName : (__bridge id)color};
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:s ?: @"" attributes:a];
    return CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
}

static CGFloat text_width(NSString *s, BOOL small) {
    if (!s.length) return 0;
    CGColorRef c = rgba(0, 0, 0, 1);
    CTLineRef l = make_line(s, c, small);
    CGFloat w = CTLineGetTypographicBounds(l, NULL, NULL, NULL);
    CFRelease(l);
    CGColorRelease(c);
    return ceil(w);
}

static NSString *title_of(NSMenuItem *i) {
    NSAttributedString *a = [i respondsToSelector:@selector(attributedTitle)] ? i.attributedTitle : nil;
    return a.length ? a.string : (i.title ?: @"");
}

static BOOL is_header(NSMenuItem *i) { return [i respondsToSelector:@selector(isSectionHeader)] && i.isSectionHeader; }

/* The key equivalent as a Mac menu shows it: ⌃⌥⇧⌘ and the key. */
static NSString *key_text(NSMenuItem *i) {
    NSString *k = i.keyEquivalent;
    if (!k.length) return @"";
    NSEventModifierFlags m = i.keyEquivalentModifierMask;
    unichar c = [k characterAtIndex:0];
    BOOL upper = c < 128 && isupper(c);
    NSMutableString *s = [NSMutableString string];
    if (m & NSEventModifierFlagControl) [s appendString:@"⌃"];
    if (m & NSEventModifierFlagOption) [s appendString:@"⌥"];
    if ((m & NSEventModifierFlagShift) || upper) [s appendString:@"⇧"];
    if (m & NSEventModifierFlagCommand) [s appendString:@"⌘"];
    switch (c) {
    case '\r': case 3: [s appendString:@"↩"]; break;
    case 27: [s appendString:@"⎋"]; break;
    case 8: case 127: [s appendString:@"⌫"]; break;
    case 0xF728: [s appendString:@"⌦"]; break;
    case '\t': [s appendString:@"⇥"]; break;
    case ' ': [s appendString:@"Space"]; break;
    case 0xF700: [s appendString:@"↑"]; break;
    case 0xF701: [s appendString:@"↓"]; break;
    case 0xF702: [s appendString:@"←"]; break;
    case 0xF703: [s appendString:@"→"]; break;
    default:
        if (c >= 0xF704 && c <= 0xF71B) [s appendFormat:@"F%d", c - 0xF704 + 1];
        else [s appendString:k.uppercaseString];
    }
    return s;
}

/* ---- a menu's view: its items, laid out from the top, one highlighted ---- */

@interface HKMenuView : NSView
@end

@implementation HKMenuView {
  @public
    NSMenu *_menu;
    NSMutableArray<NSMenuItem *> *_items;
    NSMutableData *_rows; /* each item's rectangle (NSRect), in the view (y up) */
    NSInteger _hi;
    CGFloat _w, _h;
    NSWindow *_window;
}

- (instancetype)initWithMenu:(NSMenu *)menu skipFirst:(BOOL)skip minWidth:(CGFloat)minWidth {
    NSMutableArray *items = [NSMutableArray array];
    NSArray *all = menu.itemArray;
    for (NSUInteger k = skip ? 1 : 0; k < all.count; k++) {
        NSMenuItem *i = all[k];
        if (i.isHidden) continue;
        if ([i respondsToSelector:@selector(isAlternate)] && i.isAlternate) continue;
        [items addObject:i];
    }
    CGFloat titles = 0, keys = 0, h = 2 * kPad;
    BOOL arrows = NO;
    for (NSMenuItem *i in items) {
        if (i.isSeparatorItem) { h += kSepH; continue; }
        h += kRowH;
        CGFloat indent = [i respondsToSelector:@selector(indentationLevel)] ? 10 * i.indentationLevel : 0;
        titles = fmax(titles, text_width(title_of(i), is_header(i)) + indent);
        keys = fmax(keys, text_width(key_text(i), NO));
        if (i.submenu) arrows = YES;
    }
    CGFloat w = kStateW + titles + (keys ? keys + 28 : 0) + (arrows ? 22 : 0) + kRightPad;
    w = ceil(fmax(fmax(w, minWidth), 120));
    if ((self = [super initWithFrame:NSMakeRect(0, 0, w, h)])) {
        _menu = menu;
        _items = items;
        _w = w, _h = h, _hi = -1;
        _rows = [NSMutableData dataWithLength:items.count * sizeof(NSRect)];
        NSRect *rr = _rows.mutableBytes;
        NSUInteger n = 0;
        CGFloat y = h - kPad;
        for (NSMenuItem *i in items) {
            CGFloat rh = i.isSeparatorItem ? kSepH : kRowH;
            y -= rh;
            rr[n++] = NSMakeRect(0, y, w, rh);
        }
    }
    return self;
}

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (NSRect)rowRect:(NSInteger)i { return ((const NSRect *)_rows.bytes)[i]; }

/* The row a point in the view is on, or -1. */
- (NSInteger)rowAt:(NSPoint)p {
    for (NSUInteger i = 0; i < _items.count; i++)
        if (NSPointInRect(p, [self rowRect:(NSInteger)i])) return (NSInteger)i;
    return -1;
}

- (BOOL)selectable:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_items.count) return NO;
    NSMenuItem *it = _items[(NSUInteger)i];
    return !it.isSeparatorItem && !is_header(it) && it.isEnabled;
}

/* How far below the menu's top an item's row starts (for positioning an item at a point). */
- (CGFloat)topOffsetOf:(NSMenuItem *)item {
    NSUInteger i = [_items indexOfObjectIdenticalTo:item];
    return i == NSNotFound ? 0 : _h - NSMaxY([self rowRect:(NSInteger)i]);
}

- (void)setHighlight:(NSInteger)i {
    if (![self selectable:i]) i = -1;
    if (i == _hi) return;
    _hi = i;
    id d = _menu.delegate;
    if ([d respondsToSelector:@selector(menu:willHighlightItem:)])
        ((void (*)(id, SEL, id, id))objc_msgSend)(d, @selector(menu:willHighlightItem:), _menu, i >= 0 ? _items[(NSUInteger)i] : nil);
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect {} /* drawn in drawLayer:inContext: (a drawRect: of its own makes Henge draw it) */

- (void)drawLayer:(CALayer *)layer inContext:(CGContextRef)ctx {
    CGContextSaveGState(ctx);
    CGRect b = CGRectMake(0, 0, _w, _h);
    CGPathRef bg = CGPathCreateWithRoundedRect(CGRectInset(b, 0.5, 0.5), kRadius, kRadius, NULL);
    CGColorRef fill = rgba(0.965, 0.965, 0.965, 1), edge = rgba(0, 0, 0, 0.22);
    CGContextAddPath(ctx, bg);
    CGContextSetFillColorWithColor(ctx, fill);
    CGContextFillPath(ctx);
    CGContextAddPath(ctx, bg);
    CGContextSetStrokeColorWithColor(ctx, edge);
    CGContextSetLineWidth(ctx, 1);
    CGContextStrokePath(ctx);
    CGPathRelease(bg);
    CGColorRelease(fill);
    CGColorRelease(edge);
    CGColorRef text = rgba(0, 0, 0, 0.88), dim = rgba(0, 0, 0, 0.3), white = rgba(1, 1, 1, 1), head = rgba(0, 0, 0, 0.5),
               accent = rgba(0.0, 0.45, 0.95, 1), sep = rgba(0, 0, 0, 0.12);
    CGFloat ascent = CTFontGetAscent(menu_font(NO)), descent = CTFontGetDescent(menu_font(NO));
    for (NSUInteger k = 0; k < _items.count; k++) {
        NSMenuItem *it = _items[k];
        NSRect r = [self rowRect:(NSInteger)k];
        if (it.isSeparatorItem) {
            CGFloat y = floor(NSMidY(r)) + 0.5;
            CGContextSetStrokeColorWithColor(ctx, sep);
            CGContextSetLineWidth(ctx, 1);
            CGContextMoveToPoint(ctx, 10, y);
            CGContextAddLineToPoint(ctx, _w - 10, y);
            CGContextStrokePath(ctx);
            continue;
        }
        BOOL on = (NSInteger)k == _hi;
        if (on) {
            CGPathRef hp = CGPathCreateWithRoundedRect(CGRectMake(5, r.origin.y, _w - 10, r.size.height), 5, 5, NULL);
            CGContextAddPath(ctx, hp);
            CGContextSetFillColorWithColor(ctx, accent);
            CGContextFillPath(ctx);
            CGPathRelease(hp);
        }
        BOOL header = is_header(it);
        CGColorRef color = on ? white : header ? head : it.isEnabled ? text : dim;
        CGFloat base = r.origin.y + floor((r.size.height - (ascent + descent)) / 2 + descent);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        if (it.state != 0 && !header) {
            CTLineRef l = make_line(it.state > 0 ? @"✓" : @"–", color, NO);
            CGContextSetTextPosition(ctx, 8, base);
            CTLineDraw(l, ctx);
            CFRelease(l);
        }
        CGFloat indent = [it respondsToSelector:@selector(indentationLevel)] ? 10 * it.indentationLevel : 0;
        CTLineRef t = make_line(title_of(it), color, header);
        CGContextSetTextPosition(ctx, (header ? 10 : kStateW) + indent, base);
        CTLineDraw(t, ctx);
        CFRelease(t);
        NSString *keys = key_text(it);
        if (keys.length) {
            CTLineRef l = make_line(keys, on ? white : dim, NO);
            CGFloat kw = CTLineGetTypographicBounds(l, NULL, NULL, NULL);
            CGContextSetTextPosition(ctx, _w - kRightPad - kw, base);
            CTLineDraw(l, ctx);
            CFRelease(l);
        }
        if (it.submenu) { /* a chevron at the right */
            CGFloat cy = NSMidY(r), x = _w - 15;
            CGContextSetStrokeColorWithColor(ctx, color);
            CGContextSetLineWidth(ctx, 1.5);
            CGContextSetLineCap(ctx, kCGLineCapRound);
            CGContextMoveToPoint(ctx, x, cy + 4);
            CGContextAddLineToPoint(ctx, x + 4, cy);
            CGContextAddLineToPoint(ctx, x, cy - 4);
            CGContextStrokePath(ctx);
        }
    }
    CGColorRelease(text);
    CGColorRelease(dim);
    CGColorRelease(white);
    CGColorRelease(head);
    CGColorRelease(accent);
    CGColorRelease(sep);
    CGContextRestoreGState(ctx);
}
@end

/* ---- tracking ---- */

static NSMutableArray<HKMenuView *> *g_open; /* the menu and the submenus open from it, outermost first */
static BOOL g_cancel;

static NSRect screen_frame(void) { return [NSScreen mainScreen].frame; }

static void opening(NSMenu *menu) {
    [menu hk_prepare]; /* menuNeedsUpdate: and the items' enabling */
    id d = menu.delegate;
    if ([d respondsToSelector:@selector(menuWillOpen:)]) ((void (*)(id, SEL, id))objc_msgSend)(d, @selector(menuWillOpen:), menu);
}

static void closed(NSMenu *menu) {
    id d = menu.delegate;
    if ([d respondsToSelector:@selector(menuDidClose:)]) ((void (*)(id, SEL, id))objc_msgSend)(d, @selector(menuDidClose:), menu);
}

/* Shows a menu's view with its top left at a point on the screen (moved to stay on the screen). */
static void show_view(HKMenuView *v, NSPoint top_left) {
    NSRect s = screen_frame();
    NSRect f = NSMakeRect(top_left.x, top_left.y - v->_h, v->_w, v->_h);
    if (NSMaxX(f) > NSMaxX(s)) f.origin.x = NSMaxX(s) - f.size.width;
    if (f.origin.x < s.origin.x) f.origin.x = s.origin.x;
    if (f.origin.y < s.origin.y) f.origin.y = s.origin.y;
    if (NSMaxY(f) > NSMaxY(s)) f.origin.y = fmax(s.origin.y, NSMaxY(s) - f.size.height);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:f styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    w.backgroundColor = [NSColor clearColor];
    w.level = 101; /* NSPopUpMenuWindowLevel: above every other window */
    w.contentView = v;
    v->_window = w;
    [w orderFront:nil];
    [v setNeedsDisplay:YES];
    [g_open addObject:v];
}

static void close_from(NSUInteger level) {
    while (g_open.count > level) {
        HKMenuView *v = g_open.lastObject;
        [g_open removeLastObject];
        [v->_window close];
        v->_window = nil;
        closed(v->_menu);
    }
}

/* Opens the submenu of the highlighted item of a level beside it, if it is not open already. */
static void open_submenu(NSUInteger level) {
    HKMenuView *v = g_open[level];
    if (v->_hi < 0) return;
    NSMenu *sub = v->_items[(NSUInteger)v->_hi].submenu;
    if (!sub) return;
    if (g_open.count > level + 1 && g_open[level + 1]->_menu == sub) return;
    close_from(level + 1);
    opening(sub);
    HKMenuView *s = [[HKMenuView alloc] initWithMenu:sub skipFirst:NO minWidth:0];
    NSRect wf = v->_window.frame, row = [v rowRect:v->_hi];
    NSPoint at = NSMakePoint(NSMaxX(wf) - 3, wf.origin.y + NSMaxY(row) + kPad);
    if (at.x + s->_w > NSMaxX(screen_frame())) at.x = wf.origin.x - s->_w + 3;
    show_view(s, at);
}

/* The open level and row a point on the screen is over (level -1: none). */
static NSInteger hit(NSPoint p, NSInteger *row) {
    for (NSInteger l = (NSInteger)g_open.count - 1; l >= 0; l--) {
        HKMenuView *v = g_open[(NSUInteger)l];
        NSRect f = v->_window.frame;
        if (!NSPointInRect(p, f)) continue;
        *row = [v rowAt:NSMakePoint(p.x - f.origin.x, p.y - f.origin.y)];
        return l;
    }
    *row = -1;
    return -1;
}

static void hover(NSInteger level, NSInteger row) {
    if (level < 0) {
        HKMenuView *last = g_open.lastObject;
        [last setHighlight:-1];
        return;
    }
    HKMenuView *v = g_open[(NSUInteger)level];
    [v setHighlight:row];
    if (v->_hi >= 0 && v->_items[(NSUInteger)v->_hi].submenu) open_submenu((NSUInteger)level);
    else close_from((NSUInteger)level + 1);
}

static void step(int by) {
    HKMenuView *v = g_open.lastObject;
    NSInteger n = (NSInteger)v->_items.count;
    if (!n) return;
    NSInteger i = v->_hi;
    for (NSInteger k = 0; k < n; k++) {
        i = i < 0 ? (by > 0 ? 0 : n - 1) : (i + by + n) % n;
        if ([v selectable:i]) break;
    }
    [v setHighlight:i];
}

/* Tracks a menu shown with its top left at a point (or, with an item, that item's top left there) until an item is
   chosen (returned, its action not yet sent) or the menu is dismissed (nil). */
static NSMenuItem *track(NSMenu *menu, NSPoint at, NSMenuItem *positioned, BOOL skip_first, CGFloat min_width, BOOL context) {
    if (!NSApp || g_open.count) return nil; /* one menu at a time */
    if (!g_open) g_open = [NSMutableArray array];
    g_cancel = NO;
    opening(menu);
    HKMenuView *v = [[HKMenuView alloc] initWithMenu:menu skipFirst:skip_first minWidth:min_width];
    NSPoint top_left = at;
    if (positioned) top_left.y += [v topOffsetOf:positioned];
    if (context) {
        top_left.x += 1;
        /* no room below the point: above it, as AppKit does */
        if (top_left.y - v->_h < screen_frame().origin.y && top_left.y + v->_h <= NSMaxY(screen_frame())) top_left.y += v->_h;
    }
    show_view(v, top_left);
    if (positioned) [v setHighlight:(NSInteger)[v->_items indexOfObjectIdenticalTo:positioned]];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"NSMenuDidBeginTrackingNotification" object:menu];
    NSTimeInterval opened = [NSDate timeIntervalSinceReferenceDate];
    BOOL down_inside = NO, dragged = NO;
    NSMenuItem *chosen = nil;
    while (!g_cancel && !chosen && g_open.count) {
        @autoreleasepool {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]
                                               inMode:@"NSEventTrackingRunLoopMode" dequeue:YES];
            if (!e) continue;
            NSEventType t = e.type;
            switch (t) {
            case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged: case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown:
            case NSEventTypeOtherMouseDown: case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp:
            case NSEventTypeOtherMouseUp: {
                NSPoint p = e.window ? [e.window convertPointToScreen:e.locationInWindow] : [NSEvent mouseLocation];
                NSInteger row, level = hit(p, &row);
                if (t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown || t == NSEventTypeOtherMouseDown) {
                    if (level < 0) { g_cancel = YES; break; } /* a press outside: dismissed */
                    down_inside = YES;
                    hover(level, row);
                } else if (t == NSEventTypeLeftMouseUp || t == NSEventTypeRightMouseUp || t == NSEventTypeOtherMouseUp) {
                    /* A release chooses after a press in the menu, or at the end of a drag from where it opened (not
                       the release of the click that opened it). */
                    BOOL late = [NSDate timeIntervalSinceReferenceDate] - opened > 0.3;
                    if (level < 0 || !(down_inside || (dragged && late))) break;
                    hover(level, row);
                    HKMenuView *lv = g_open[(NSUInteger)level];
                    if (lv->_hi >= 0 && lv->_hi == row && !lv->_items[(NSUInteger)row].submenu) chosen = lv->_items[(NSUInteger)row];
                } else {
                    if (t != NSEventTypeMouseMoved) dragged = YES;
                    if (level >= 0 || !g_open.lastObject->_items.count) hover(level, row);
                    else [g_open.lastObject setHighlight:-1];
                }
                break;
            }
            case NSEventTypeKeyDown: {
                HKMenuView *last = g_open.lastObject;
                switch (e.keyCode) {
                case 53: g_cancel = YES; break; /* Escape */
                case 125: step(1); break;
                case 126: step(-1); break;
                case 124: /* right: into a submenu */
                    if (last->_hi >= 0 && last->_items[(NSUInteger)last->_hi].submenu) {
                        open_submenu(g_open.count - 1);
                        step(1);
                    }
                    break;
                case 123: if (g_open.count > 1) close_from(g_open.count - 1); break; /* left: out of one */
                case 36: case 76: case 49: /* Return, Enter, Space */
                    if (last->_hi < 0) break;
                    if (last->_items[(NSUInteger)last->_hi].submenu) {
                        open_submenu(g_open.count - 1);
                        step(1);
                    } else chosen = last->_items[(NSUInteger)last->_hi];
                    break;
                default: break;
                }
                break;
            }
            case NSEventTypeKeyUp: case NSEventTypeFlagsChanged: case NSEventTypeScrollWheel: break;
            default: [NSApp sendEvent:e]; break; /* the app's own events (application-defined, periodic) */
            }
        }
    }
    close_from(0);
    [[NSNotificationCenter defaultCenter] postNotificationName:@"NSMenuDidEndTrackingNotification" object:menu];
    hk_log("NSMenu: \"%s\" (%ld items): %s%s", menu.title.UTF8String, (long)menu.numberOfItems, chosen ? "chose " : "nothing chosen",
           chosen ? title_of(chosen).UTF8String : "");
    return chosen;
}

static void send_item(NSMenuItem *item) {
    NSMenu *m = item.menu;
    NSInteger i = [m indexOfItem:item];
    if (m && i >= 0) [m performActionForItemAtIndex:i];
}

/* A point in a view, or on the screen without one. */
static NSPoint on_screen(NSPoint p, NSView *view) {
    if (!view.window) return p;
    return [view.window convertPointToScreen:[view convertPoint:p toView:nil]];
}

void hk_menu_context(NSMenu *menu, NSEvent *event, NSView *view) {
    NSPoint p;
    NSWindow *w = event.window ?: view.window;
    if (event && w) p = [w convertPointToScreen:event.locationInWindow];
    else p = [NSEvent mouseLocation];
    NSMenuItem *chosen = track(menu, p, nil, NO, 0, YES);
    if (chosen) send_item(chosen);
}

BOOL hk_menu_positioned(NSMenu *menu, NSMenuItem *item, NSPoint location, NSView *view) {
    NSPoint p = on_screen(location, view);
    /* the point is the item's top left; a view that is not flipped gives its bottom left */
    if (view && !view.isFlipped && item) p.y += kRowH;
    NSMenuItem *chosen = track(menu, p, item, NO, 0, item == nil);
    if (chosen) send_item(chosen);
    return chosen != nil;
}

void hk_menu_cell_click(id cell_, NSRect frame, NSView *view) {
    NSPopUpButtonCell *cell = cell_;
    NSMenu *menu = cell.menu;
    if (!menu.numberOfItems) return;
    NSPoint top_left = NSMakePoint(NSMinX(frame), view.isFlipped ? NSMinY(frame) : NSMaxY(frame));
    NSMenuItem *chosen;
    if (cell.pullsDown) { /* below the button, without its title item */
        NSPoint bottom_left = NSMakePoint(NSMinX(frame), view.isFlipped ? NSMaxY(frame) : NSMinY(frame));
        chosen = track(menu, on_screen(bottom_left, view), nil, YES, frame.size.width, NO);
    } else { /* the chosen item over the button */
        NSMenuItem *sel = cell.selectedItem;
        NSPoint p = on_screen(top_left, view);
        p.y += (kRowH - frame.size.height) / 2; /* the item's row centred on the button */
        chosen = track(menu, p, sel, NO, frame.size.width, NO);
    }
    if (!chosen) return;
    [cell selectItem:chosen];
    if (chosen.action) send_item(chosen);
    else if (cell.action) [NSApp sendAction:cell.action to:cell.target from:view ?: (id)cell];
}

void hk_menu_cancel(NSMenu *menu) {
    for (HKMenuView *v in g_open)
        if (v->_menu == menu) g_cancel = YES;
}

NSMenuItem *hk_menu_highlighted(NSMenu *menu) {
    for (HKMenuView *v in g_open)
        if (v->_menu == menu && v->_hi >= 0) return v->_items[(NSUInteger)v->_hi];
    return nil;
}
