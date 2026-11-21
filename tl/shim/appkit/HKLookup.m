/* What Henge's AppKit answers for a symbol a program imports from a Mac-only library (or a Mac-only symbol of a
   library iOS has): its classes, C functions and constants, from the generated table of this image's own symbols (iOS
   has classes of its own named NSColor and NSFont, so never by name); then, by name, the classes UIFoundation has on
   both systems (NSParagraphStyle, NSShadow...), never one from a Mac-only library loaded in the Mac test runner. */
#import "HKShim.h"
#include "../../loader/tl_loader.h"
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>

static const char *own_image(void) {
    static const char *path;
    if (!path) {
        Dl_info info;
        if (dladdr((const void *)&hk_lookup, &info)) path = strdup(info.dli_fname);
    }
    return path;
}

static int class_allowed(Class c) {
    const char *img = class_getImageName(c);
    if (!img) return 0;
    const char *own = own_image();
    return (own && !strcmp(img, own)) || strstr(img, "/UIFoundation.framework/") != NULL;
}

static const char *const g_renamed[][2] = {
    {"_OBJC_CLASS_$_NSColor", "_OBJC_CLASS_$_HKColor"}, {"_OBJC_METACLASS_$_NSColor", "_OBJC_METACLASS_$_HKColor"},
    {"_OBJC_CLASS_$_NSFont", "_OBJC_CLASS_$_HKFont"}, {"_OBJC_METACLASS_$_NSFont", "_OBJC_METACLASS_$_HKFont"},
    {"_OBJC_CLASS_$_NSFontDescriptor", "_OBJC_CLASS_$_HKFontDescriptor"},
    {"_OBJC_METACLASS_$_NSFontDescriptor", "_OBJC_METACLASS_$_HKFontDescriptor"},
};

static void *own_symbol(const char *symbol) {
    for (size_t i = 0; i < sizeof g_renamed / sizeof *g_renamed; i++)
        if (!strcmp(symbol, g_renamed[i][0])) symbol = g_renamed[i][1];
    for (int i = 0; i < hk_nsymbols; i++)
        if (!strcmp(hk_symbols[i].name, symbol)) return hk_symbols[i].address;
    return NULL;
}

/* This AppKit's class of that name, if it has one. */
static Class own_class(const char *name) {
    if (!name) return Nil;
    char sym[300];
    snprintf(sym, sizeof sym, "_OBJC_CLASS_$_%s", name);
    return (__bridge Class)own_symbol(sym);
}

/* A program that finds classes by name (Rust's objc2 does for all of AppKit) gets this AppKit's: on iOS the runtime
   has other classes called NSColor and NSFont. */
/* They return void *, not Class: ARC would retain and autorelease a returned Class, and a root class that answers no
   messages (Chromium's CrZombie) takes that retain for a message to a freed object and stops the program. */
Class tl_missing_class_by_name(const char *name); /* tl/loader/tl_stubs.m */
static void *hk_objc_getClass(const char *name) {
    Class own = own_class(name);
    if (own && getenv("HK_LOG_BYNAME")) hk_log("objc_getClass(%s): Henge's", name);
    Class c = own ?: objc_getClass(name);
    if (!c && (c = tl_missing_class_by_name(name))) hk_log("objc_getClass(%s): a stand-in (not on this system)", name);
    return (__bridge void *)c;
}
static void *hk_objc_lookUpClass(const char *name) { return (__bridge void *)(own_class(name) ?: objc_lookUpClass(name)); }
static void *hk_objc_getRequiredClass(const char *name) {
    return (__bridge void *)(own_class(name) ?: objc_getRequiredClass(name));
}
static Class hk_objc_getMetaClass(const char *name) {
    Class c = own_class(name);
    return c ? object_getClass(c) : (Class)objc_getMetaClass(name);
}
static Class hk_NSClassFromString(NSString *name) { return own_class(name.UTF8String) ?: NSClassFromString(name); }

void *hk_lookup(const char *install_name, const char *symbol) {
    if (!symbol || symbol[0] != '_') return NULL;
    static const struct { const char *name; void *fn; } by_name[] = {
        {"_objc_getClass", (void *)hk_objc_getClass}, {"_objc_lookUpClass", (void *)hk_objc_lookUpClass},
        {"_objc_getRequiredClass", (void *)hk_objc_getRequiredClass}, {"_objc_getMetaClass", (void *)hk_objc_getMetaClass},
        {"_NSClassFromString", (void *)hk_NSClassFromString},
    };
    for (size_t i = 0; i < sizeof by_name / sizeof *by_name; i++)
        if (!strcmp(symbol, by_name[i].name)) return tl_binding_links_appkit_classes() ? NULL : by_name[i].fn;
    void *own = own_symbol(symbol); /* this image's own definitions first, classes included */
    if (own) return own;
    /* Foundation's NSTask would start real processes: the app gets Henge's (HKTask.m). */
    if (!strcmp(symbol, "_OBJC_CLASS_$_NSTask")) return (__bridge void *)objc_getClass("HKTask");
    if (!strcmp(symbol, "_OBJC_METACLASS_$_NSTask")) return (__bridge void *)object_getClass(objc_getClass("HKTask"));
    /* MediaPlayer's centers when the process has no MediaPlayer (HKPrivate.m). */
    static const char *const media[][3] = {{"MPRemoteCommandCenter", "HKRemoteCommandCenter"},
                                           {"MPNowPlayingInfoCenter", "HKNowPlayingInfoCenter"}};
    for (size_t i = 0; i < sizeof media / sizeof *media; i++) {
        size_t n = strlen(media[i][0]), len = strlen(symbol);
        if (len < n || strcmp(symbol + len - n, media[i][0]) || objc_getClass(media[i][0])) continue;
        if (!strncmp(symbol, "_OBJC_CLASS_$_", 14)) return (__bridge void *)objc_getClass(media[i][1]);
        if (!strncmp(symbol, "_OBJC_METACLASS_$_", 18)) return (__bridge void *)object_getClass(objc_getClass(media[i][1]));
    }
    if (!strncmp(symbol, "_OBJC_CLASS_$_", 14) || !strncmp(symbol, "_OBJC_METACLASS_$_", 18)) {
        int meta = symbol[6] == 'M';
        Class c = objc_getClass(symbol + (meta ? 18 : 14));
        if (!c || !class_allowed(c)) return NULL;
        return meta ? (__bridge void *)object_getClass(c) : (__bridge void *)c;
    }
    return NULL;
}
