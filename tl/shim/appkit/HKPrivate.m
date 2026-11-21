/* AppKit's private classes that apps check for by name (NSClassFromString) and then swizzle or call: present, with
   the methods they look for, doing nothing. */
#import "HKShim.h"

/* Chromium's Services-menu filter (chrome_swizzle_services_menu_updater.mm) stops the browser unless these exist
   and answer these selectors, with the type encodings its own replacements have. */
@interface _NSServiceEntry : HKShimObject
@property(copy) NSString *bundleIdentifier;
@end
@implementation _NSServiceEntry
@end

@interface _NSServicesMenuUpdater : HKShimObject
- (void)populateMenu:(id)menu withServiceEntries:(NSArray *)entries forDisplay:(BOOL)display;
- (void)plugMenu:(id)menu intoMenu:(id)parent;
@end
@implementation _NSServicesMenuUpdater
- (void)populateMenu:(id)menu withServiceEntries:(NSArray *)entries forDisplay:(BOOL)display {}
- (void)plugMenu:(id)menu intoMenu:(id)parent {}
@end

/* AppKit's nib loading (NSBundle's NSNibLoading category): apps load their menu bar from a nib. No nibs are read
   yet: loading succeeds with nothing in it, so the app goes on without that menu. */
@implementation NSBundle (HKNibLoading)
- (BOOL)loadNibNamed:(NSString *)name owner:(id)owner topLevelObjects:(NSArray **)objects {
    hk_log("nib %s not loaded (nibs are not read yet)", name.UTF8String);
    if (objects) *objects = @[];
    return YES;
}
+ (BOOL)loadNibNamed:(NSString *)name owner:(id)owner {
    return [hk_app_bundle() loadNibNamed:name owner:owner topLevelObjects:NULL];
}
@end

/* MediaPlayer's remote-command and now-playing centers, for when the process has no MediaPlayer (the Mac test
   runners refuse it, since on a Mac it loads the real AppKit): a command takes targets and hands back a token, as
   Spotify expects. On iOS the real MediaPlayer is loaded and these are never used (hk_lookup). */
@interface HKRemoteCommand : NSObject
@property(nonatomic, getter=isEnabled) BOOL enabled;
@end
@implementation HKRemoteCommand
- (id)addTargetWithHandler:(id)handler { return [NSObject new]; }
- (void)addTarget:(id)target action:(SEL)action {}
- (void)removeTarget:(id)target {}
- (void)removeTarget:(id)target action:(SEL)action {}
+ (void)load { hk_add_fallback(self); }
@end

@interface HKRemoteCommandCenter : NSObject
@end
@implementation HKRemoteCommandCenter
+ (instancetype)sharedCommandCenter {
    static HKRemoteCommandCenter *c;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ c = [self new]; });
    return c;
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    return [super methodSignatureForSelector:sel] ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}
/* playCommand, pauseCommand, nextTrackCommand...: one working command each */
- (void)forwardInvocation:(NSInvocation *)inv {
    static NSMutableDictionary *commands;
    if (!commands) commands = [NSMutableDictionary new];
    NSString *name = NSStringFromSelector(inv.selector);
    id result = nil;
    if ([name hasSuffix:@"Command"]) result = commands[name] ?: (commands[name] = [HKRemoteCommand new]);
    if (inv.methodSignature.methodReturnLength == sizeof(id)) [inv setReturnValue:&result];
}
@end

@interface HKNowPlayingInfoCenter : NSObject
@property(copy) NSDictionary *nowPlayingInfo;
@property NSUInteger playbackState;
@end
@implementation HKNowPlayingInfoCenter
+ (instancetype)defaultCenter {
    static HKNowPlayingInfoCenter *c;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ c = [self new]; });
    return c;
}
+ (void)load { hk_add_fallback(self); }
@end
