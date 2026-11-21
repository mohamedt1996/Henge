/* The base of Henge's AppKit: a log, and the fallback that makes an unimplemented message say so once and answer
   0 instead of raising (the program goes on; the log is the to-do list). */
#import "HKShim.h"
#import <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>

void hk_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "hk: %s\n", line);
}

static pthread_mutex_t g_said_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableSetRef g_said;

static void say_once(id self, SEL sel, BOOL meta) {
    char key[512];
    snprintf(key, sizeof key, "%c[%s %s]", meta ? '+' : '-', meta ? class_getName((Class)self) : object_getClassName(self),
             sel_getName(sel));
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    pthread_mutex_lock(&g_said_lock);
    if (!g_said) g_said = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
    BOOL fresh = !CFSetContainsValue(g_said, k);
    if (fresh) CFSetAddValue(g_said, k);
    pthread_mutex_unlock(&g_said_lock);
    CFRelease(k);
    if (fresh) hk_log("not implemented, answers 0: %s", key);
}

/* An unknown selector's real signature is unknown: answer as "returns an object, takes nothing", so the return
   register reads 0 (nil, NO, 0) whatever the caller expected; a struct return is not cleared. */
static NSMethodSignature *fallback_signature(id self, SEL _cmd, SEL sel) {
    Class cls = object_getClass(self);
    Method m = class_getInstanceMethod(cls, sel);
    if (m) return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(m)];
    return [NSMethodSignature signatureWithObjCTypes:"@@:"];
}

static void fallback_forward(id self, SEL _cmd, NSInvocation *inv) {
    say_once(self, inv.selector, class_isMetaClass(object_getClass(self)));
    NSUInteger len = inv.methodSignature.methodReturnLength;
    if (len) {
        void *zero = calloc(1, len < 64 ? 64 : len);
        [inv setReturnValue:zero];
        free(zero);
    }
}

void hk_add_fallback(Class cls) {
    Class meta = object_getClass(cls);
    class_addMethod(cls, @selector(methodSignatureForSelector:), (IMP)fallback_signature, "@@::");
    class_addMethod(cls, @selector(forwardInvocation:), (IMP)fallback_forward, "v@:@");
    class_addMethod(meta, @selector(methodSignatureForSelector:), (IMP)fallback_signature, "@@::");
    class_addMethod(meta, @selector(forwardInvocation:), (IMP)fallback_forward, "v@:@");
}

@implementation HKShimObject
+ (void)load { hk_add_fallback(self); }
@end
