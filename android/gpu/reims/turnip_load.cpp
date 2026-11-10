// SPDX-License-Identifier: GPL-2.0-or-later
// Graphics: reims, Vulkan driver: Turnip. Opens the app's own Vulkan driver (libvulkan_freedreno.so,
// Mesa's Turnip built for Android's KGSL) the way Android opens a vendor GPU driver: in a child of the
// "sphal" linker namespace, which can see the platform libraries the driver needs (libhardware.so,
// libsync.so, libnativewindow.so) that an app's own namespace is refused. The hidden linker calls come
// from linkernsbypass (Billy Laws, BSD-2-Clause; the loader libadrenotools builds on, as Dolphin, Yuzu's
// forks and Winlator load Turnip). Answers the driver's vk_icdGetInstanceProcAddr, which metal2vulkan
// takes as its Vulkan entry point (M2V_VULKAN_GIPA, Henge's port).
#include <dlfcn.h>
#include <string.h>
#include <string>

#include "linkernsbypass/android_linker_ns.h"
#include "turnip_load.h"

static char turnip_error[512];

const char *reims_turnip_error(void) { return turnip_error; }

void *reims_turnip_open(const char *dir, const char *name) {
    turnip_error[0] = 0;
    if (!linkernsbypass_load_status()) {
        snprintf(turnip_error, sizeof turnip_error, "the linker's namespace calls were not found (Android < 9?)");
        return nullptr;
    }
    // The namespace Android's own Vulkan loader opens vendor drivers in: "sphal" on every device since
    // Android 8 with a vendor partition; "vendor" and "default" as adrenotools' fallbacks.
    android_namespace_t *parent = nullptr;
    const char *parent_name = nullptr;
    for (const char *n : {"sphal", "vendor", "default"}) {
        parent = android_get_exported_namespace(n);
        if (parent) { parent_name = n; break; }
    }
    if (!parent) {
        snprintf(turnip_error, sizeof turnip_error, "no sphal, vendor or default linker namespace is exported");
        return nullptr;
    }
    // A shared child of it that also searches the app's library directory, as adrenotools' hook makes
    // for the custom driver, and that can see libandroid (not in the vendor side).
    android_namespace_t *ns = android_create_namespace("henge-turnip", dir, dir, ANDROID_NAMESPACE_TYPE_SHARED, nullptr, parent);
    if (!ns) {
        snprintf(turnip_error, sizeof turnip_error, "could not create a linker namespace under %s", parent_name);
        return nullptr;
    }
    android_link_namespaces(ns, nullptr, "libandroid.so:liblog.so:libz.so:libm.so:libdl.so:libc.so");
    std::string path = std::string(dir) + "/" + name;
    void *handle = linkernsbypass_namespace_dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL, ns);
    if (!handle) {
        const char *why = dlerror();
        snprintf(turnip_error, sizeof turnip_error, "dlopen in a child of %s: %s", parent_name, why ? why : "?");
        return nullptr;
    }
    void *gipa = dlsym(handle, "vk_icdGetInstanceProcAddr");
    if (!gipa) gipa = dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) {
        snprintf(turnip_error, sizeof turnip_error, "%s exports no vk_icdGetInstanceProcAddr", name);
        return nullptr;
    }
    snprintf(turnip_error, sizeof turnip_error, "opened in a child of the %s namespace", parent_name);
    return gipa;
}
