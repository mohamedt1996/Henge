// SPDX-License-Identifier: GPL-2.0-or-later
// Graphics: reims, Vulkan driver: Turnip. Opens the app's own Vulkan driver (libvulkan_freedreno.so,
// Mesa's Turnip built for Android's KGSL) the way Android opens a vendor GPU driver: in a child of the
// "sphal" linker namespace, which can see the platform libraries the driver needs (libhardware.so,
// libsync.so, libnativewindow.so) that an app's own namespace is refused. The hidden linker calls come
// from linkernsbypass (Billy Laws, BSD-2-Clause; the loader libadrenotools builds on, as Dolphin, Yuzu's
// forks and Winlator load Turnip). Answers a vkGetInstanceProcAddr for the driver, which metal2vulkan
// takes as its Vulkan entry point (M2V_VULKAN_GIPA, Henge's port): the driver's vk_icdGetInstanceProcAddr
// where it exports one, else (Mesa's Android builds export only HMI) one over its hardware module.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "linkernsbypass/android_linker_ns.h"
#include "turnip_load.h"

static char turnip_error[512];

// <hardware/hardware.h> and <hardware/hwvulkan.h>, as far as this needs them (LP64).
namespace {
struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
    int (*open)(const hw_module_t *module, const char *id, hw_device_t **device);
};
struct hw_module_t {
    uint32_t tag; // 'HWMT'
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag; // 'HWDT'
    uint32_t version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(hw_device_t *device);
};
typedef void (*VoidFunction)(void);
typedef VoidFunction (*GetInstanceProcAddr)(void *instance, const char *name);
struct hwvulkan_device_t {
    hw_device_t common;
    VoidFunction EnumerateInstanceExtensionProperties;
    VoidFunction CreateInstance;
    GetInstanceProcAddr GetInstanceProcAddr_;
};
static_assert(sizeof(hw_module_t) == 248, "hw_module_t, LP64");
static_assert(sizeof(hw_device_t) == 120, "hw_device_t, LP64");
constexpr uint32_t kModuleTag = 'H' << 24 | 'W' << 16 | 'M' << 8 | 'T';
constexpr uint32_t kDeviceTag = 'H' << 24 | 'W' << 16 | 'D' << 8 | 'T';

hwvulkan_device_t *hal_device;

// No layers: the answer a loader gives where there are none (VK_SUCCESS).
int no_layers(uint32_t *count, void *properties) {
    (void)properties;
    if (count) *count = 0;
    return 0;
}

// vkGetInstanceProcAddr over the module's device: the loader's global functions for a null instance
// (the device's own two, and the rest from the driver where it answers them), the driver's for a real one.
VoidFunction hal_gipa(void *instance, const char *name) {
    if (!instance && name) {
        if (!strcmp(name, "vkCreateInstance")) return hal_device->CreateInstance;
        if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return hal_device->EnumerateInstanceExtensionProperties;
        if (!strcmp(name, "vkGetInstanceProcAddr")) return (VoidFunction)hal_gipa;
        VoidFunction f = hal_device->GetInstanceProcAddr_(nullptr, name);
        if (!f && !strcmp(name, "vkEnumerateInstanceLayerProperties")) return (VoidFunction)no_layers;
        return f;
    }
    return hal_device->GetInstanceProcAddr_(instance, name);
}

void *hal_entry(void *handle, const char *name) {
    auto *module = static_cast<hw_module_t *>(dlsym(handle, "HMI"));
    if (!module) {
        snprintf(turnip_error, sizeof turnip_error, "%s exports neither vk_icdGetInstanceProcAddr nor HMI", name);
        return nullptr;
    }
    if (module->tag != kModuleTag || !module->methods || !module->methods->open) {
        snprintf(turnip_error, sizeof turnip_error, "%s: HMI is not a hardware module (tag %#x)", name, module->tag);
        return nullptr;
    }
    hw_device_t *device = nullptr;
    int rc = module->methods->open(module, "vk0", &device);
    if (rc || !device) {
        snprintf(turnip_error, sizeof turnip_error, "%s: the module's vk0 device did not open (%d)", name, rc);
        return nullptr;
    }
    auto *vk = reinterpret_cast<hwvulkan_device_t *>(device);
    if (device->tag != kDeviceTag || !vk->CreateInstance || !vk->EnumerateInstanceExtensionProperties || !vk->GetInstanceProcAddr_) {
        snprintf(turnip_error, sizeof turnip_error, "%s: vk0 is not a Vulkan device (tag %#x)", name, device->tag);
        return nullptr;
    }
    hal_device = vk; // kept for the rest of the process, as the library is
    return reinterpret_cast<void *>(hal_gipa);
}
} // namespace

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
    const char *how = "an ICD";
    if (!gipa) {
        // Mesa built for Android exports one symbol, HMI: the driver as a hardware module ("vulkan"), as
        // Android's own Vulkan loader opens it. Its device ("vk0") carries the three entry points.
        gipa = hal_entry(handle, name);
        how = "Android's hardware module";
        if (!gipa) return nullptr; // hal_entry said why
    }
    snprintf(turnip_error, sizeof turnip_error, "opened in a child of the %s namespace, as %s", parent_name, how);
    return gipa;
}
