/* Stand-ins for the classes and constants a program imports that this host lacks (TlHost.soft_missing): a class
   becomes an empty class whose every unknown message answers nil (said once per class and message), a constant
   named like a string (NSFooKey, NSBarNotification) becomes a string of its own name, any other constant zeroes. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <string.h>

void tl_log_line(const char *fmt, ...); /* tl_loader.c */

static id answer_nil(id self, SEL _cmd, ...) { return nil; }

static pthread_mutex_t g_said_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet *g_said;

static void say_once(Class cls, SEL sel, BOOL meta) {
    pthread_mutex_lock(&g_said_lock);
    if (!g_said) g_said = [NSMutableSet new];
    NSString *key = [NSString stringWithFormat:@"%c[%s %s]", meta ? '+' : '-', class_getName(cls), sel_getName(sel)];
    BOOL fresh = ![g_said containsObject:key];
    if (fresh) [g_said addObject:key];
    pthread_mutex_unlock(&g_said_lock);
    if (fresh) tl_log_line("missing class answers nil: %s", key.UTF8String);
}

static BOOL resolve_instance(id self, SEL _cmd, SEL sel) {
    say_once((Class)self, sel, NO);
    class_addMethod((Class)self, sel, (IMP)answer_nil, "@@:");
    return YES;
}

static BOOL resolve_class(id self, SEL _cmd, SEL sel) {
    say_once((Class)self, sel, YES);
    class_addMethod(object_getClass(self), sel, (IMP)answer_nil, "@@:");
    return YES;
}

static Class stand_in_class(const char *name) {
    char full[512];
    snprintf(full, sizeof full, "TLMissing_%s", name);
    Class c = objc_getClass(full);
    if (c) return c;
    c = objc_allocateClassPair([NSObject class], full, 0);
    if (!c) return objc_getClass(full);
    class_addMethod(object_getClass(c), @selector(resolveInstanceMethod:), (IMP)resolve_instance, "c@::");
    class_addMethod(object_getClass(c), @selector(resolveClassMethod:), (IMP)resolve_class, "c@::");
    objc_registerClassPair(c);
    return c;
}

/* A stand-in for a class looked up by name (Rust's objc2 panics on a missing class, where a missing framework on
   iOS should only make a feature quiet): the system's prefixes only. */
Class tl_missing_class_by_name(const char *name) {
    static const char *const prefixes[] = {"SC", "NS", "AV", "CA", "MTK", "AS", "UN", "SK", "GC", "CL", "MK", "PK", "WK",
                                           "QL", "IK", "PDF", "SF", "LA", "CN", "EK", "MP", "SM", NULL};
    if (!name) return Nil;
    for (int i = 0; prefixes[i]; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i])) && name[strlen(prefixes[i])] >= 'A' && name[strlen(prefixes[i])] <= 'Z') {
            @autoreleasepool {
                return stand_in_class(name);
            }
        }
    return Nil;
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

static int looks_like_string_constant(const char *name) {
    static const char *const tails[] = {"Key", "Name", "Notification", "Attribute", "AttributeName", "Identifier",
                                        "Domain", "Mode", "Type", "Option", "Exception", "Value", "Format",
                                        "Mode", "Role", "Action", "Subrole", "Orientation", "Appearance"};
    if (strncmp(name, "NS", 2) && strncmp(name, "kAX", 3) && strncmp(name, "kCG", 3) && strncmp(name, "kCF", 3) &&
        strncmp(name, "kSec", 4) && strncmp(name, "kUT", 3) && strncmp(name, "kLS", 3) && strncmp(name, "kIO", 3) &&
        strncmp(name, "kCV", 3) && strncmp(name, "kTIS", 4))
        return 0;
    for (size_t i = 0; i < sizeof tails / sizeof *tails; i++)
        if (ends_with(name, tails[i])) return 1;
    if (!strncmp(name, "NSPasteboardType", 16) || strstr(name, "PboardType")) return 1; /* NSPasteboardTypePDF, ... */
    return 0;
}

uint64_t tl_missing_data(const char *sym) {
    if (sym[0] != '_') return 0;
    const char *name = sym + 1;
    @autoreleasepool {
        if (!strncmp(name, "OBJC_CLASS_$_", 13)) return (uint64_t)stand_in_class(name + 13);
        if (!strncmp(name, "OBJC_METACLASS_$_", 17)) return (uint64_t)object_getClass(stand_in_class(name + 17));
        if (!strncmp(name, "OBJC_EHTYPE_$_", 14) || !strncmp(name, "OBJC_IVAR_$_", 12)) return (uint64_t)calloc(1, 64);
        if (looks_like_string_constant(name)) {
            void **slot = calloc(1, 64);
            *slot = (void *)CFBridgingRetain([[NSString alloc] initWithUTF8String:name]);
            return (uint64_t)slot;
        }
        if (name[0] == 'k' && name[1] >= 'A' && name[1] <= 'Z') return (uint64_t)calloc(1, 256);
    }
    return 0;
}

/* For an import known to be data (the program never calls it): a class or ivar as above; anything else a string of
   its own name, which is what most constants of Apple's libraries are (keys, notification names, error domains). */
uint64_t tl_missing_data_any(const char *sym) {
    uint64_t known = tl_missing_data(sym);
    if (known) return known;
    if (sym[0] != '_') return 0;
    @autoreleasepool {
        void **slot = calloc(1, 64);
        *slot = (void *)CFBridgingRetain([[NSString alloc] initWithUTF8String:sym + 1]);
        return (uint64_t)slot;
    }
}

/* A launchd job file's program and arguments (for launchctl load, answered inside the app). argv gets up to max-1
   strings (strdup'd), NULL-terminated; returns their count, or -1. */
int tl_plist_job(const char *path, char **argv, int max) {
    @autoreleasepool {
        NSDictionary *job = [NSDictionary dictionaryWithContentsOfFile:@(path)];
        if (![job isKindOfClass:[NSDictionary class]]) return -1;
        NSArray *args = job[@"ProgramArguments"];
        NSString *program = job[@"Program"] ?: ([args isKindOfClass:[NSArray class]] ? args.firstObject : nil);
        if (![program isKindOfClass:[NSString class]]) return -1;
        int n = 0;
        argv[n++] = strdup(program.fileSystemRepresentation);
        if ([args isKindOfClass:[NSArray class]])
            for (NSUInteger i = 1; i < args.count && n < max - 1; i++)
                if ([args[i] isKindOfClass:[NSString class]]) argv[n++] = strdup([args[i] UTF8String]);
        argv[n] = NULL;
        return n;
    }
}
