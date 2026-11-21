/* The application's main nib (Info.plist's NSMainNibFile), as far as apps that build their windows in code need it:
   the object the nib makes the application's delegate. AppKit's NSApplicationMain loads that nib before the run loop
   starts; Henge's (HKCore.m) loads none, so when the application is about to finish launching without a delegate,
   this reads the compiled nib (a NIBArchive) for the "delegate" outlet of the file's owner (the application), makes
   that object (a custom class of the app's, by name) and makes it the delegate. setDelegate: subscribes it to the
   launch notifications that follow (applicationDidFinishLaunching:...). Menus and windows in the nib are not made.

   NIBArchive: "NIBArchive", two 32-bit words, then four (count, offset) pairs, 32-bit little endian: objects, keys,
   values, class names. Objects are (class, first value, value count); keys are names; values are (key, type, data);
   class names carry a list of 32-bit extras. Integers there are 7-bit groups, low first, the last with its high bit
   set. Strings are NSString objects with their bytes under "NS.bytes"; arrays list their items under
   "UINibEncoderEmptyKey". */
#import "HKShim.h"
#import <objc/message.h>
#import <objc/runtime.h>

typedef struct {
    uint32_t cls, first, count;
} NibObject;
typedef struct {
    uint32_t key;
    uint8_t type;
    uint64_t number; /* integers, booleans, object references */
    const uint8_t *bytes;
    uint32_t length; /* data */
} NibValue;

typedef struct {
    NSData *file;
    NibObject *objects;
    uint32_t nobjects;
    NSMutableArray<NSString *> *keys, *classes;
    NibValue *values;
    uint32_t nvalues;
} Nib;

static int read_varint(const uint8_t **p, const uint8_t *end, uint32_t *out) {
    uint32_t v = 0;
    for (int shift = 0; *p < end && shift < 35; shift += 7) {
        uint8_t b = *(*p)++;
        v |= (uint32_t)(b & 0x7f) << shift;
        if (b & 0x80) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int nib_parse(Nib *nib, NSData *data) {
    const uint8_t *base = data.bytes, *end = base + data.length;
    if (data.length < 50 || memcmp(base, "NIBArchive", 10)) return 0;
    nib->file = data;
    uint32_t h[8];
    for (int i = 0; i < 8; i++) h[i] = u32(base + 18 + 4 * i);
    const uint8_t *p = base + h[1];
    nib->objects = calloc(h[0] ?: 1, sizeof *nib->objects);
    nib->nobjects = h[0];
    for (uint32_t i = 0; i < h[0]; i++)
        if (!read_varint(&p, end, &nib->objects[i].cls) || !read_varint(&p, end, &nib->objects[i].first) ||
            !read_varint(&p, end, &nib->objects[i].count))
            return 0;
    nib->keys = [NSMutableArray array];
    p = base + h[3];
    for (uint32_t i = 0; i < h[2]; i++) {
        uint32_t n;
        if (!read_varint(&p, end, &n) || p + n > end) return 0;
        [nib->keys addObject:[[NSString alloc] initWithBytes:p length:n encoding:NSUTF8StringEncoding] ?: @""];
        p += n;
    }
    nib->values = calloc(h[4] ?: 1, sizeof *nib->values);
    nib->nvalues = h[4];
    p = base + h[5];
    for (uint32_t i = 0; i < h[4]; i++) {
        NibValue *v = &nib->values[i];
        if (!read_varint(&p, end, &v->key) || p >= end) return 0;
        v->type = *p++;
        static const int sizes[] = {1, 2, 4, 8, 0, 0, 4, 8, -1, 0, 4};
        if (v->type > 10) return 0;
        int size = sizes[v->type];
        if (size < 0) {
            if (!read_varint(&p, end, &v->length) || p + v->length > end) return 0;
            v->bytes = p;
            p += v->length;
            continue;
        }
        if (p + size > end) return 0;
        for (int k = 0; k < size; k++) v->number |= (uint64_t)p[k] << (8 * k);
        if (v->type == 4) v->number = 1;
        v->bytes = p;
        p += size;
    }
    nib->classes = [NSMutableArray array];
    p = base + h[7];
    for (uint32_t i = 0; i < h[6]; i++) {
        uint32_t n, extras;
        if (!read_varint(&p, end, &n) || !read_varint(&p, end, &extras) || p + 4 * extras + n > end) return 0;
        p += 4 * extras;
        NSUInteger len = n && p[n - 1] == 0 ? n - 1 : n;
        [nib->classes addObject:[[NSString alloc] initWithBytes:p length:len encoding:NSUTF8StringEncoding] ?: @""];
        p += n;
    }
    return 1;
}

static void nib_free(Nib *nib) {
    free(nib->objects);
    free(nib->values);
}

static NSString *nib_class(Nib *nib, uint32_t obj) { return obj < nib->nobjects && nib->objects[obj].cls < nib->classes.count ? nib->classes[nib->objects[obj].cls] : nil; }

static NibValue *nib_value(Nib *nib, uint32_t obj, NSString *key) {
    if (obj >= nib->nobjects) return NULL;
    NibObject *o = &nib->objects[obj];
    for (uint32_t i = o->first; i < o->first + o->count && i < nib->nvalues; i++)
        if (nib->values[i].key < nib->keys.count && [nib->keys[nib->values[i].key] isEqualToString:key]) return &nib->values[i];
    return NULL;
}

static int64_t nib_ref(Nib *nib, uint32_t obj, NSString *key) {
    NibValue *v = nib_value(nib, obj, key);
    return v && v->type == 10 ? (int64_t)v->number : -1;
}

static NSString *nib_string(Nib *nib, int64_t obj) {
    if (obj < 0) return nil;
    NibValue *v = nib_value(nib, (uint32_t)obj, @"NS.bytes");
    if (!v || v->type != 8) return nil;
    return [[NSString alloc] initWithBytes:v->bytes length:v->length encoding:NSUTF8StringEncoding];
}

/* The class name a nib's custom object stands for ("NSApplication" for the file's owner of a main nib). */
static NSString *custom_class(Nib *nib, int64_t obj) {
    if (obj < 0 || ![nib_class(nib, (uint32_t)obj) isEqualToString:@"NSCustomObject"]) return nil;
    return nib_string(nib, nib_ref(nib, (uint32_t)obj, @"NSClassName"));
}

static NSMutableArray *g_nib_objects; /* what the nib made, kept: the application's delegate is weak */

static NSData *main_nib_data(void) {
    NSBundle *app = hk_app_bundle();
    NSString *name = app.infoDictionary[@"NSMainNibFile"];
    if (!name.length) return nil;
    NSString *path = [app pathForResource:name ofType:@"nib"];
    BOOL dir = NO;
    if (path && [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] && dir) {
        NSString *inner = [path stringByAppendingPathComponent:@"keyedobjects.nib"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:inner]) {
            for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:path error:nil])
                if ([f hasPrefix:@"keyedobjects"]) inner = [path stringByAppendingPathComponent:f];
        }
        path = inner;
    }
    if (!path) {
        hk_log("main nib \"%s\" not found", name.UTF8String);
        return nil;
    }
    return [NSData dataWithContentsOfFile:path];
}

static void adopt_main_nib_delegate(void) {
    if (!NSApp || NSApp.delegate) return;
    NSData *data = main_nib_data();
    if (!data) return;
    Nib nib = {0};
    if (!nib_parse(&nib, data)) {
        hk_log("main nib: not a NIBArchive this can read");
        nib_free(&nib);
        return;
    }
    for (uint32_t i = 0; i < nib.nobjects; i++) {
        if (![nib_class(&nib, i) isEqualToString:@"NSNibOutletConnector"]) continue;
        if (![nib_string(&nib, nib_ref(&nib, i, @"NSLabel")) isEqualToString:@"delegate"]) continue;
        NSString *owner = custom_class(&nib, nib_ref(&nib, i, @"NSSource"));
        if (!owner || !([owner isEqualToString:@"NSApplication"] || [NSApp isKindOfClass:NSClassFromString(owner) ?: [NSNull class]]))
            continue;
        NSString *name = custom_class(&nib, nib_ref(&nib, i, @"NSDestination"));
        Class cls = name ? NSClassFromString(name) : Nil;
        if (!cls) {
            hk_log("main nib: the application's delegate class %s is not loaded", name.UTF8String ?: "?");
            continue;
        }
        id obj = [[cls alloc] init];
        if (!obj) continue;
        if (!g_nib_objects) g_nib_objects = [NSMutableArray array];
        [g_nib_objects addObject:obj];
        hk_log("main nib: the application's delegate is a new %s", name.UTF8String);
        NSApp.delegate = obj;
        if ([obj respondsToSelector:@selector(awakeFromNib)]) ((void (*)(id, SEL))objc_msgSend)(obj, @selector(awakeFromNib));
        break;
    }
    nib_free(&nib);
}

@interface HKMainNib : NSObject
@end
@implementation HKMainNib
+ (void)load {
    [[NSNotificationCenter defaultCenter] addObserverForName:@"NSApplicationWillFinishLaunchingNotification" object:nil queue:nil
                                                  usingBlock:^(NSNotification *note) { adopt_main_nib_delegate(); }];
}
@end
