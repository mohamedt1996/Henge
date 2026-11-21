/* Henge's AppKit, the Mac-only C calls a Mac app makes outside AppKit: Carbon and ApplicationServices (HIToolbox
   events and text input sources, accessibility, printing), CoreGraphics's displays, windows and events (one virtual
   display, the presenter's), CoreVideo's display link (a real 60 Hz timer thread), the keychain and authorization,
   LaunchServices and Apple events, SystemConfiguration, and a few others. Calls that would hand back something the
   system does not have here fail the way the real ones do when refused, clearing their out-parameters.

   Each function keeps the real prototype (read from the macOS SDK) but is defined under an hk_ name carrying the
   real symbol as an asm label: on the Mac, Foundation already declares most of these with the SDK's own types, and
   on iOS many are declared unavailable. Types here are this file's own, ABI-identical to the SDK's. */
#import "HKShim.h"
#import <CoreText/CoreText.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>

#define HK_FN(ret, name, args) \
    ret hk_##name args __asm__("_" #name); \
    ret hk_##name args
#define HK_CFSTRING(name, value) const CFStringRef hk_##name __asm__("_" #name) = CFSTR(value)
#define HK_NSSTRING(name, value) NSString *const hk_##name __asm__("_" #name) = @value

typedef int32_t HKCGError;
typedef int32_t HKAXError;
enum {
    kHKCGErrorFailure = 1000, kHKAXErrorFailure = -25200, kHKEventNotHandled = -9874, kHKEventParameterNotFound = -9870,
    kHKEventLoopTimedOut = -9875, kHKEventHotKeyInvalid = -9879, kHKProcNotFound = -600, kHKPMNotImplemented = -30873,
    kHKErrSecUnimplemented = -4, kHKErrSecItemNotFound = -25300, kHKErrSecNoTrustSettings = -25263,
    kHKErrAuthorizationDenied = -60005, kHKErrAEEventNotHandled = -1708, kHKLSUnknownErr = -10810,
};
static const OSStatus kHKUnsupportedOperation = 'unop'; /* CoreAudio's and CoreMediaIO's */

/* ================================================================ HIToolbox: events, hot keys, text input sources */

typedef struct {
    uint32_t eventClass, eventKind;
} HKEventTypeSpec;
typedef struct {
    uint32_t signature, id;
} HKEventHotKeyID;
typedef OSStatus (*HKEventHandlerUPP)(void *callRef, void *event, void *userData);

static int g_event_target;

HK_FN(void *, GetApplicationEventTarget, (void)) { return &g_event_target; }
HK_FN(OSStatus, InstallEventHandler, (void *target, HKEventHandlerUPP handler, unsigned long ntypes, const HKEventTypeSpec *types,
                                      void *userData, void **outRef)) {
    if (outRef) *outRef = NULL;
    return kHKEventNotHandled;
}
HK_FN(OSStatus, RemoveEventHandler, (void *handler)) { return 0; }
HK_FN(OSStatus, CallNextEventHandler, (void *callRef, void *event)) { return kHKEventNotHandled; }
HK_FN(uint32_t, GetEventKind, (void *event)) { return 0; }
HK_FN(OSStatus, GetEventParameter, (void *event, uint32_t name, uint32_t desiredType, uint32_t *actualType,
                                    unsigned long bufferSize, unsigned long *actualSize, void *data)) {
    if (actualType) *actualType = 0;
    if (actualSize) *actualSize = 0;
    return kHKEventParameterNotFound;
}
HK_FN(OSStatus, ReceiveNextEvent, (unsigned long ntypes, const HKEventTypeSpec *types, double timeout, Boolean pull, void **outEvent)) {
    if (outEvent) *outEvent = NULL;
    return kHKEventLoopTimedOut;
}
HK_FN(void, ReleaseEvent, (void *event)) {}
HK_FN(OSStatus, AEProcessEvent, (void *event)) { return kHKErrAEEventNotHandled; }
HK_FN(OSStatus, RegisterEventHotKey, (uint32_t keyCode, uint32_t modifiers, HKEventHotKeyID hotKeyID, void *target,
                                      uint32_t options, void **outRef)) {
    if (outRef) *outRef = NULL;
    return kHKEventHotKeyInvalid;
}
HK_FN(OSStatus, UnregisterEventHotKey, (void *hotKey)) { return kHKEventHotKeyInvalid; }
HK_FN(OSStatus, EnableSecureEventInput, (void)) { return 0; }
HK_FN(OSStatus, DisableSecureEventInput, (void)) { return 0; }
HK_FN(uint32_t, GetCurrentKeyModifiers, (void)) { return 0; }
HK_FN(uint8_t, LMGetKbdType, (void)) { return 40; /* an ANSI keyboard */ }
HK_FN(uint8_t, LMGetKbdLast, (void)) { return 40; }
HK_FN(uint32_t, KBGetLayoutType, (int16_t keyboardType)) { return 'ANSI'; /* kKeyboardANSI */ }
HK_FN(OSStatus, TSMSetDocumentProperty, (void *doc, uint32_t tag, uint32_t size, void *data)) { return 0; }
HK_FN(OSStatus, TSMRemoveDocumentProperty, (void *doc, uint32_t tag)) { return 0; }

HK_CFSTRING(kTISPropertyInputSourceID, "TISPropertyInputSourceID");
HK_CFSTRING(kTISPropertyUnicodeKeyLayoutData, "TISPropertyUnicodeKeyLayoutData");

/* The one input source: the US keyboard layout, as a dictionary of its properties. */
static CFDictionaryRef us_keyboard(void) CF_RETURNS_RETAINED;
static CFDictionaryRef us_keyboard(void) {
    NSDictionary *d = @{
        (__bridge NSString *)hk_kTISPropertyInputSourceID : @"com.apple.keylayout.US",
        @"TISPropertyLocalizedName" : @"U.S.",
        @"TISPropertyInputSourceType" : @"TISTypeKeyboardLayout",
        @"TISPropertyInputSourceCategory" : @"TISCategoryKeyboardInputSource",
        @"TISPropertyInputSourceIsASCIICapable" : @YES,
        @"TISPropertyInputSourceIsEnabled" : @YES,
        @"TISPropertyInputSourceIsSelected" : @YES,
        @"TISPropertyInputSourceLanguages" : @[ @"en" ],
        /* a placeholder for the layout's 'uchr' data (UCKeyTranslate below knows the US layout without it); GLFW
           refuses to start without some */
        (__bridge NSString *)hk_kTISPropertyUnicodeKeyLayoutData : [NSData dataWithBytes:"uchr-us\0" length:8],
    };
    return CFBridgingRetain(d);
}
HK_FN(CFTypeRef, TISCopyCurrentKeyboardInputSource, (void)) { return us_keyboard(); }
HK_FN(CFTypeRef, TISCopyCurrentKeyboardLayoutInputSource, (void)) { return us_keyboard(); }
HK_FN(CFTypeRef, TISCopyCurrentASCIICapableKeyboardLayoutInputSource, (void)) { return us_keyboard(); }
HK_FN(CFArrayRef, TISCreateASCIICapableInputSourceList, (void)) {
    CFDictionaryRef s = us_keyboard();
    CFArrayRef a = CFArrayCreate(NULL, (const void **)&s, 1, &kCFTypeArrayCallBacks);
    CFRelease(s);
    return a;
}
HK_FN(void *, TISGetInputSourceProperty, (CFTypeRef source, CFStringRef key)) {
    if (!source || !key || CFGetTypeID(source) != CFDictionaryGetTypeID()) return NULL;
    return (void *)CFDictionaryGetValue((CFDictionaryRef)source, key);
}
/* The US layout's characters by Mac key code (ANSI positions), plain and with Shift. */
static const char kUSPlain[52] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\t `";
static const char kUSShift[52] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\0LJ\"K:|<?NM>\t ~";
HK_FN(OSStatus, UCKeyTranslate, (const void *layout, uint16_t keyCode, uint16_t action, uint32_t modifiers, uint32_t keyboardType,
                                 uint32_t options, uint32_t *deadKeyState, unsigned long maxLength, unsigned long *actualLength,
                                 UniChar *chars)) {
    /* modifiers: EventModifiers >> 8, where shift is 0x02 and caps lock 0x04 */
    if (deadKeyState) *deadKeyState = 0;
    unichar c = 0;
    if (keyCode < 51) c = (unsigned char)((modifiers & 0x02) ? kUSShift : kUSPlain)[keyCode];
    if (c && (modifiers & 0x04) && c >= 'a' && c <= 'z') c -= 32;
    if (keyCode == 36) c = '\r';
    if (keyCode == 51) c = 8;
    if (keyCode == 53) c = 27;
    if (actualLength) *actualLength = c && maxLength ? 1 : 0;
    if (c && maxLength && chars) chars[0] = c;
    return 0;
}

/* ================================================================ processes */

HK_FN(OSStatus, TransformProcessType, (const void *psn, uint32_t state)) { return 0; }
HK_FN(OSStatus, SetApplicationIsDaemon, (Boolean isDaemon)) { return 0; }
HK_FN(OSErr, GetProcessInformation, (const void *psn, void *info)) { return kHKProcNotFound; }
HK_FN(pid_t, responsibility_get_pid_responsible_for_pid, (pid_t pid)) { return pid; }
HK_FN(int, responsibility_spawnattrs_setdisclaim, (posix_spawnattr_t *attrs, bool disclaim)) { return 0; }

/* The program's JIT write toggle. On the Mac it is passed to libSystem's (anything else linked with this file may
   rely on it); on iOS there is nothing to toggle. */
HK_FN(void, pthread_jit_write_protect_np, (int enabled)) {
#if TARGET_OS_OSX
    static void (*real)(int);
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *h = dlopen("/usr/lib/system/libsystem_pthread.dylib", RTLD_LAZY | RTLD_NOLOAD);
        if (h) real = (void (*)(int))dlsym(h, "pthread_jit_write_protect_np");
    });
    if (real) real(enabled);
#endif
}

/* ================================================================ accessibility (HIServices): no assistive client */

HK_FN(HKAXError, AXObserverCreateWithInfoCallback, (pid_t pid, void *callback, void **outObserver)) {
    if (outObserver) *outObserver = NULL;
    return kHKAXErrorFailure;
}
HK_FN(HKAXError, AXObserverAddNotification, (void *observer, void *element, CFStringRef notification, void *refcon)) {
    return kHKAXErrorFailure;
}
HK_FN(CFRunLoopSourceRef, AXObserverGetRunLoopSource, (void *observer)) { return NULL; }
HK_FN(void *, AXUIElementCreateApplication, (pid_t pid)) { return NULL; }
HK_FN(CFTypeID, AXUIElementGetTypeID, (void)) { return 0; /* _kCFRuntimeNotATypeID: matches nothing */ }
HK_FN(HKAXError, AXUIElementCopyAttributeNames, (void *element, CFArrayRef *names)) {
    if (names) *names = NULL;
    return kHKAXErrorFailure;
}
HK_FN(HKAXError, AXUIElementCopyActionNames, (void *element, CFArrayRef *names)) {
    if (names) *names = NULL;
    return kHKAXErrorFailure;
}
HK_FN(HKAXError, AXUIElementCopyParameterizedAttributeNames, (void *element, CFArrayRef *names)) {
    if (names) *names = NULL;
    return kHKAXErrorFailure;
}
HK_FN(HKAXError, AXUIElementCopyAttributeValue, (void *element, CFStringRef attribute, CFTypeRef *value)) {
    if (value) *value = NULL;
    return kHKAXErrorFailure;
}
HK_FN(HKAXError, AXUIElementCopyParameterizedAttributeValue, (void *element, CFStringRef attribute, CFTypeRef parameter,
                                                            CFTypeRef *result)) {
    if (result) *result = NULL;
    return kHKAXErrorFailure;
}
HK_FN(HKAXError, AXUIElementSetAttributeValue, (void *element, CFStringRef attribute, CFTypeRef value)) { return kHKAXErrorFailure; }
HK_FN(HKAXError, AXUIElementPerformAction, (void *element, CFStringRef action)) { return kHKAXErrorFailure; }

/* Text markers are opaque byte strings (CFData here) and marker ranges pairs of them (CFArray), so a program's own
   accessibility code that makes and reads them works. AXValues are CFData too: a 4-byte type, then the value. */
HK_FN(CFTypeRef, AXTextMarkerCreate, (CFAllocatorRef allocator, const UInt8 *bytes, CFIndex length)) {
    return CFDataCreate(allocator, bytes, length);
}
HK_FN(const UInt8 *, AXTextMarkerGetBytePtr, (CFTypeRef marker)) { return marker ? CFDataGetBytePtr((CFDataRef)marker) : NULL; }
HK_FN(CFIndex, AXTextMarkerGetLength, (CFTypeRef marker)) { return marker ? CFDataGetLength((CFDataRef)marker) : 0; }
HK_FN(CFTypeID, AXTextMarkerGetTypeID, (void)) { return CFDataGetTypeID(); }
HK_FN(CFTypeRef, AXTextMarkerRangeCreate, (CFAllocatorRef allocator, CFTypeRef start, CFTypeRef end)) {
    if (!start || !end) return NULL;
    const void *pair[2] = {start, end};
    return CFArrayCreate(allocator, pair, 2, &kCFTypeArrayCallBacks);
}
HK_FN(CFTypeRef, AXTextMarkerRangeCopyStartMarker, (CFTypeRef range)) {
    return range && CFArrayGetCount((CFArrayRef)range) == 2 ? CFRetain(CFArrayGetValueAtIndex((CFArrayRef)range, 0)) : NULL;
}
HK_FN(CFTypeRef, AXTextMarkerRangeCopyEndMarker, (CFTypeRef range)) {
    return range && CFArrayGetCount((CFArrayRef)range) == 2 ? CFRetain(CFArrayGetValueAtIndex((CFArrayRef)range, 1)) : NULL;
}
HK_FN(CFTypeID, AXTextMarkerRangeGetTypeID, (void)) { return CFArrayGetTypeID(); }

static size_t ax_value_size(uint32_t type) {
    switch (type) {
    case 1: return sizeof(CGPoint);
    case 2: return sizeof(CGSize);
    case 3: return sizeof(CGRect);
    case 4: return sizeof(CFRange);
    case 5: return sizeof(int32_t); /* AXError */
    default: return 0;
    }
}
HK_FN(CFTypeRef, AXValueCreate, (uint32_t type, const void *value)) {
    size_t n = ax_value_size(type);
    if (!n || !value) return NULL;
    CFMutableDataRef d = CFDataCreateMutable(NULL, 0);
    CFDataAppendBytes(d, (const UInt8 *)&type, sizeof type);
    CFDataAppendBytes(d, value, (CFIndex)n);
    return d;
}
HK_FN(uint32_t, AXValueGetType, (CFTypeRef value)) {
    uint32_t type = 0;
    if (value && CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)value) >= (CFIndex)sizeof type)
        memcpy(&type, CFDataGetBytePtr((CFDataRef)value), sizeof type);
    return ax_value_size(type) && CFDataGetLength((CFDataRef)value) == (CFIndex)(sizeof type + ax_value_size(type)) ? type : 0;
}
HK_FN(CFTypeID, AXValueGetTypeID, (void)) { return CFDataGetTypeID(); }
HK_FN(Boolean, AXValueGetValue, (CFTypeRef value, uint32_t type, void *out)) {
    if (!out || !type || hk_AXValueGetType(value) != type) return false;
    memcpy(out, CFDataGetBytePtr((CFDataRef)value) + sizeof type, ax_value_size(type));
    return true;
}
HK_FN(Boolean, UAZoomEnabled, (void)) { return false; }
HK_FN(OSStatus, UAZoomChangeFocus, (const CGRect *rect, const CGRect *highlight, uint32_t type)) { return 0; }

/* ================================================================ printing (PrintCore): no printers */

#define PM_OUT(p) \
    do { \
        if (p) *(p) = NULL; \
    } while (0)
HK_FN(OSStatus, PMRelease, (void *object)) { return 0; }
HK_FN(OSStatus, PMCopyPageFormat, (void *src, void *dst)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMCopyPrintSettings, (void *src, void *dst)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMCreatePageFormatWithPMPaper, (void **format, void *paper)) {
    PM_OUT(format);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMGetAdjustedPageRect, (void *format, void *rect)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMGetAdjustedPaperRect, (void *format, void *rect)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMGetOrientation, (void *format, uint16_t *orientation)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMGetPageFormatPaper, (void *format, void **paper)) {
    PM_OUT(paper);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPageFormatCreateDataRepresentation, (void *format, CFDataRef *data, uint32_t dataFormat)) {
    PM_OUT(data);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPageFormatCreateWithDataRepresentation, (CFDataRef data, void **format)) {
    PM_OUT(format);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPaperCreateCustom, (void *printer, CFStringRef paperID, CFStringRef name, double width, double height,
                                      const void *margins, void **paper)) {
    PM_OUT(paper);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPaperGetHeight, (void *paper, double *height)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMPaperGetWidth, (void *paper, double *width)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMPaperGetMargins, (void *paper, void *margins)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMPaperGetPPDPaperName, (void *paper, CFStringRef *name)) {
    PM_OUT(name);
    return kHKPMNotImplemented;
}
HK_FN(void *, PMPrinterCreateFromPrinterID, (CFStringRef printerID)) { return NULL; }
HK_FN(CFStringRef, PMPrinterGetID, (void *printer)) { return NULL; }
HK_FN(OSStatus, PMPrinterGetIndexedPrinterResolution, (void *printer, uint32_t index, void *resolution)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMPrinterGetPaperList, (void *printer, CFArrayRef *list)) {
    PM_OUT(list);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPrinterGetPrinterResolutionCount, (void *printer, uint32_t *count)) {
    if (count) *count = 0;
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPrinterSetOutputResolution, (void *printer, void *settings, const void *resolution)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMPrintSettingsCreateDataRepresentation, (void *settings, CFDataRef *data, uint32_t format)) {
    PM_OUT(data);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPrintSettingsCreateWithDataRepresentation, (CFDataRef data, void **settings)) {
    PM_OUT(settings);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMPrintSettingsSetJobName, (void *settings, CFStringRef name)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMPrintSettingsSetValue, (void *settings, CFStringRef key, CFTypeRef value, Boolean locked)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionBeginCGDocumentNoDialog, (void *session, void *settings, void *format)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionBeginPageNoDialog, (void *session, void *format, const void *frame)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionEndDocumentNoDialog, (void *session)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionEndPageNoDialog, (void *session)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionCopyDestinationFormat, (void *session, void *settings, CFStringRef *format)) {
    PM_OUT(format);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSessionCopyDestinationLocation, (void *session, void *settings, CFURLRef *location)) {
    PM_OUT(location);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSessionGetCGGraphicsContext, (void *session, CGContextRef *context)) {
    PM_OUT(context);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSessionGetCurrentPrinter, (void *session, void **printer)) {
    PM_OUT(printer);
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSessionGetDestinationType, (void *session, void *settings, uint16_t *type)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionSetCurrentPMPrinter, (void *session, void *printer)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSessionSetDestination, (void *session, void *settings, uint16_t type, CFStringRef format, CFURLRef location)) {
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSessionValidatePageFormat, (void *session, void *format, Boolean *changed)) {
    if (changed) *changed = false;
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSessionValidatePrintSettings, (void *session, void *settings, Boolean *changed)) {
    if (changed) *changed = false;
    return kHKPMNotImplemented;
}
HK_FN(OSStatus, PMSetCollate, (void *settings, Boolean collate)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSetCopies, (void *settings, uint32_t copies, Boolean lock)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSetDuplex, (void *settings, uint32_t duplex)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSetFirstPage, (void *settings, uint32_t first, Boolean lock)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSetLastPage, (void *settings, uint32_t last, Boolean lock)) { return kHKPMNotImplemented; }
HK_FN(OSStatus, PMSetOrientation, (void *format, uint16_t orientation, Boolean lock)) { return kHKPMNotImplemented; }
#undef PM_OUT

/* ================================================================ CoreGraphics: one display (the presenter's screen) */

static const uint32_t kMainDisplay = 1;

static CGSize screen_points(void) {
    const HKPresenter *p = hk_presenter();
    return p && p->screen_size.width > 0 && p->screen_size.height > 0 ? p->screen_size : CGSizeMake(1280, 800);
}

HK_FN(uint32_t, CGMainDisplayID, (void)) { return kMainDisplay; }
/* The one display's UUID, fixed (winit refuses a display without one); the reverse for the main display only. */
HK_FN(CFUUIDRef, CGDisplayCreateUUIDFromDisplayID, (uint32_t display)) {
    return display == kMainDisplay ? CFUUIDCreateFromString(NULL, CFSTR("6E05DE6A-8E1F-4C1E-9D3A-48454E474531")) : NULL;
}
HK_FN(uint32_t, CGDisplayGetDisplayIDFromUUID, (CFUUIDRef uuid)) { return uuid ? kMainDisplay : 0; }
/* The display's place and size: the presenter's screen, in points and in pixels. */
static CGSize hk_display_points(void) {
    const HKPresenter *p = hk_presenter();
    return p && p->screen_size.width > 0 ? p->screen_size : CGSizeMake(1280, 800);
}
static CGFloat hk_display_scale(void) {
    const HKPresenter *p = hk_presenter();
    return p && p->scale > 0 ? p->scale : 2;
}
HK_FN(CGRect, CGDisplayBounds, (uint32_t display)) {
    CGSize s = hk_display_points();
    return display == kMainDisplay ? CGRectMake(0, 0, s.width, s.height) : CGRectZero;
}
HK_FN(size_t, CGDisplayPixelsWide, (uint32_t display)) { return display == kMainDisplay ? (size_t)(hk_display_points().width * hk_display_scale()) : 0; }
HK_FN(size_t, CGDisplayPixelsHigh, (uint32_t display)) { return display == kMainDisplay ? (size_t)(hk_display_points().height * hk_display_scale()) : 0; }
static HKCGError one_display(uint32_t max, uint32_t *displays, uint32_t *count) {
    if (displays && max > 0) displays[0] = kMainDisplay;
    if (count) *count = displays ? (max > 0 ? 1 : 0) : 1;
    return 0;
}
HK_FN(HKCGError, CGGetActiveDisplayList, (uint32_t max, uint32_t *displays, uint32_t *count)) { return one_display(max, displays, count); }
HK_FN(HKCGError, CGGetOnlineDisplayList, (uint32_t max, uint32_t *displays, uint32_t *count)) { return one_display(max, displays, count); }
HK_FN(CGColorSpaceRef, CGDisplayCopyColorSpace, (uint32_t display)) { return CGColorSpaceCreateWithName(kCGColorSpaceSRGB); }
/* A display mode is a dictionary here; its getters answer for the one display whatever mode they are given. */
HK_FN(CFTypeRef, CGDisplayCopyDisplayMode, (uint32_t display)) {
    CGSize s = screen_points();
    return CFBridgingRetain(@{@"Width" : @(s.width), @"Height" : @(s.height), @"RefreshRate" : @60});
}
HK_FN(size_t, CGDisplayModeGetWidth, (CFTypeRef mode)) { return (size_t)screen_points().width; }
HK_FN(size_t, CGDisplayModeGetHeight, (CFTypeRef mode)) { return (size_t)screen_points().height; }
HK_FN(double, CGDisplayModeGetRefreshRate, (CFTypeRef mode)) { return 60; }
HK_FN(void, CGDisplayModeRelease, (CFTypeRef mode)) {
    if (mode) CFRelease(mode);
}
HK_FN(CFTypeRef, CGDisplayModeRetain, (CFTypeRef mode)) { return mode ? CFRetain(mode) : NULL; }
HK_FN(size_t, CGDisplayModeGetPixelWidth, (CFTypeRef mode)) { return (size_t)(screen_points().width * hk_display_scale()); }
HK_FN(size_t, CGDisplayModeGetPixelHeight, (CFTypeRef mode)) { return (size_t)(screen_points().height * hk_display_scale()); }
HK_FN(int32_t, CGDisplayModeGetIODisplayModeID, (CFTypeRef mode)) { return 1; }
HK_FN(uint32_t, CGDisplayModeGetIOFlags, (CFTypeRef mode)) { return 0x7; /* valid, safe, default */ }
HK_FN(bool, CGDisplayModeIsUsableForDesktopGUI, (CFTypeRef mode)) { return true; }
HK_FN(CFStringRef, CGDisplayModeCopyPixelEncoding, (CFTypeRef mode)) { return CFSTR("--------RRRRRRRRGGGGGGGGBBBBBBBB"); }
/* Every mode the display has: its one. */
HK_FN(CFArrayRef, CGDisplayCopyAllDisplayModes, (uint32_t display, CFDictionaryRef options)) {
    if (display != kMainDisplay) return NULL;
    CFTypeRef mode = hk_CGDisplayCopyDisplayMode(display);
    CFArrayRef modes = CFArrayCreate(NULL, &mode, 1, &kCFTypeArrayCallBacks);
    CFRelease(mode);
    return modes;
}
HK_FN(uint32_t, CGDisplayIsMain, (uint32_t display)) { return display == kMainDisplay; }
HK_FN(uint32_t, CGDisplayIsActive, (uint32_t display)) { return display == kMainDisplay; }
HK_FN(uint32_t, CGDisplayIsOnline, (uint32_t display)) { return display == kMainDisplay; }
HK_FN(uint32_t, CGDisplayIsAsleep, (uint32_t display)) { return 0; }
/* The display's physical size in millimetres, at a Mac's 110 points per inch. */
HK_FN(CGSize, CGDisplayScreenSize, (uint32_t display)) {
    CGSize s = hk_display_points();
    return display == kMainDisplay ? CGSizeMake(s.width * 25.4 / 110, s.height * 25.4 / 110) : CGSizeZero;
}
HK_FN(uint32_t, CGDisplayVendorNumber, (uint32_t display)) { return 0x610; /* Apple */ }
HK_FN(uint32_t, CGDisplayModelNumber, (uint32_t display)) { return 1; }
HK_FN(uint32_t, CGDisplaySerialNumber, (uint32_t display)) { return 1; }
HK_FN(uint32_t, CGDisplayUnitNumber, (uint32_t display)) { return 0; }
/* The pointer is the finger's (or the trackpad's): hiding it is the screen's business. */
HK_FN(HKCGError, CGDisplayHideCursor, (uint32_t display)) { return 0; }
HK_FN(HKCGError, CGDisplayShowCursor, (uint32_t display)) { return 0; }
HK_FN(uint32_t, CGDisplayIsBuiltin, (uint32_t display)) { return 1; }
HK_FN(double, CGDisplayRotation, (uint32_t display)) { return 0; }
HK_FN(uint32_t, CGDisplayMirrorsDisplay, (uint32_t display)) { return 0; /* kCGNullDirectDisplay */ }
HK_FN(uint32_t, CGDisplayUsesForceToGray, (void)) { return 0; }
HK_FN(uint32_t, CGDisplayIDToOpenGLDisplayMask, (uint32_t display)) { return 1; }
id hk_CGDirectDisplayCopyCurrentMetalDevice(uint32_t display) __asm__("_CGDirectDisplayCopyCurrentMetalDevice") NS_RETURNS_RETAINED;
id hk_CGDirectDisplayCopyCurrentMetalDevice(uint32_t display) { return MTLCreateSystemDefaultDevice(); }
HK_FN(HKCGError, CGDisplayRegisterReconfigurationCallback, (void *callback, void *userInfo)) { return 0; }
HK_FN(HKCGError, CGDisplayRemoveReconfigurationCallback, (void *callback, void *userInfo)) { return 0; }
HK_FN(CGImageRef, CGDisplayCreateImage, (uint32_t display)) { return NULL; }
HK_FN(CGImageRef, CGWindowListCreateImage, (CGRect bounds, uint32_t option, uint32_t window, uint32_t imageOption)) { return NULL; }
HK_FN(CGImageRef, CGWindowListCreateImageFromArray, (CGRect bounds, CFArrayRef windows, uint32_t imageOption)) { return NULL; }
HK_FN(CFArrayRef, CGWindowListCopyWindowInfo, (uint32_t option, uint32_t relativeTo)) {
    return CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
}
HK_FN(CFArrayRef, CGWindowListCreateDescriptionFromArray, (CFArrayRef windows)) {
    return CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
}
HK_FN(Boolean, CGPreflightScreenCaptureAccess, (void)) { return false; }
HK_FN(Boolean, CGRequestScreenCaptureAccess, (void)) { return false; }
HK_FN(HKCGError, CGWarpMouseCursorPosition, (CGPoint point)) { return 0; }
HK_FN(HKCGError, CGAssociateMouseAndMouseCursorPosition, (uint32_t connected)) { return 0; }
HK_FN(bool, CGFontRenderingGetFontSmoothingDisabled, (void)) { return false; }

HK_FN(void *, CGDisplayStreamCreate, (uint32_t display, size_t width, size_t height, int32_t format, CFDictionaryRef properties,
                                      id handler)) {
    return NULL;
}
HK_FN(CFRunLoopSourceRef, CGDisplayStreamGetRunLoopSource, (void *stream)) { return NULL; }
HK_FN(HKCGError, CGDisplayStreamStart, (void *stream)) { return kHKCGErrorFailure; }
HK_FN(HKCGError, CGDisplayStreamStop, (void *stream)) { return kHKCGErrorFailure; }
HK_FN(const CGRect *, CGDisplayStreamUpdateGetRects, (void *update, int32_t type, size_t *count)) {
    if (count) *count = 0;
    return NULL;
}

HK_FN(void *, CGEventCreate, (void *source)) { return NULL; }
HK_FN(CFDataRef, CGEventCreateData, (CFAllocatorRef allocator, void *event)) { return NULL; }
HK_FN(void *, CGEventCreateFromData, (CFAllocatorRef allocator, CFDataRef data)) { return NULL; }
HK_FN(void *, CGEventCreateNextEvent, (int32_t connection)) { return NULL; }
/* An NSEvent's CGEvent is the NSEvent itself (HKCore.m): the fields apps read from it come from the NSEvent. */
static NSEvent *hk_event_of(void *event) {
    id e = (__bridge id)event;
    return e && [e isKindOfClass:[NSEvent class]] ? (NSEvent *)e : nil;
}
HK_FN(int64_t, CGEventGetIntegerValueField, (void *event, uint32_t field)) {
    NSEvent *e = hk_event_of(event);
    if (!e) return 0;
    switch (field) {
    case 1: return e.clickCount;                                     /* kCGMouseEventClickState */
    case 3: return e.buttonNumber;                                   /* kCGMouseEventButtonNumber */
    case 9: return e.keyCode;                                        /* kCGKeyboardEventKeycode */
    case 8: return e.isARepeat;                                      /* kCGKeyboardEventAutorepeat */
    case 11: return (int64_t)lround(e.scrollingDeltaY / 10);         /* kCGScrollWheelEventDeltaAxis1 */
    case 12: return (int64_t)lround(e.scrollingDeltaX / 10);         /* kCGScrollWheelEventDeltaAxis2 */
    case 88: return e.hasPreciseScrollingDeltas;                     /* kCGScrollWheelEventIsContinuous */
    case 93: return (int64_t)(e.scrollingDeltaY / 10 * 65536);       /* kCGScrollWheelEventFixedPtDeltaAxis1 */
    case 94: return (int64_t)(e.scrollingDeltaX / 10 * 65536);       /* kCGScrollWheelEventFixedPtDeltaAxis2 */
    case 96: return (int64_t)lround(e.scrollingDeltaY);              /* kCGScrollWheelEventPointDeltaAxis1 */
    case 97: return (int64_t)lround(e.scrollingDeltaX);              /* kCGScrollWheelEventPointDeltaAxis2 */
    case 99: return e.phase;                                         /* kCGScrollWheelEventScrollPhase */
    case 123: return e.momentumPhase;                                /* kCGScrollWheelEventMomentumPhase */
    default: return 0;
    }
}
HK_FN(CGPoint, CGEventGetLocation, (void *event)) {
    NSEvent *e = hk_event_of(event);
    if (!e || !e.window) return CGPointZero;
    NSPoint p = [e.window convertPointToScreen:e.locationInWindow];
    return CGPointMake(p.x, hk_presenter()->screen_size.height - p.y); /* CG's global coordinates: origin top left */
}
HK_FN(uint32_t, CGEventGetType, (void *event)) {
    NSEvent *e = hk_event_of(event);
    return e ? (uint32_t)e.type : 0; /* NSEventType and CGEventType agree for mouse, key and scroll events */
}
HK_FN(bool, CGEventSourceKeyState, (int32_t state, uint16_t key)) { return false; }
HK_FN(double, CGEventSourceSecondsSinceLastEventType, (int32_t state, uint32_t type)) { return 0; }
HK_FN(CFMachPortRef, CGEventTapCreate, (uint32_t tap, uint32_t place, uint32_t options, uint64_t mask, void *callback, void *userInfo)) {
    return NULL;
}
HK_FN(void, CGEventTapEnable, (CFMachPortRef tap, bool enable)) {}

/* The private window-server calls: there is no window server connection. */
HK_FN(int32_t, _CGSDefaultConnection, (void)) { return 0; }
HK_FN(int32_t, CGSMainConnectionID, (void)) { return 0; }
HK_FN(HKCGError, CGSGetEventPort, (int32_t connection, mach_port_t *port)) {
    if (port) *port = MACH_PORT_NULL;
    return 0;
}
HK_FN(HKCGError, CGSSetDenyWindowServerConnections, (bool deny)) { return 0; }
HK_FN(HKCGError, CGSSetWindowAlpha, (int32_t connection, uint32_t window, float alpha)) { return 0; }
HK_FN(HKCGError, CGSSetWindowTransform, (int32_t connection, uint32_t window, CGAffineTransform transform)) { return 0; }
HK_FN(HKCGError, CGSSetWindowWarp, (int32_t connection, uint32_t window, int w, int h, const void *mesh)) { return 0; }
HK_FN(HKCGError, CGSSetWindowCaptureExcludeShape, (int32_t connection, uint32_t window, const void *region)) { return 0; }

HK_CFSTRING(kCGWindowBounds, "kCGWindowBounds");
HK_CFSTRING(kCGWindowIsOnscreen, "kCGWindowIsOnscreen");
HK_CFSTRING(kCGWindowLayer, "kCGWindowLayer");
HK_CFSTRING(kCGWindowName, "kCGWindowName");
HK_CFSTRING(kCGWindowNumber, "kCGWindowNumber");
HK_CFSTRING(kCGWindowOwnerName, "kCGWindowOwnerName");
HK_CFSTRING(kCGWindowOwnerPID, "kCGWindowOwnerPID");
HK_CFSTRING(kCGDisplayStreamColorSpace, "kCGDisplayStreamColorSpace");
HK_CFSTRING(kCGDisplayStreamDestinationRect, "kCGDisplayStreamDestinationRect");
HK_CFSTRING(kCGDisplayStreamMinimumFrameTime, "kCGDisplayStreamMinimumFrameTime");
HK_CFSTRING(kCGDisplayStreamPreserveAspectRatio, "kCGDisplayStreamPreserveAspectRatio");
HK_CFSTRING(kCGDisplayStreamShowCursor, "kCGDisplayStreamShowCursor");

/* ================================================================ CoreVideo's display link: a 60 Hz thread */

/* The link calls its output callback from its own thread every 1/60 s, with timestamps in CoreVideo's layout: video
   time in nanoseconds of mach time, host time in mach units. Each start makes a thread with a new generation, which
   runs while the link's generation is its own; a thread stopped from inside its own callback finishes on its own,
   and the last such thread frees a link released meanwhile. */
typedef struct HKDisplayLink {
    _Atomic int refs;
    _Atomic uint64_t gen; /* the running thread's generation; 0 when stopped */
    pthread_mutex_t lock; /* guards the rest */
    void *callback, *context;
    pthread_t thread;
    bool has_thread, free_on_exit;
    int live_threads;
    uint64_t next_gen;
} HKDisplayLink;

typedef CVReturn (*HKDisplayLinkCallback)(HKDisplayLink *, const CVTimeStamp *, const CVTimeStamp *, CVOptionFlags, CVOptionFlags *,
                                          void *);
typedef struct {
    HKDisplayLink *link;
    uint64_t gen;
} HKLinkThreadArg;

static const int64_t kRefreshNs = 16666667;

static uint64_t ns_of_mach(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return t * tb.numer / tb.denom;
}
static uint64_t mach_of_ns(uint64_t ns) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return ns * tb.denom / tb.numer;
}
static void fill_stamp(CVTimeStamp *ts, uint64_t host) {
    memset(ts, 0, sizeof *ts);
    ts->version = 0;
    ts->videoTimeScale = 1000000000;
    ts->videoTime = (int64_t)ns_of_mach(host);
    ts->hostTime = host;
    ts->rateScalar = 1;
    ts->videoRefreshPeriod = kRefreshNs;
    ts->flags = kCVTimeStampVideoTimeValid | kCVTimeStampHostTimeValid | kCVTimeStampVideoRefreshPeriodValid;
}

static void link_free(HKDisplayLink *link) {
    pthread_mutex_destroy(&link->lock);
    free(link);
}

static void *link_thread(void *arg) {
    HKLinkThreadArg a = *(HKLinkThreadArg *)arg;
    free(arg);
    HKDisplayLink *link = a.link;
    pthread_setname_np("CVDisplayLink");
    uint64_t period = mach_of_ns(kRefreshNs), next = mach_absolute_time() + period;
    while (atomic_load(&link->gen) == a.gen) {
        mach_wait_until(next);
        if (atomic_load(&link->gen) != a.gen) break;
        CVTimeStamp now, out;
        fill_stamp(&now, mach_absolute_time());
        fill_stamp(&out, next + period);
        pthread_mutex_lock(&link->lock);
        HKDisplayLinkCallback cb = (HKDisplayLinkCallback)link->callback;
        void *ctx = link->context;
        pthread_mutex_unlock(&link->lock);
        if (cb) {
            CVOptionFlags flagsOut = 0;
            @autoreleasepool {
                cb(link, &now, &out, 0, &flagsOut, ctx);
            }
        }
        next += period;
        uint64_t t = mach_absolute_time();
        if (next < t) next = t + period - (t - next) % period; /* skipped frames are not made up */
    }
    pthread_mutex_lock(&link->lock);
    bool last = --link->live_threads == 0 && link->free_on_exit;
    pthread_mutex_unlock(&link->lock);
    if (last) link_free(link);
    return NULL;
}

/* Stops the link: its thread is joined from any other thread, or left to finish when this is that thread. */
static void link_stop(HKDisplayLink *link) {
    pthread_mutex_lock(&link->lock);
    atomic_store(&link->gen, 0);
    bool had = link->has_thread;
    pthread_t t = link->thread;
    link->has_thread = false;
    pthread_mutex_unlock(&link->lock);
    if (!had) return;
    if (pthread_equal(t, pthread_self())) pthread_detach(t);
    else pthread_join(t, NULL);
}

static CVReturn link_create(HKDisplayLink **out) {
    if (!out) return kCVReturnInvalidArgument;
    HKDisplayLink *link = calloc(1, sizeof *link);
    if (!link) return kCVReturnAllocationFailed;
    atomic_init(&link->refs, 1);
    atomic_init(&link->gen, 0);
    pthread_mutex_init(&link->lock, NULL);
    *out = link;
    return kCVReturnSuccess;
}
HK_FN(CVReturn, CVDisplayLinkCreateWithCGDisplay, (uint32_t display, HKDisplayLink **out)) { return link_create(out); }
HK_FN(CVReturn, CVDisplayLinkCreateWithActiveCGDisplays, (HKDisplayLink **out)) { return link_create(out); }
HK_FN(CVReturn, CVDisplayLinkCreateWithCGDisplays, (uint32_t *displays, CFIndex count, HKDisplayLink **out)) { return link_create(out); }
HK_FN(HKDisplayLink *, CVDisplayLinkRetain, (HKDisplayLink *link)) {
    if (link) atomic_fetch_add(&link->refs, 1);
    return link;
}
HK_FN(void, CVDisplayLinkRelease, (HKDisplayLink *link)) {
    if (!link || atomic_fetch_sub(&link->refs, 1) != 1) return;
    link_stop(link);
    pthread_mutex_lock(&link->lock);
    bool threads_left = link->live_threads > 0;
    link->free_on_exit = threads_left;
    pthread_mutex_unlock(&link->lock);
    if (!threads_left) link_free(link);
}
HK_FN(CVReturn, CVDisplayLinkSetOutputCallback, (HKDisplayLink *link, HKDisplayLinkCallback callback, void *context)) {
    if (!link) return kCVReturnInvalidArgument;
    pthread_mutex_lock(&link->lock);
    link->callback = callback;
    link->context = context;
    pthread_mutex_unlock(&link->lock);
    return kCVReturnSuccess;
}
HK_FN(CVReturn, CVDisplayLinkStart, (HKDisplayLink *link)) {
    if (!link) return kCVReturnInvalidArgument;
    pthread_mutex_lock(&link->lock);
    CVReturn r = kCVReturnSuccess;
    if (!link->has_thread) {
        HKLinkThreadArg *arg = malloc(sizeof *arg);
        arg->link = link;
        arg->gen = ++link->next_gen;
        atomic_store(&link->gen, arg->gen);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
        if (pthread_create(&link->thread, &attr, link_thread, arg) == 0) {
            link->has_thread = true;
            link->live_threads++;
        } else {
            free(arg);
            atomic_store(&link->gen, 0);
            r = kCVReturnError;
        }
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&link->lock);
    return r;
}
HK_FN(CVReturn, CVDisplayLinkStop, (HKDisplayLink *link)) {
    if (!link) return kCVReturnInvalidArgument;
    link_stop(link);
    return kCVReturnSuccess;
}
HK_FN(Boolean, CVDisplayLinkIsRunning, (HKDisplayLink *link)) { return link && atomic_load(&link->gen) != 0; }
HK_FN(CVReturn, CVDisplayLinkGetCurrentTime, (HKDisplayLink *link, CVTimeStamp *out)) {
    if (!link || !out) return kCVReturnInvalidArgument;
    fill_stamp(out, mach_absolute_time());
    return kCVReturnSuccess;
}
HK_FN(CVTime, CVDisplayLinkGetNominalOutputVideoRefreshPeriod, (HKDisplayLink *link)) {
    return (CVTime){.timeValue = kRefreshNs, .timeScale = 1000000000, .flags = 0};
}
HK_FN(double, CVDisplayLinkGetActualOutputVideoRefreshPeriod, (HKDisplayLink *link)) { return kRefreshNs / 1e9; }
HK_FN(uint32_t, CVDisplayLinkGetCurrentCGDisplay, (HKDisplayLink *link)) { return kMainDisplay; }
HK_FN(CVReturn, CVDisplayLinkSetCurrentCGDisplay, (HKDisplayLink *link, uint32_t display)) { return kCVReturnSuccess; }

/* ================================================================ Metal, CoreText, CoreAudio, CoreMediaIO */

NSArray *hk_MTLCopyAllDevicesWithObserver(__strong id *observer, id handler) __asm__("_MTLCopyAllDevicesWithObserver")
    NS_RETURNS_RETAINED;
NSArray *hk_MTLCopyAllDevicesWithObserver(__strong id *observer, id handler) {
    if (observer) *observer = nil;
    id device = MTLCreateSystemDefaultDevice();
    return device ? @[ device ] : @[];
}
HK_NSSTRING(MTLDeviceWasRemovedNotification, "MTLDeviceWasRemoved");
HK_NSSTRING(MTLDeviceRemovalRequestedNotification, "MTLDeviceRemovalRequested");

/* Every face of a family (the collection is taken to be all fonts, as CTFontCollectionCreateFromAvailableFonts's). */
HK_FN(CFArrayRef, CTFontCollectionCreateMatchingFontDescriptorsForFamily, (CFTypeRef collection, CFStringRef family,
                                                                          CFDictionaryRef options)) {
    if (!family) return NULL;
    const void *key = kCTFontFamilyNameAttribute;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, (const void **)&family, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
    CFSetRef mandatory = CFSetCreate(NULL, &key, 1, &kCFTypeSetCallBacks);
    CFArrayRef r = CTFontDescriptorCreateMatchingFontDescriptors(d, mandatory);
    CFRelease(mandatory);
    CFRelease(d);
    CFRelease(attrs);
    return r;
}

HK_FN(OSStatus, AudioHardwareCreateProcessTap, (void *description, uint32_t *outTap)) {
    if (outTap) *outTap = 0; /* kAudioObjectUnknown */
    return kHKUnsupportedOperation;
}
HK_FN(OSStatus, AudioHardwareDestroyProcessTap, (uint32_t tap)) { return kHKUnsupportedOperation; }
HK_FN(OSStatus, CMIOObjectGetPropertyData, (uint32_t object, const void *address, uint32_t qualifierSize, const void *qualifier,
                                            uint32_t dataSize, uint32_t *dataUsed, void *data)) {
    if (dataUsed) *dataUsed = 0;
    return kHKUnsupportedOperation;
}

/* ================================================================ Security: no keychain, no authorization */

#define SEC_OUT(p) \
    do { \
        if (p) *(p) = NULL; \
    } while (0)
HK_FN(OSStatus, SecKeychainFindGenericPassword, (CFTypeRef keychains, uint32_t serviceLength, const char *service,
                                                 uint32_t accountLength, const char *account, uint32_t *passwordLength,
                                                 void **passwordData, void **item)) {
    if (passwordLength) *passwordLength = 0;
    SEC_OUT(passwordData);
    SEC_OUT(item);
    return kHKErrSecItemNotFound;
}
HK_FN(OSStatus, SecKeychainCopyDefault, (void **keychain)) {
    SEC_OUT(keychain);
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecKeychainGetStatus, (void *keychain, uint32_t *status)) {
    if (status) *status = 0;
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecKeychainGetUserInteractionAllowed, (Boolean *state)) {
    if (state) *state = true;
    return 0;
}
HK_FN(OSStatus, SecKeychainSetUserInteractionAllowed, (Boolean state)) { return 0; }
HK_FN(OSStatus, SecKeychainAddCallback, (void *callback, uint32_t mask, void *context)) { return kHKErrSecUnimplemented; }
HK_FN(OSStatus, SecKeychainItemCopyKeychain, (void *item, void **keychain)) {
    SEC_OUT(keychain);
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecKeychainItemCreateFromContent, (uint32_t itemClass, void *attributes, uint32_t length, const void *data,
                                                   void *keychain, void *access, void **item)) {
    SEC_OUT(item);
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecKeychainItemDelete, (void *item)) { return kHKErrSecUnimplemented; }
HK_FN(OSStatus, SecKeychainItemFreeContent, (void *attributes, void *data)) { return 0; }
HK_FN(OSStatus, SecAccessCreate, (CFStringRef descriptor, CFArrayRef trusted, void **access)) {
    SEC_OUT(access);
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecAccessCopyACLList, (void *access, CFArrayRef *list)) {
    SEC_OUT(list);
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecACLCopyContents, (void *acl, CFArrayRef *apps, CFStringRef *description, uint16_t *prompt)) {
    SEC_OUT(apps);
    SEC_OUT(description);
    if (prompt) *prompt = 0;
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, SecACLSetContents, (void *acl, CFArrayRef apps, CFStringRef description, uint16_t prompt)) {
    return kHKErrSecUnimplemented;
}
HK_FN(CFTypeRef, SecIdentityCopyPreferred, (CFStringRef name, CFArrayRef usage, CFArrayRef issuers)) { return NULL; }
HK_FN(OSStatus, SecTrustSettingsCopyCertificates, (uint32_t domain, CFArrayRef *certificates)) {
    SEC_OUT(certificates);
    return kHKErrSecNoTrustSettings;
}
HK_FN(OSStatus, SecTrustSettingsCopyTrustSettings, (CFTypeRef certificate, uint32_t domain, CFArrayRef *settings)) {
    SEC_OUT(settings);
    return kHKErrSecNoTrustSettings;
}
/* Code-signing requirements (macOS's requirement language, which iOS's Security cannot parse): a requirement is made
   (its text kept, as a CF string) and every check against one passes; iOS's sandbox is the app's real boundary.
   Chromium builds one from its own signature at startup and stops if it gets none. */
HK_FN(OSStatus, SecRequirementCreateWithString, (CFStringRef text, uint32_t flags, CFTypeRef *requirement)) {
    if (requirement) *requirement = text ? CFStringCreateCopy(NULL, text) : CFSTR("");
    return 0;
}
HK_FN(OSStatus, SecRequirementCreateWithStringAndErrors, (CFStringRef text, uint32_t flags, CFErrorRef *errors,
                                                         CFTypeRef *requirement)) {
    if (errors) *errors = NULL;
    if (requirement) *requirement = text ? CFStringCreateCopy(NULL, text) : CFSTR("");
    return 0;
}
HK_FN(OSStatus, SecRequirementCopyString, (CFTypeRef requirement, uint32_t flags, CFStringRef *text)) {
    if (text) *text = requirement && CFGetTypeID(requirement) == CFStringGetTypeID() ? CFStringCreateCopy(NULL, requirement) : CFSTR("");
    return 0;
}
HK_FN(OSStatus, SecCodeCheckValidity, (CFTypeRef code, uint32_t flags, CFTypeRef requirement)) { return 0; }
HK_FN(OSStatus, SecCodeCheckValidityWithErrors, (CFTypeRef code, uint32_t flags, CFTypeRef requirement, CFErrorRef *errors)) {
    if (errors) *errors = NULL;
    return 0;
}
HK_FN(OSStatus, SecStaticCodeCheckValidity, (CFTypeRef code, uint32_t flags, CFTypeRef requirement)) { return 0; }
HK_FN(OSStatus, SecStaticCodeCheckValidityWithErrors, (CFTypeRef code, uint32_t flags, CFTypeRef requirement, CFErrorRef *errors)) {
    if (errors) *errors = NULL;
    return 0;
}

HK_FN(OSStatus, SecCodeCopyGuestWithAttributes, (CFTypeRef host, CFDictionaryRef attributes, uint32_t flags, CFTypeRef *guest)) {
    SEC_OUT(guest);
    return kHKErrSecUnimplemented;
}
HK_FN(OSStatus, AuthorizationCreate, (const void *rights, const void *environment, uint32_t flags, void **authorization)) {
    SEC_OUT(authorization);
    return kHKErrAuthorizationDenied;
}
HK_FN(OSStatus, AuthorizationCopyRights, (void *authorization, const void *rights, const void *environment, uint32_t flags,
                                          void **authorized)) {
    SEC_OUT(authorized);
    return kHKErrAuthorizationDenied;
}
HK_FN(OSStatus, AuthorizationExecuteWithPrivileges, (void *authorization, const char *tool, uint32_t options, char *const *arguments,
                                                     FILE **pipe)) {
    SEC_OUT(pipe);
    return kHKErrAuthorizationDenied;
}
HK_FN(OSStatus, AuthorizationFree, (void *authorization, uint32_t flags)) { return 0; }
HK_FN(OSStatus, AuthorizationRightGet, (const char *right, CFDictionaryRef *definition)) {
    SEC_OUT(definition);
    return kHKErrAuthorizationDenied;
}
HK_FN(OSStatus, AuthorizationRightSet, (void *authorization, const char *right, CFTypeRef definition, CFStringRef description,
                                        CFBundleRef bundle, CFStringRef table)) {
    return kHKErrAuthorizationDenied;
}
#undef SEC_OUT

/* ================================================================ SystemConfiguration: no dynamic store */

/* The dynamic store: a quiet one that holds nothing and never reports a change. Failing makes Chromium retry it
   every second for good. */
HK_FN(void *, SCDynamicStoreCreate, (CFAllocatorRef allocator, CFStringRef name, void *callout, void *context)) {
    return (void *)CFDataCreate(allocator, (const UInt8 *)"hk", 2);
}
HK_FN(CFPropertyListRef, SCDynamicStoreCopyValue, (void *store, CFStringRef key)) { return NULL; }
static void hk_quiet_source(void *info) {}
HK_FN(CFRunLoopSourceRef, SCDynamicStoreCreateRunLoopSource, (CFAllocatorRef allocator, void *store, CFIndex order)) {
    CFRunLoopSourceContext ctx = {0};
    ctx.perform = hk_quiet_source;
    return CFRunLoopSourceCreate(allocator, order, &ctx);
}
HK_FN(Boolean, SCDynamicStoreSetNotificationKeys, (void *store, CFArrayRef keys, CFArrayRef patterns)) { return store != NULL; }
HK_FN(int, SCError, (void)) { return 0; }

/* ================================================================ CoreServices: Apple events, LaunchServices, Spotlight */

typedef struct {
    uint32_t descriptorType;
    void *dataHandle;
} HKAEDesc;

static void null_desc(HKAEDesc *d) {
    if (d) d->descriptorType = 'null', d->dataHandle = NULL;
}
HK_FN(OSErr, AECreateAppleEvent, (uint32_t eventClass, uint32_t eventID, const HKAEDesc *target, int16_t returnID,
                                  int32_t transactionID, HKAEDesc *result)) {
    null_desc(result);
    return kHKErrAEEventNotHandled;
}
HK_FN(OSErr, AECreateDesc, (uint32_t type, const void *data, long size, HKAEDesc *result)) {
    null_desc(result);
    return kHKErrAEEventNotHandled;
}
HK_FN(OSErr, AEDisposeDesc, (HKAEDesc *desc)) {
    null_desc(desc);
    return 0;
}
HK_FN(OSErr, AEGetParamDesc, (const HKAEDesc *event, uint32_t keyword, uint32_t type, HKAEDesc *result)) {
    null_desc(result);
    return kHKErrAEEventNotHandled;
}
HK_FN(OSErr, AEPutParamDesc, (HKAEDesc *event, uint32_t keyword, const HKAEDesc *desc)) { return kHKErrAEEventNotHandled; }
HK_FN(OSErr, AEInstallEventHandler, (uint32_t eventClass, uint32_t eventID, void *handler, long refcon, Boolean system)) {
    return kHKErrAEEventNotHandled;
}
HK_FN(OSErr, AERemoveEventHandler, (uint32_t eventClass, uint32_t eventID, void *handler, Boolean system)) {
    return kHKErrAEEventNotHandled;
}
HK_FN(OSStatus, AESendMessage, (const HKAEDesc *event, HKAEDesc *reply, int32_t mode, long timeout)) {
    null_desc(reply);
    return kHKErrAEEventNotHandled;
}

HK_FN(OSStatus, LSRegisterURL, (CFURLRef url, Boolean update)) { return kHKLSUnknownErr; }
HK_FN(void *, LSSharedFileListCreate, (CFAllocatorRef allocator, CFStringRef type, CFTypeRef options)) { return NULL; }
HK_FN(CFArrayRef, LSSharedFileListCopySnapshot, (void *list, uint32_t *seed)) {
    if (seed) *seed = 0;
    return NULL;
}
HK_FN(void *, LSSharedFileListInsertItemURL, (void *list, void *after, CFStringRef name, void *icon, CFURLRef url,
                                              CFDictionaryRef set, CFArrayRef clear)) {
    return NULL;
}
HK_FN(CFTypeRef, LSSharedFileListItemCopyProperty, (void *item, CFStringRef name)) { return NULL; }
HK_FN(CFURLRef, LSSharedFileListItemCopyResolvedURL, (void *item, uint32_t flags, CFErrorRef *error)) {
    if (error) *error = NULL;
    return NULL;
}
HK_FN(OSStatus, LSSharedFileListItemRemove, (void *list, void *item)) { return kHKLSUnknownErr; }
HK_FN(void, _LSSetApplicationLaunchServicesServerConnectionStatus, (uint64_t flags, id block)) {}
HK_FN(void, _CSCheckFixDisable, (void)) {}
HK_FN(CFTypeRef, MDItemCreate, (CFAllocatorRef allocator, CFStringRef path)) { return NULL; }
HK_FN(Boolean, MDItemSetAttribute, (CFTypeRef item, CFStringRef name, CFTypeRef value)) { return false; }

HK_CFSTRING(kLSQuarantineDataURLKey, "LSQuarantineDataURL");
HK_CFSTRING(kLSQuarantineOriginURLKey, "LSQuarantineOriginURL");
HK_CFSTRING(kLSQuarantineTypeKey, "LSQuarantineType");
HK_CFSTRING(kLSQuarantineTypeOtherDownload, "LSQuarantineTypeOtherDownload");
HK_CFSTRING(kLSQuarantineTypeWebDownload, "LSQuarantineTypeWebDownload");
HK_CFSTRING(kLSSharedFileListLoginItemHidden, "com.apple.loginitem.HideOnLaunch");
HK_CFSTRING(kLSSharedFileListSessionLoginItems, "com.apple.LSSharedFileList.SessionLoginItems");
HK_CFSTRING(_kLSOpenOptionBackgroundLaunchKey, "_kLSOpenOptionBackgroundLaunchKey");
HK_CFSTRING(kMDItemWhereFroms, "kMDItemWhereFroms");
/* A list position, not a string: any unique non-null reference does. */
CFTypeRef hk_kLSSharedFileListItemLast __asm__("_kLSSharedFileListItemLast") = CFSTR("kLSSharedFileListItemLast");

HK_NSSTRING(NSURLQuarantinePropertiesKey, "NSURLQuarantinePropertiesKey");
HK_NSSTRING(NSURLTagNamesKey, "NSURLTagNamesKey");
