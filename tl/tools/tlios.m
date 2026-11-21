/* tlios: runs a Mac app through Henge's loader on this Mac the way it would run on iOS: Henge's AppKit
   (tl/shim/appkit) instead of the real one (which never loads here), every library and symbol iOS lacks refused
   (a deny list from tl_ios_gap.py --deny), no other processes, missing symbols soft. What it shows is drawn into
   PNG files, and input comes from a FIFO, since this process has no window server connection of its own.

   tlios [-v] -deny FILE [-snap DIR] [-every SECONDS] [-input FIFO] [-screen WxH] [-scale S] EXECUTABLE [ARGS...]

   Input lines: "click X Y" (window points, origin top left), "down X Y", "up X Y", "move X Y", "drag X Y",
   "scroll DX DY", "key CODE CHARS", "type TEXT", "snap". */
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/stat.h>

#include "../loader/tl_loader.h"
#include "../shim/appkit/HKShim.h"

static NSMutableDictionary<NSString *, NSString *> *g_deny; /* "lib\tsym" -> alias lib or "" */
static NSMutableSet<NSString *> *g_deny_libs, *g_deny_syms;
static int g_verbose;

/* Libraries whose Mac versions load the real AppKit: refused too (they would bring AppKit's classes in). */
static const char *const g_appkit_loaders[] = {"/AuthenticationServices.framework/", "/ScreenCaptureKit.framework/",
                                               "/MediaPlayer.framework/", "/SafariServices.framework/",
                                               "/LocalAuthenticationEmbeddedUI.framework/", "/AppKit.framework/",
                                               "/Cocoa.framework/", "/WebKit.framework/", "/AVKit.framework/",
                                               "/Quartz.framework/", "/QuickLookUI.framework/", "/Accessibility.framework/",
                                               "/MetalKit.framework/", "/StoreKit.framework/", NULL};

static NSString *leaf_of(const char *install_name) {
    const char *slash = strrchr(install_name, '/');
    return @(slash ? slash + 1 : install_name);
}

static BOOL lib_denied(const char *install_name) {
    if (!install_name || !*install_name) return NO;
    for (int i = 0; g_appkit_loaders[i]; i++)
        if (strstr(install_name, g_appkit_loaders[i])) return YES;
    char buf[1024];
    if (!tl_ios_map_install_name(install_name, buf, sizeof buf)) return YES;
    if ([g_deny_libs containsObject:@(install_name)] || [g_deny_libs containsObject:leaf_of(install_name)]) return YES;
    return NO;
}

static NSString *deny_key(const char *install_name, const char *symbol) {
    return [NSString stringWithFormat:@"%s\t%s", install_name, symbol];
}

static void load_deny(const char *path) {
    g_deny = [NSMutableDictionary new];
    g_deny_libs = [NSMutableSet new];
    g_deny_syms = [NSMutableSet new];
    NSString *text = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSArray *f = [line componentsSeparatedByString:@"\t"];
        if (f.count < 2) continue;
        NSString *lib = f[0], *sym = f[1];
        if ([sym isEqualToString:@"*"]) {
            [g_deny_libs addObject:lib];
            if (![lib containsString:@"/"]) [g_deny_libs addObject:lib];
            continue;
        }
        NSString *alias = f.count > 2 && [f[2] hasPrefix:@"="] ? [f[2] substringFromIndex:1] : @"";
        g_deny[[NSString stringWithFormat:@"%@\t%@", lib, sym]] = alias;
        if (!alias.length) [g_deny_syms addObject:sym];
    }
    fprintf(stderr, "tlios: refusing %lu symbols and %lu libraries iOS lacks\n", (unsigned long)g_deny.count, (unsigned long)g_deny_libs.count);
}

static const char *map_name(const char *name, char *buf, size_t len) {
    return lib_denied(name) ? NULL : name;
}

static void *shim_lookup(const char *install_name, const char *symbol) {
    @autoreleasepool {
        /* Finding classes by name goes through Henge's AppKit wherever it is imported from (as on iOS). */
        if (!strcmp(symbol, "_objc_getClass") || !strcmp(symbol, "_objc_lookUpClass") || !strcmp(symbol, "_objc_getRequiredClass") ||
            !strcmp(symbol, "_objc_getMetaClass") || !strcmp(symbol, "_NSClassFromString")) {
            const char *skip = getenv("TLIOS_NO_BYNAME"); /* a test switch: names not to route */
            if (skip && strstr(skip, symbol)) return NULL;
            return hk_lookup(install_name, symbol);
        }
        if (install_name && *install_name) {
            NSString *alias = g_deny[deny_key(install_name, symbol)];
            if (alias.length) {
                void *h = dlopen(alias.UTF8String, RTLD_LAZY);
                void *s = h ? dlsym(h, symbol + 1) : NULL;
                if (s) return s;
            }
            if (lib_denied(install_name) || alias) return hk_lookup(install_name, symbol);
            return NULL;
        }
        /* No library named (dlsym with RTLD_DEFAULT): Henge's AppKit first for anything it defines. */
        return hk_lookup("", symbol);
    }
}

static int host_lacks(const char *install_name, const char *symbol) {
    @autoreleasepool {
        if (!install_name || !*install_name) return [g_deny_syms containsObject:@(symbol)];
        if (lib_denied(install_name)) return 1;
        return g_deny[deny_key(install_name, symbol)] != nil;
    }
}

/* TLIOS_MISSING=1: after each image is linked, what is still missing (stubs and stand-ins), once each. */
static void image_linked(TlImage *im) {
    static int said;
    fprintf(stderr, "tlios: linked %s at %p\n", tl_image_path(im), tl_image_base(im));
    if (!getenv("TLIOS_MISSING")) return;
    for (; said < tl_missing_count(); said++) fprintf(stderr, "tlios: missing %s\n", tl_missing_name(said));
}

/* TLIOS_MSGLOG=1: the Objective-C runtime logs every message (to /tmp/msgSends-PID, its own fixed place). */
extern void instrumentObjcMessageSends(int flag);

/* A process started inside the app runs as its own .app (a helper's bundle). */
static void process_started(int ns, const char *path) {
    const char *macos = strstr(path, ".app/Contents/MacOS/");
    if (!macos) return;
    char app[4096];
    snprintf(app, sizeof app, "%.*s", (int)(macos - path + 4), path);
    tl_adopt_ns_bundle(ns, app);
}

/* ---- the presenter: windows to PNG files ---- */

static NSString *g_snap_dir;
static NSMutableArray<NSWindow *> *g_shown;
static int g_snap_count;
static id<MTLDevice> g_device;

static void window_changed(NSWindow *window, int visible) {
    if (!g_shown) g_shown = [NSMutableArray new];
    [g_shown removeObjectIdenticalTo:window];
    if (visible) [g_shown addObject:window];
    if (g_verbose) fprintf(stderr, "tlios: window %ld %s %s\n", (long)window.windowNumber, visible ? "shown" : "hidden", NSStringFromRect(window.frame).UTF8String);
}

static void write_png(CGImageRef img, NSString *path) {
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
    if (!d) return;
    CGImageDestinationAddImage(d, img, NULL);
    CGImageDestinationFinalize(d);
    CFRelease(d);
}

static void dump_layer(CALayer *l, int depth) {
    if (depth > 12) return;
    CGRect f = l.frame;
    fprintf(stderr, "%*s%s {%g,%g,%g,%g}%s%s%s%s op=%.2f%s sub=%lu\n", depth * 2, "", object_getClassName(l), f.origin.x, f.origin.y, f.size.width, f.size.height,
            l.hidden ? " HIDDEN" : "", l.contents ? " contents" : "", l.backgroundColor ? " bg" : "", l.geometryFlipped ? " flipped" : "",
            l.opacity, l.masksToBounds ? " clip" : "", (unsigned long)l.sublayers.count);
    for (CALayer *c in l.sublayers) dump_layer(c, depth + 1);
}

/* Layers outside any rendering context are never asked to draw: ask them (on iOS, UIKit's context does it). */
static void display_tree(CALayer *l) {
    [l displayIfNeeded];
    for (CALayer *c in l.sublayers) display_tree(c);
}

/* The screen as the presenter would show it: every visible window's layer tree at its place. */
static void snapshot(void) {
    if (!g_snap_dir) return;
    @autoreleasepool {
        if (!g_device) g_device = MTLCreateSystemDefaultDevice();
        CGSize screen = hk_presenter()->screen_size;
        CGFloat scale = hk_presenter()->scale;
        size_t W = (size_t)(screen.width * scale), H = (size_t)(screen.height * scale);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [g_device newTextureWithDescriptor:td];
        CALayer *stage = [CALayer layer];
        stage.anchorPoint = CGPointZero;
        stage.frame = CGRectMake(0, 0, W, H);
        stage.backgroundColor = CGColorCreateGenericRGB(0.18, 0.2, 0.24, 1);
        CALayer *scaled = [CALayer layer];
        scaled.anchorPoint = CGPointZero;
        scaled.frame = CGRectMake(0, 0, screen.width, screen.height);
        scaled.sublayerTransform = CATransform3DIdentity;
        scaled.transform = CATransform3DMakeScale(scale, scale, 1);
        [stage addSublayer:scaled];
        NSMutableArray *moved = [NSMutableArray array];
        for (NSWindow *w in hk_visible_windows()) {
            CALayer *root = w.hk_rootLayer;
            CALayer *holder = [CALayer layer];
            holder.anchorPoint = CGPointZero;
            holder.frame = w.frame; /* screen coordinates, origin bottom left: CALayer on the Mac is the same */
            CALayer *old = root.superlayer;
            [root removeFromSuperlayer];
            [holder addSublayer:root];
            [scaled addSublayer:holder];
            [moved addObject:@[ root, old ?: (id)[NSNull null] ]];
        }
        id<MTLCommandQueue> q = [g_device newCommandQueue];
        display_tree(stage);
        if (getenv("TLIOS_DUMP")) dump_layer(stage, 0);
        CARenderer *r = [CARenderer rendererWithMTLTexture:tex options:@{kCARendererMetalCommandQueue : q}];
        r.layer = stage;
        r.bounds = stage.frame;
        [CATransaction flush]; /* the renderer sees only what is committed after it has the layer */
        [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [r addUpdateRect:r.bounds];
        [r render];
        [r endFrame];
        id<MTLCommandBuffer> cb = [q commandBuffer]; /* the renderer's work is on q: this one finishes after it */
        [cb commit];
        [cb waitUntilCompleted];
        void *px = malloc(W * H * 4);
        [tex getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(px, W, H, 8, W * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGImageRef upside = CGBitmapContextCreateImage(ctx); /* the texture's rows run top down: turn it over */
        CGContextClearRect(ctx, CGRectMake(0, 0, W, H));
        CGContextTranslateCTM(ctx, 0, H);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), upside);
        CGImageRelease(upside);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        NSString *path = [g_snap_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"screen-%03d.png", g_snap_count++]];
        write_png(img, path);
        write_png(img, [g_snap_dir stringByAppendingPathComponent:@"screen-latest.png"]);
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        free(px);
        for (NSArray *m in moved) {
            CALayer *root = m[0];
            [root removeFromSuperlayer];
            if (m[1] != [NSNull null]) [(CALayer *)m[1] addSublayer:root];
        }
        fprintf(stderr, "tlios: snapshot %s (%lu windows)\n", path.UTF8String, (unsigned long)moved.count);
    }
}

static void snap_timer(CFRunLoopTimerRef t, void *info) { snapshot(); }

/* ---- input from a FIFO ---- */

static NSWindow *front_window(void) {
    NSArray *ws = hk_visible_windows();
    NSWindow *key = NSApp.keyWindow;
    return key && key.isVisible ? key : ws.lastObject;
}

/* Window points with the origin at the top left (as one reads a snapshot) to AppKit's, bottom left. */
static CGPoint to_window(NSWindow *w, double x, double y) { return CGPointMake(x, w.frame.size.height - y); }

static void handle_line(NSString *line) {
    NSArray *f = [line componentsSeparatedByString:@" "];
    if (!f.count) return;
    NSString *cmd = f[0];
    if ([cmd isEqualToString:@"snap"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ snapshot(); });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }
    NSWindow *w = front_window();
    if (!w) { fprintf(stderr, "tlios: no window for '%s'\n", line.UTF8String); return; }
    double a = f.count > 1 ? [f[1] doubleValue] : 0, b = f.count > 2 ? [f[2] doubleValue] : 0;
    if ([cmd isEqualToString:@"click"]) {
        hk_post_mouse(w, NSEventTypeMouseMoved, to_window(w, a, b), 0, 0);
        hk_post_mouse(w, NSEventTypeLeftMouseDown, to_window(w, a, b), 0, 1);
        hk_post_mouse(w, NSEventTypeLeftMouseUp, to_window(w, a, b), 0, 1);
    } else if ([cmd isEqualToString:@"down"]) hk_post_mouse(w, NSEventTypeLeftMouseDown, to_window(w, a, b), 0, 1);
    else if ([cmd isEqualToString:@"up"]) hk_post_mouse(w, NSEventTypeLeftMouseUp, to_window(w, a, b), 0, 1);
    else if ([cmd isEqualToString:@"drag"]) hk_post_mouse(w, NSEventTypeLeftMouseDragged, to_window(w, a, b), 0, 1);
    else if ([cmd isEqualToString:@"move"]) hk_post_mouse(w, NSEventTypeMouseMoved, to_window(w, a, b), 0, 0);
    else if ([cmd isEqualToString:@"scroll"]) hk_post_scroll(w, to_window(w, w.frame.size.width / 2, w.frame.size.height / 2), a, b, NSEventPhaseChanged);
    else if ([cmd isEqualToString:@"swipe"]) { /* swipe DX DY N: a two-finger gesture, began, N changes, ended */
        CGPoint at = to_window(w, w.frame.size.width / 2, w.frame.size.height / 2);
        int n = f.count > 3 ? [f[3] intValue] : 5;
        hk_post_scroll(w, at, 0, 0, NSEventPhaseBegan);
        for (int k = 0; k < n; k++) { hk_post_scroll(w, at, a, b, NSEventPhaseChanged); usleep(16000); }
        hk_post_scroll(w, at, 0, 0, NSEventPhaseEnded);
    }
    else if ([cmd isEqualToString:@"key"]) {
        NSString *chars = f.count > 2 ? f[2] : @"";
        if ([chars isEqualToString:@"RET"]) chars = @"\r";
        hk_post_key(w, NSEventTypeKeyDown, (unsigned short)a, chars, chars, 0);
        hk_post_key(w, NSEventTypeKeyUp, (unsigned short)a, chars, chars, 0);
    } else if ([cmd isEqualToString:@"type"]) {
        NSString *text = [line substringFromIndex:5];
        for (NSUInteger i = 0; i < text.length; i++) {
            NSString *c = [text substringWithRange:NSMakeRange(i, 1)];
            hk_post_key(w, NSEventTypeKeyDown, 0, c, c, 0);
            hk_post_key(w, NSEventTypeKeyUp, 0, c, c, 0);
        }
    } else fprintf(stderr, "tlios: unknown input '%s'\n", line.UTF8String);
}

static void *input_thread(void *arg) {
    const char *path = arg;
    for (;;) {
        FILE *f = fopen(path, "r");
        if (!f) { sleep(1); continue; }
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            @autoreleasepool { handle_line(@(line)); }
        }
        fclose(f);
    }
    return NULL;
}

int main(int argc, const char **argv, const char **envp) {
    @autoreleasepool {
        TlHost host;
        /* Room for an app and the processes it starts; TLIOS_REGION_MB=N tries the phone's size (1520 at its largest
           code cache). */
        size_t region = getenv("TLIOS_REGION_MB") ? (size_t)atoi(getenv("TLIOS_REGION_MB")) << 20 : (size_t)3 << 30;
        if (tl_host_init_split(&host, region)) {
            fprintf(stderr, "tlios: no executable region\n");
            return 120;
        }
        host.map_install_name = map_name;
        host.shim_lookup = shim_lookup;
        host.host_lacks = host_lacks;
        host.image_linked = image_linked;
        host.soft_missing = 1;
        host.no_processes = 1;
        host.ios_kernel_rules = 1;
        host.processes_as_threads = !getenv("TLIOS_NO_PROCS"); /* a test switch: refuse processes as before */
        host.process_started = process_started;
        static HKPresenter presenter = {{1280, 800}, 2, window_changed};
        double every = 0;
        const char *input = NULL;
        int i = 1;
        for (; i < argc && argv[i][0] == '-'; i++) {
            if (!strcmp(argv[i], "-v")) host.verbose = g_verbose = 1;
            else if (!strcmp(argv[i], "-deny") && i + 1 < argc) load_deny(argv[++i]);
            else if (!strcmp(argv[i], "-snap") && i + 1 < argc) g_snap_dir = @(argv[++i]);
            else if (!strcmp(argv[i], "-every") && i + 1 < argc) every = atof(argv[++i]);
            else if (!strcmp(argv[i], "-input") && i + 1 < argc) input = argv[++i];
            else if (!strcmp(argv[i], "-screen") && i + 1 < argc) sscanf(argv[++i], "%lfx%lf", &presenter.screen_size.width, &presenter.screen_size.height);
            else if (!strcmp(argv[i], "-scale") && i + 1 < argc) presenter.scale = atof(argv[++i]);
            else break;
        }
        if (i >= argc || !g_deny) {
            fprintf(stderr, "usage: tlios [-v] -deny FILE [-snap DIR] [-every S] [-input FIFO] [-screen WxH] [-scale S] EXECUTABLE [ARGS...]\n");
            return 121;
        }
        hk_set_presenter(&presenter);
        /* No early hk_font_adopt(nil) here, unlike the iOS app: on a Mac CoreText fonts are plain CF objects, and
           macOS's UIFoundation takes them for AppKit's fonts once they answer AppKit's font methods. */
        /* NSParagraphStyle, NSShadow and friends: UIFoundation, which iOS has as well. */
        dlopen("/System/Library/PrivateFrameworks/UIFoundation.framework/UIFoundation", RTLD_LAZY);
        if (g_snap_dir) {
            mkdir(g_snap_dir.UTF8String, 0755);
            if (every > 0) {
                CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + every, every, 0, 0, snap_timer, NULL);
                CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
            }
        }
        if (input) {
            mkfifo(input, 0600);
            pthread_t t;
            pthread_create(&t, NULL, input_thread, (void *)input);
            pthread_detach(t);
        }
        char err[1024] = "";
        TlImage *exe = tl_load(&host, argv[i], err, sizeof err);
        if (!exe) {
            fprintf(stderr, "tlios: %s\n", err);
            return 122;
        }
        const char *path = tl_image_path(exe);
        const char *macos = strstr(path, ".app/Contents/MacOS/");
        if (macos) {
            char app[4096];
            snprintf(app, sizeof app, "%.*s", (int)(macos - path + 4), path);
            tl_main_bundle_for_everyone(); /* see tl_loader.h: the Mac's UIFoundation needs it, iOS's doesn't */
            if (tl_adopt_main_bundle(app)) fprintf(stderr, "tlios: %s is not a bundle\n", app);
        }
        for (uint32_t k = 0; k < _dyld_image_count(); k++)
            if (strstr(_dyld_get_image_name(k), "/AppKit.framework/")) fprintf(stderr, "tlios: THE REAL APPKIT IS LOADED: %s\n", _dyld_get_image_name(k));
        if (getenv("TLIOS_MSGLOG")) instrumentObjcMessageSends(1);
        return tl_run_main(exe, argc - i, argv + i, envp);
    }
}
