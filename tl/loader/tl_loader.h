#ifndef HENGE_TL_LOADER_H
#define HENGE_TL_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Henge's Mach-O loader for the Mac-app translation layer: loads an arm64
   macOS executable and the frameworks inside its bundle without dyld, binds
   their imports to the host's own libraries (iOS's, or a shim), registers
   them with the Objective-C and Swift runtimes, and runs them in-process.

   Why not dyld: on a TXM device (every device iOS 27 supports) an app cannot
   make new pages executable; Henge gets one executable region per launch
   from a debugger. Code goes into that region, written through its writable
   alias; every other segment is ordinary memory placed at the same distance
   from the code as in the file, so PC-relative references still land. */

typedef struct TlImage TlImage;

typedef struct TlHost {
    /* The executable region: code runs at rx, and is written at rx + rw_diff. */
    uint8_t *rx;
    intptr_t rw_diff;
    size_t size, used;
    /* A macOS install name to the path dlopen should use on this host, or NULL
       when the host has no such library (then only shim_lookup can answer). */
    const char *(*map_install_name)(const char *install_name, char *buf, size_t len);
    /* A symbol the host provides itself for a library (AppKit on iOS, say), or
       NULL. Asked before the real library, so a shim can replace it. */
    void *(*shim_lookup)(const char *install_name, const char *symbol);
    /* Messages ("tl: ..."); NULL writes them to stderr. */
    void (*log)(const char *line);
    /* A missing non-weak symbol fails the load (1) or binds to a stub that
       reports it if it is ever called (0). */
    int strict;
    int verbose;
    /* A missing function returns 0 when called, logged once, instead of stopping the program; a missing class or
       constant gets a stand-in (a class answering nil to everything, a string named after the constant). For
       bringing up big apps, where most of what is missing is never needed. */
    int soft_missing;
    /* The program may not start other processes (fork, posix_spawn, exec fail with EPERM), as on iOS. */
    int no_processes;
    /* 1 when the host library must not answer this symbol although it has it (a Mac test runner refusing what
       iOS lacks); install_name is "" for lookups without one. Asked after shim_lookup. */
    int (*host_lacks)(const char *install_name, const char *symbol);
    /* iOS's kernel rules for the program: no new executable memory (mmap or mprotect with PROT_EXEC fail), no Mach
       service registration (bootstrap_check_in/register refused). For Mac runners shaped like iOS. */
    int ios_kernel_rules;
    /* Processes as threads: a program starting another program of its own (posix_spawn, exec of a Mach-O that is not
       a system tool) gets it loaded into this process, in a namespace of its own (its own copy of every library it
       loads, so its own globals), its main run on a thread; pids, parent pids, waiting, killing, exit and the Mach
       bootstrap names programs meet each other by are answered to match. What iOS needs for apps built from
       several processes (Chrome's renderers, Steam's web helper). */
    int processes_as_threads;
    /* Called when such a process starts, with its namespace and executable (the host adopts its bundle). */
    void (*process_started)(int ns, const char *path);
    /* Address space the host holds for the program's large reservations (4 GB or more, no access yet: Chromium's
       allocators), where the system has too little room for them to find on their own (iOS: 64 GB in all). The
       loader places them in it, each at the alignment its size implies; NULL if none. */
    void *high_room[4];
    size_t high_room_bytes[4];
    int high_rooms;
    /* Called for each image once it is fixed up, before its initializers run (app-specific adjustments). */
    void (*image_linked)(TlImage *image);
} TlHost;

/* A region shaped like iOS's (an RX mapping plus an RW alias of the same
   pages) made on this Mac, for tests. 0, or -1. */
int tl_host_init_split(TlHost *host, size_t bytes);
/* Use an executable region the host already has (iOS: Henge's JIT region). */
void tl_host_init_region(TlHost *host, void *rx, void *rw, size_t bytes);

/* Loads the executable at path (thin or universal; the arm64 slice), its
   bundle's frameworks, fixes them up, registers them, and runs their
   initializers. NULL with a reason in err. */
TlImage *tl_load(TlHost *host, const char *path, char *err, size_t errlen);
/* Make NSBundle.mainBundle the guest app's bundle (the folder holding Contents/MacOS/<executable>) for the
   guest's own code; the host (and the system acting for it) keeps its own. 0, or -1. */
int tl_adopt_main_bundle(const char *app_path);
/* The adopted bundle for every caller, not just the app's code: for Mac test runners, where macOS's UIFoundation
   picks its AppKit-style text code by the main bundle (iOS's is UIKit's whatever the bundle). Before adopting. */
void tl_main_bundle_for_everyone(void);
/* A process started inside the app (namespace ns) runs as the app at app_path (its main bundle for its own code). */
void tl_adopt_ns_bundle(int ns, const char *app_path);
/* The adopted bundle (an NSBundle *), or NULL: for host code acting for the app (Henge's AppKit). */
void *tl_main_bundle(void);
/* Calls the executable's main. */
int tl_run_main(TlImage *exe, int argc, const char **argv, const char **envp);

/* The libSystem calls through which a program finds and loads code while it runs (dlopen, dlsym, dladdr, dlerror,
   _NSGetExecutablePath, the _dyld_* list of images) answered by this loader, so code the program loads itself is
   loaded here, never by dyld; and, with no_processes, fork/posix_spawn/exec refused. A host's shim_lookup asks this
   first for libSystem's symbols ("_dlopen"); NULL when it is not one of them. */
void *tl_runtime_lookup(const char *symbol);
/* Loads a library (and what it links) as dlopen does for the program. NULL with a reason in err. */
TlImage *tl_load_library(const char *path, char *err, size_t errlen);
/* While an image's imports are being bound: whether it links AppKit's classes directly (_OBJC_CLASS_$_NS... from
   AppKit), as Objective-C apps do, rather than finding them by name, as Rust's objc2 does. Hosts route the
   program's objc_getClass through their AppKit only for the second kind (Chromium breaks when its objc_getClass is
   not libobjc's own). */
int tl_binding_links_appkit_classes(void);
/* The namespace of the code at addr (0: the program, or not loaded here). */
int tl_ns_of_address(const void *addr);
/* The guest image holding addr, or NULL. */
TlImage *tl_image_containing(const void *addr);

/* What was loaded, for the host and tests. */
void *tl_image_base(const TlImage *image);
size_t tl_image_size(const TlImage *image);
const char *tl_image_path(const TlImage *image);
/* The address of an exported symbol ("_main"), or NULL. */
void *tl_image_symbol(TlImage *image, const char *symbol);
/* Symbols that were missing and bound to the reporting stub. */
int tl_missing_count(void);
const char *tl_missing_name(int index);

/* The usual TlHost.map_install_name on iOS: a macOS framework path to iOS's
   (no Versions/X; Mac Catalyst's /System/iOSSupport copies to the originals),
   or NULL for libraries iOS does not have (AppKit, Carbon and the like), which
   the host then answers with shims. */
const char *tl_ios_map_install_name(const char *install_name, char *buf, size_t len);

#endif
