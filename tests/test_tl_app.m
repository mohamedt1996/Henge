/* The app's glue for Mac apps with windows (ios/Henge/Native/henge-tl-app.m) on the Mac: the same loading, switches,
   bindings and window hand-off the phone uses, with iOS's library names (so Mac-only libraries are refused as on
   iOS; symbols Mac libraries have beyond iOS's are not, which tl/tools/tlios checks instead). Windows are drawn to
   PNG files every few seconds.

   test_tl_app APP SNAPDIR [SECONDS] */
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>

#include "henge-ios-jit.h"
#include "henge-tl.h"
#include "jit_region.h"

static HvJitRegion jit;
static HengeDualMapping mapping;
_Atomic int henge_tl_ran;
int henge_vm_state(void) { return 0; }
HengeDualMapping *henge_ios_jit_get_mapping(void) { return &mapping; }
/* The Mac has room enough; TEST_TL_ROOM=1 holds what the phone holds (with TL_MAX_MAP_GB=8, about as tight as iOS):
   64 GB aligned to 32, blocks at +0 (40 GB), +44 GB (4 GB) and +48 GB (16 GB), 4 GB at +40 left for others. */
int henge_ios_take_high_room(void **base, size_t *bytes) {
    if (!getenv("TEST_TL_ROOM")) return 0;
    size_t all = (size_t)96 << 30;
    void *p = mmap(NULL, all, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    uint8_t *b = (uint8_t *)(((uintptr_t)p + ((size_t)32 << 30) - 1) & ~(((uintptr_t)32 << 30) - 1));
    munmap(b + ((size_t)40 << 30), (size_t)4 << 30); /* the unheld middle */
    base[0] = b, bytes[0] = (size_t)40 << 30;
    base[1] = b + ((size_t)48 << 30), bytes[1] = (size_t)16 << 30;
    base[2] = b + ((size_t)44 << 30), bytes[2] = (size_t)4 << 30;
    return 3;
}

static NSString *g_dir;
static int g_n;

/* TEST_TL_IN=FIFO: input as the phone's screen sends it, in the Mac's screen points with the origin at the top left
   (as one reads a snapshot): "tap X Y" (press and release at once, as a trackpad tap), "click X Y" (moved there
   first, as a touch-mode tap), "right X Y", "move X Y", "down X Y", "drag X Y", "up X Y", "scroll DX DY X Y",
   "key CODE [CHARS|RET]", "type TEXT", "snap". */
static void snap(CFRunLoopTimerRef t, void *info);
static double screen_h = 680;

static void input_line(NSString *line) {
    NSArray<NSString *> *f = [line componentsSeparatedByString:@" "];
    NSString *cmd = f.firstObject;
    double a = f.count > 1 ? f[1].doubleValue : 0, b = f.count > 2 ? f[2].doubleValue : 0;
    double y = screen_h - b;
    if ([cmd isEqualToString:@"snap"]) {
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ snap(NULL, NULL); });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    } else if ([cmd isEqualToString:@"tap"]) {
        henge_tl_app_mouse(1, a, y, 1, 0);
        henge_tl_app_mouse(2, a, y, 1, 0);
    } else if ([cmd isEqualToString:@"click"]) {
        henge_tl_app_mouse(5, a, y, 0, 0);
        henge_tl_app_mouse(1, a, y, 1, 0);
        henge_tl_app_mouse(2, a, y, 1, 0);
    } else if ([cmd isEqualToString:@"right"]) {
        henge_tl_app_mouse(3, a, y, 1, 0);
        henge_tl_app_mouse(4, a, y, 1, 0);
    } else if ([cmd isEqualToString:@"move"]) henge_tl_app_mouse(5, a, y, 0, 0);
    else if ([cmd isEqualToString:@"down"]) henge_tl_app_mouse(1, a, y, 1, 0);
    else if ([cmd isEqualToString:@"drag"]) henge_tl_app_mouse(6, a, y, 1, 0);
    else if ([cmd isEqualToString:@"up"]) henge_tl_app_mouse(2, a, y, 1, 0);
    else if ([cmd isEqualToString:@"scroll"] && f.count > 4) {
        double sx = f[3].doubleValue, sy = screen_h - f[4].doubleValue;
        henge_tl_app_scroll(sx, sy, 0, 0, 1);
        for (int i = 0; i < 5; i++) { henge_tl_app_scroll(sx, sy, a / 5, b / 5, 4); usleep(16000); }
        henge_tl_app_scroll(sx, sy, 0, 0, 8);
    } else if ([cmd isEqualToString:@"key"]) {
        NSString *c = f.count > 2 ? f[2] : @"";
        if ([c isEqualToString:@"RET"]) c = @"\r";
        unsigned long flags = f.count > 3 ? strtoul(f[3].UTF8String, NULL, 0) : 0; /* key CODE CHARS FLAGS: 0x100000 is Command */
        henge_tl_app_key(1, (unsigned short)a, c.UTF8String, flags);
        henge_tl_app_key(0, (unsigned short)a, c.UTF8String, flags);
    } else if ([cmd isEqualToString:@"type"] && line.length > 5) {
        NSString *text = [line substringFromIndex:5];
        for (NSUInteger i = 0; i < text.length; i++) {
            const char *c = [text substringWithRange:NSMakeRange(i, 1)].UTF8String;
            henge_tl_app_key(1, 0, c, 0);
            henge_tl_app_key(0, 0, c, 0);
        }
    } else if ([cmd isEqualToString:@"menus"]) { /* the menu bar's menus, as the screen's menu button gets them */
        const void *raw = henge_tl_app_main_menu();
        NSArray *items = raw ? (__bridge_transfer NSArray *)raw : nil;
        __block void (^show)(NSArray *, int);
        void (^__block __weak weak)(NSArray *, int);
        weak = show = ^(NSArray *list, int depth) {
            for (NSDictionary *d in list) {
                if ([d[@"separator"] boolValue]) { fprintf(stderr, "test_tl_app: menu %*s----\n", depth * 2, ""); continue; }
                fprintf(stderr, "test_tl_app: menu %*s%s%s%s [%s]\n", depth * 2, "", [d[@"title"] UTF8String], [d[@"enabled"] boolValue] ? "" : " (off)",
                        [d[@"state"] intValue] ? " (on)" : "", [[d[@"path"] componentsJoinedByString:@"."] UTF8String]);
                if (d[@"children"] && depth < 1) weak(d[@"children"], depth + 1);
            }
        };
        show(items, 0);
    } else if ([cmd isEqualToString:@"choose"] && f.count > 1) { /* choose 2.5: an item of the menu bar's menus */
        NSArray<NSString *> *parts = [f[1] componentsSeparatedByString:@"."];
        int path[8], n = 0;
        for (NSString *p in parts) if (n < 8) path[n++] = p.intValue;
        henge_tl_app_menu_choose(path, n);
    } else fprintf(stderr, "test_tl_app: unknown input '%s'\n", line.UTF8String);
}

/* The screen's input arrives on the main thread (UIKit's gesture callbacks): so here. */
static void *input_thread(void *arg) {
    const char *path = arg;
    for (;;) {
        FILE *f = fopen(path, "r");
        if (!f) { sleep(1); continue; }
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\n")] = 0;
            NSString *line = @(buf);
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                @autoreleasepool { input_line(line); }
                dispatch_semaphore_signal(done);
            });
            CFRunLoopWakeUp(CFRunLoopGetMain());
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        }
        fclose(f);
    }
    return NULL;
}

static void changed(void) { fprintf(stderr, "test_tl_app: %d windows\n", henge_tl_app_window_count()); }

static void snap(CFRunLoopTimerRef t, void *info) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        int n = henge_tl_app_window_count();
        for (int i = 0; i < n; i++) {
            CALayer *root = (__bridge CALayer *)henge_tl_app_window_layer(i);
            double x, y, w, h;
            henge_tl_app_window_frame(i, &x, &y, &w, &h);
            size_t W = (size_t)w * 2, H = (size_t)h * 2;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            id<MTLCommandQueue> q = [dev newCommandQueue];
            CALayer *stage = [CALayer layer], *old = root.superlayer;
            stage.anchorPoint = CGPointZero;
            stage.frame = CGRectMake(0, 0, W, H);
            CALayer *scaled = [CALayer layer];
            scaled.anchorPoint = CGPointZero;
            scaled.frame = CGRectMake(0, 0, w, h);
            scaled.transform = CATransform3DMakeScale(2, 2, 1);
            [stage addSublayer:scaled];
            [root removeFromSuperlayer];
            [scaled addSublayer:root];
            CARenderer *r = [CARenderer rendererWithMTLTexture:tex options:@{kCARendererMetalCommandQueue : q}];
            r.layer = stage;
            r.bounds = stage.frame;
            [CATransaction flush]; /* the renderer sees only what is committed after it has the layer */
            [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
            [r addUpdateRect:r.bounds];
            [r render];
            [r endFrame];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [cb commit];
            [cb waitUntilCompleted];
            [root removeFromSuperlayer];
            [old addSublayer:root];
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
            NSString *path = [g_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"win%d-%03d.png", i, g_n]];
            CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
            CGImageDestinationAddImage(d, img, NULL);
            CGImageDestinationFinalize(d);
            CFRelease(d);
            CGImageRelease(img);
            CGContextRelease(ctx);
            CGColorSpaceRelease(cs);
            free(px);
            fprintf(stderr, "test_tl_app: %s\n", path.UTF8String);
        }
        g_n++;
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: test_tl_app APP SNAPDIR [SECONDS]\n");
            return 2;
        }
        setenv("HV_JIT_SPLIT", "1", 1);
        if (hv_jit_region_open(&jit, (getenv("TEST_TL_APP_MB") ? atoi(getenv("TEST_TL_APP_MB")) : 1536) << 20)) return 1; /* the phone's largest code cache */
        mapping = (HengeDualMapping){.rx_addr = (uint8_t *)jit.base, .rw_addr = (uint8_t *)jit.base + jit.rw_diff, .size = jit.capacity * 4};
        g_dir = @(argv[2]);
        [[NSFileManager defaultManager] createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
        double every = argc > 3 ? atof(argv[3]) : 5;
        /* NSParagraphStyle, string drawing: UIKit always has UIFoundation loaded on iOS. */
        dlopen("/System/Library/PrivateFrameworks/UIFoundation.framework/UIFoundation", RTLD_LAZY);
        double screen_w = 1180; /* TEST_TL_SCREEN=WxH: another screen (an iPhone 17 Pro's is 1180x543) */
        if (getenv("TEST_TL_SCREEN")) sscanf(getenv("TEST_TL_SCREEN"), "%lfx%lf", &screen_w, &screen_h);
        henge_tl_app_set_screen(screen_w, screen_h, 2);
        henge_tl_app_on_change(changed);
        /* Loaded on another thread, as the app does (MacAppScreen loads off the main thread). */
        static char err[512] = "";
        __block int rc = 0;
        const char *app = argv[1];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            rc = henge_tl_app_load(app, err, sizeof err);
            dispatch_semaphore_signal(done);
        });
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (rc) {
            fprintf(stderr, "test_tl_app: %s\n", err);
            return 1;
        }
        if (getenv("TEST_TL_IN")) {
            pthread_t th;
            pthread_create(&th, NULL, input_thread, strdup(getenv("TEST_TL_IN")));
        }
        CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + every, every, 0, 0, snap, NULL);
        CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
        return henge_tl_app_run();
    }
}
