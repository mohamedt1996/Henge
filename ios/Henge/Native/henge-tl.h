/* Mac apps without macOS (experimental): the translation layer (../../../tl) as the Swift side sees it. */
#ifndef HENGE_TL_H
#define HENGE_TL_H

#include <stddef.h>
#include <stdint.h>

/* Copies what a .dmg holds at its top level into dir: every .app folder, and every lone Mach-O program. Returns how
   many it copied (0 if the image holds neither), or -1 with a reason in err. */
int henge_tl_import_dmg(const char *dmg, const char *dir, char *err, size_t errlen);

/* What an app or program is. kind: what its code needs, worst first (an app linking AppKit is HENGE_TL_APPKIT even
   if it also links Foundation). 0, or -1 with a reason in err. */
enum HengeTlKind {
    HENGE_TL_PROGRAM,   /* a command-line program: libSystem, Foundation, Swift and the like, all on iOS */
    HENGE_TL_SWIFTUI,   /* a SwiftUI app without AppKit's own calls */
    HENGE_TL_CATALYST,  /* a Mac Catalyst app (UIKit) */
    HENGE_TL_APPKIT,    /* an AppKit app: needs an AppKit, which iOS lacks */
    HENGE_TL_INTEL,     /* no arm64 code at all */
};
typedef struct HengeTlInfo {
    char name[256], identifier[256], min_macos[32];
    char executable[1024]; /* the Mach-O to run */
    int arm64, x86_64, kind;
    int libraries;         /* how many libraries it links */
} HengeTlInfo;
int henge_tl_info(const char *path, HengeTlInfo *info, char *err, size_t errlen);

/* Runs a program's main on a thread of its own, in this process, through Henge's own loader, its standard output
   and error going to the file out_path. Needs JIT on, and no Mac started in this launch (the loader takes the far
   end of the JIT region and remaps part of it). Returns main's result (or the code it passed to exit), or -1 with a
   reason in err if it could not be loaded. Blocks until it returns. */
int henge_tl_run(const char *path, const char *out_path, char *err, size_t errlen);
/* A Mac app ran in this launch: no Mac can start, and no other program run, until Henge is opened again. */
int henge_tl_used(void);
int henge_tl_ran_program(void);

/* ---- Mac apps with windows (experimental; henge-tl-app.m): an AppKit app runs on the main thread against Henge's
   AppKit, one per launch, and no Mac can start after it. ---- */
/* The screen the app sees, in points, and its scale. Before henge_tl_app_load. */
void henge_tl_app_set_screen(double width, double height, double scale);
/* Called on the main thread whenever a window appears, goes, or changes. */
void henge_tl_app_on_change(void (*callback)(void));
/* Loads the app (any thread; it takes a while for a big one). 0, or -1 with a reason in err. */
int henge_tl_app_load(const char *app_path, char *err, size_t errlen);
/* Runs the app's main on the calling thread, which must be the main thread; returns when the app quits. */
int henge_tl_app_run(void);
/* The same, entered from a run-loop timer on the main thread shortly (see henge-tl-app.m); finished gets main's result. */
void henge_tl_app_run_soon(void (*finished)(int));
/* The app's windows on screen, back to front: count, the layer to show (a CALayer *), and the frame in screen points
   (origin at the bottom left, as the app sees it). */
int henge_tl_app_window_count(void);
void *henge_tl_app_window_layer(int index);
int henge_tl_app_window_frame(int index, double *x, double *y, double *w, double *h);
double henge_tl_app_screen_height(void);
/* Input in screen points, origin at the bottom left. type is AppKit's NSEventType (1 left down, 2 left up, 5 moved,
   6 left dragged, 3/4 right down/up); phase is NSEventPhase; flags AppKit's modifier flags. */
void henge_tl_app_mouse(int type, double x, double y, int clicks, unsigned long flags);
void henge_tl_app_scroll(double x, double y, double dx, double dy, int phase);
void henge_tl_app_key(int down, unsigned short keycode, const char *chars, unsigned long flags);
/* The app's menu bar menus, for the screen's menu button (a Mac app's menu bar has no place on iOS): a retained
   NSArray of items, each a dictionary with "title", "enabled", "state" (1 on, -1 mixed), "separator", "path" (the
   item's indices from the menu bar down, for henge_tl_app_menu_choose) and, for a submenu, "children". The menus are
   brought up to date (enabled items, menus an app fills as they open) as AppKit does before showing them. NULL
   without a menu bar. Main thread. */
const void *henge_tl_app_main_menu(void);
/* Chooses the item at a path from henge_tl_app_main_menu: its action is sent, as AppKit does. Main thread. */
void henge_tl_app_menu_choose(const int *path, int depth);

#endif
