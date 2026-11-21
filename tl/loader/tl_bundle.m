/* The guest app's bundle as the process's main bundle. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "tl_loader.h"

static NSBundle *g_bundle;
static NSBundle *g_ns_bundles[64]; /* processes started inside the app: their own bundles */
static IMP g_host_main_bundle;
static int g_for_everyone;

void tl_main_bundle_for_everyone(void) { g_for_everyone = 1; }

/* The app's own code asking (objc_msgSend tail-calls this, so the return address is the caller's) gets the app's
   bundle; anything else, the host included (Henge reads its own resources and build all launch), the host's. */
static NSBundle *main_bundle(id self, SEL _cmd) {
    void *caller = __builtin_return_address(0);
    int ns = tl_ns_of_address(caller);
    if (ns > 0 && ns < 64 && g_ns_bundles[ns]) return g_ns_bundles[ns];
    if (g_bundle && (g_for_everyone || tl_image_containing(caller))) return g_bundle;
    return ((NSBundle * (*)(id, SEL))g_host_main_bundle)(self, _cmd);
}

/* Processes started inside the app (processes as threads) each have their own app bundle (a helper's .app). */
void tl_adopt_ns_bundle(int ns, const char *app_path) {
    @autoreleasepool {
        if (ns <= 0 || ns >= 64) return;
        g_ns_bundles[ns] = [NSBundle bundleWithPath:@(app_path)];
        if (!g_host_main_bundle) { /* the swizzle may not be in yet */
            Method m = class_getClassMethod([NSBundle class], @selector(mainBundle));
            IMP old = method_setImplementation(m, (IMP)main_bundle);
            if (old != (IMP)main_bundle) g_host_main_bundle = old;
        }
    }
}

void *tl_main_bundle(void) { return (__bridge void *)g_bundle; }

/* CFBundleGetMainBundle() for the app's code (tl_loader.c answers its imports of it here): the app's bundle as a
   CFBundle, or a started process's own; NULL before one is adopted. Qt finds its plugins (Contents/PlugIns) and
   qt.conf this way. */
static CFBundleRef g_cf_main, g_cf_ns[64];
void *tl_cf_main_bundle(int ns) {
    @autoreleasepool {
        int own = ns > 0 && ns < 64 && g_ns_bundles[ns];
        NSBundle *b = own ? g_ns_bundles[ns] : g_bundle;
        if (!b) return NULL;
        CFBundleRef *slot = own ? &g_cf_ns[ns] : &g_cf_main;
        if (!*slot) *slot = CFBundleCreate(NULL, (__bridge CFURLRef)b.bundleURL);
        return *slot;
    }
}

int tl_adopt_main_bundle(const char *app_path) {
    @autoreleasepool {
        NSBundle *b = [NSBundle bundleWithPath:@(app_path)];
        if (!b || !b.bundleIdentifier) return -1;
        g_bundle = b;
        Method m = class_getClassMethod([NSBundle class], @selector(mainBundle));
        IMP old = method_setImplementation(m, (IMP)main_bundle);
        if (old != (IMP)main_bundle) g_host_main_bundle = old;
        /* Not yet: CFBundleGetMainBundle() still answers the host's bundle (LiveContainer rewrites the CFBundle). */
        return 0;
    }
}
