/* NSTask for a Mac app inside Henge. iOS starts no processes, and a Mac test run must not start real ones either
   (Foundation's own NSTask spawns past the loader's hooks), so the app's tasks go through the loader
   (tl_task_launch): a system tool answered there (sw_vers, uname) writes its output to the task's standard output
   and ends at once; a program of the app's own starts as a process inside Henge; anything else ends at once with
   status 127, as a missing program would, and launchAndReturnError: says so. Bound in place of Foundation's NSTask
   (HKLookup.m maps NSTask's class symbols to HKTask). */
#import "HKShim.h"
#include <unistd.h>

pid_t tl_task_launch(const char *path, char *const argv[], int out_fd, const void *caller); /* tl/loader/tl_loader.c */
int tl_task_status(pid_t pid, int *status);

@interface HKTask : NSObject
@property(nonatomic, copy) NSString *launchPath;
@property(nonatomic, copy) NSURL *executableURL;
@property(copy) NSArray<NSString *> *arguments;
@property(copy) NSDictionary<NSString *, NSString *> *environment;
@property(copy) NSString *currentDirectoryPath;
@property(copy) NSURL *currentDirectoryURL;
@property(strong) id standardInput, standardOutput, standardError;
@property(copy) void (^terminationHandler)(id task);
@property NSQualityOfService qualityOfService;
@end

@implementation HKTask {
    pid_t _pid;
    int _status;
    BOOL _launched, _ended;
}

+ (instancetype)launchedTaskWithLaunchPath:(NSString *)path arguments:(NSArray<NSString *> *)arguments {
    HKTask *t = [self new];
    t.launchPath = path;
    t.arguments = arguments;
    [t launch];
    return t;
}

+ (instancetype)launchedTaskWithExecutableURL:(NSURL *)url arguments:(NSArray<NSString *> *)arguments error:(NSError **)error
                           terminationHandler:(void (^)(id))handler {
    HKTask *t = [self new];
    t.executableURL = url;
    t.arguments = arguments;
    t.terminationHandler = handler;
    return [t launchAndReturnError:error] ? t : nil;
}

- (NSURL *)executableURL { return _executableURL ?: (_launchPath ? [NSURL fileURLWithPath:_launchPath] : nil); }
- (NSString *)launchPath { return _launchPath ?: _executableURL.path; }

static int fd_of(id stream) {
    if ([stream respondsToSelector:@selector(fileHandleForWriting)]) stream = [stream fileHandleForWriting];
    return [stream respondsToSelector:@selector(fileDescriptor)] ? [stream fileDescriptor] : -1;
}

- (void)hk_end:(int)status {
    _status = status;
    _ended = YES;
    /* The parent's copy of the pipe's write end closes as the child's would at exit, so readers see the end. */
    if ([_standardOutput respondsToSelector:@selector(fileHandleForWriting)]) [[_standardOutput fileHandleForWriting] closeFile];
    void (^handler)(id) = _terminationHandler;
    if (handler) dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ handler(self); });
    [[NSNotificationCenter defaultCenter] postNotificationName:@"NSTaskDidTerminateNotification" object:self];
}

- (BOOL)launchAndReturnError:(NSError **)error {
    if (_launched) return YES;
    _launched = YES;
    NSString *path = self.launchPath;
    NSMutableArray *args = [NSMutableArray arrayWithObject:path ?: @""];
    if (_arguments) [args addObjectsFromArray:_arguments];
    char **argv = calloc(args.count + 1, sizeof *argv);
    for (NSUInteger i = 0; i < args.count; i++) argv[i] = (char *)[args[i] UTF8String];
    pid_t pid = tl_task_launch(path.fileSystemRepresentation, argv, fd_of(_standardOutput), __builtin_return_address(0));
    free(argv);
    if (pid > 0) {
        _pid = pid;
        return YES;
    }
    [self hk_end:pid == 0 ? 0 : 127];
    if (pid < 0 && error)
        *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadNoPermissionError
                                 userInfo:@{NSLocalizedDescriptionKey : @"Henge runs no other programs (as on iOS)"}];
    return pid >= 0;
}

- (void)launch { [self launchAndReturnError:NULL]; }

- (BOOL)isRunning {
    if (!_launched || _ended) return NO;
    int status = 0;
    if (_pid > 0 && tl_task_status(_pid, &status)) [self hk_end:status];
    return !_ended;
}

- (void)waitUntilExit {
    while ([self isRunning]) usleep(10000);
}

- (int)terminationStatus { return _status; }
- (NSInteger)terminationReason { return 1; /* NSTaskTerminationReasonExit */ }
- (int)processIdentifier { return _pid; }
- (void)terminate {}
- (void)interrupt {}
- (BOOL)suspend { return NO; }
- (BOOL)resume { return NO; }
@end
