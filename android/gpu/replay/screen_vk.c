/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Mac's screen on Vulkan. The frame's pixels lie in a buffer of the screen's own, which whoever draws the
   frame writes directly (screen_frame), and each present copies them into the swapchain.

   Where the window is at least as large as the frame, the swapchain is made at the frame's own scale: the
   frame's pixels one to one, with black bars to the window's shape, and Android's compositor scales it to the
   window (the display's scaler, where the hardware takes the layer). A present is then one copy of the frame
   on the GPU. Before 0.3.53 the swapchain was the window's size (3200x2136 on the Mi Pad 7), cleared whole and
   filled by a scaling blit at every frame: some 50 MB written on the GPU for a 4 MB frame, and three or four
   window-sized buffers (80 to 110 MB) held for it.

   Elsewhere (a window smaller than the frame, a rotated surface, a swapchain that is not RGBA8) the frame is
   uploaded to an image and scaled into a window-sized swapchain with a blit, letterboxed, as before. With no
   window (tests) it draws into an off-screen image, which takes either form. */
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android/native_window.h>
#endif
#include <vulkan/vulkan.h>

#include "replay.h"

#include <stdlib.h>
#include <string.h>

#define OFFSCREEN_W 1280
#define OFFSCREEN_H 800
#define MAX_IMAGES 8

struct Screen {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkSemaphore acquired, rendered;
    /* the window */
    void *window;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;          /* the swapchain's (or the off-screen image's) */
    VkExtent2D window_extent;   /* the window's */
    int direct;                 /* the swapchain is at the frame's scale: presents copy, nothing scales */
    int no_direct;              /* never: the direct form misbehaved here, or HENGE_SCREEN_DIRECT=0 */
    int sharp;                  /* HENGE_SCREEN_SHARP=1: a whole-number blit scale takes each pixel as it is */
    int sharp_now;              /* the last blit was such a one */
    const char *off_by;         /* the setting that turned the direct form off, where one did */
    const char *why_not;        /* why the swapchain is the window's size, where it is */
    uint32_t made_w, made_h;    /* the frame size the swapchain was made for */
    uint32_t bar_x, bar_y;      /* direct: where the frame lies in it */
    unsigned strikes;           /* direct: swapchains remade in a row without a frame shown */
    uint64_t presents, direct_presents;
    uint32_t nimages;
    VkImage images[MAX_IMAGES];
    VkImageLayout layouts[MAX_IMAGES];
    /* off-screen target (tests) */
    int offscreen;
    VkImage off;
    VkDeviceMemory off_mem;
    /* the frame */
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *staging_ptr;
    VkDeviceSize staging_size;
    VkImage frame;
    VkDeviceMemory frame_mem;
    uint32_t frame_w, frame_h;
    VkImageLayout frame_layout;
};

static int memory_type(Screen *s, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < s->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s->mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static int alloc_bind_image(Screen *s, VkImage img, VkDeviceMemory *out) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s->dev, img, &req);
    int t = memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = memory_type(s, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)t};
    if (t < 0 || vkAllocateMemory(s->dev, &ai, NULL, out) != VK_SUCCESS) return -1;
    return vkBindImageMemory(s->dev, img, *out, 0) == VK_SUCCESS ? 0 : -1;
}

static VkImage make_image(Screen *s, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkDeviceMemory *mem) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = f, ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage, ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    if (vkCreateImage(s->dev, &ci, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (alloc_bind_image(s, img, mem)) { vkDestroyImage(s->dev, img, NULL); return VK_NULL_HANDLE; }
    return img;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src, b.dstAccessMask = dst, b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void destroy_swapchain(Screen *s) {
    if (s->swapchain) vkDestroySwapchainKHR(s->dev, s->swapchain, NULL);
    s->swapchain = VK_NULL_HANDLE, s->nimages = 0;
}

/* The swapchain's size for a frame of w x h in a window of cw x ch, at the frame's own scale: the window's
   shape, the frame's pixels. One side is the frame's, the other the frame's or more (the bars). Answers 0 where
   the window is smaller than the frame in either direction (the frame would have to shrink: the blit's work). */
static int direct_extent(uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, VkExtent2D *out) {
    if (!cw || !ch || !w || !h || cw < w || ch < h) return 0;
    double scale = (double)cw / w < (double)ch / h ? (double)cw / w : (double)ch / h;
    uint32_t ew = (uint32_t)((double)cw / scale + 0.5), eh = (uint32_t)((double)ch / scale + 0.5);
    if (ew < w) ew = w;
    if (eh < h) eh = h;
    *out = (VkExtent2D){ew, eh};
    return 1;
}

/* The swapchain for frames of w x h (0: not known yet; the window's size). */
static int make_swapchain(Screen *s, uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->phys, s->surface, &caps) != VK_SUCCESS) return -1;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys, s->surface, &n, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (n > 32) n = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys, s->surface, &n, fmts);
    s->format = n ? fmts[0].format : VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR space = n ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    /* RGBA8 first: the frame's own bytes, which the direct form copies as they are. */
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) s->format = fmts[i].format, space = fmts[i].colorSpace, found = 1;
    for (uint32_t i = 0; i < n && !found; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) s->format = fmts[i].format, space = fmts[i].colorSpace, found = 1;
    s->window_extent = caps.currentExtent;
#ifdef __ANDROID__
    if (s->window_extent.width == 0xffffffff && s->window)
        s->window_extent = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(s->window), (uint32_t)ANativeWindow_getHeight(s->window)};
#endif
    if (!s->window_extent.width || !s->window_extent.height || s->window_extent.width == 0xffffffff) return -1;
    /* The frame's scale where the surface takes such a swapchain: not rotated (a rotated one is drawn turned,
       which only the blit's form was ever asked to do here), RGBA8, and a size within the surface's limits. */
    VkExtent2D at_frame = {0, 0};
    s->why_not = s->no_direct ? (s->off_by ? s->off_by : "that form is off here")
               : !w || !h ? "no frame yet"
               : s->format != VK_FORMAT_R8G8B8A8_UNORM ? "the surface has no RGBA8 format"
               : caps.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? "the surface is rotated"
               : !direct_extent(s->window_extent.width, s->window_extent.height, w, h, &at_frame) ? "the window is smaller than the frame"
               : at_frame.width < caps.minImageExtent.width || at_frame.height < caps.minImageExtent.height ||
                 at_frame.width > caps.maxImageExtent.width || at_frame.height > caps.maxImageExtent.height
                     ? "the surface takes no swapchain of that size"
               : NULL;
    s->direct = !s->why_not;
    s->extent = s->direct ? at_frame : s->window_extent;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s->surface;
    ci.minImageCount = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s->format, ci.imageColorSpace = space, ci.imageExtent = s->extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    /* Android rotates in the compositor when the transform is not the identity: ask for what it has. */
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                      : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s->swapchain;
    VkSwapchainKHR sc;
    VkResult r = vkCreateSwapchainKHR(s->dev, &ci, NULL, &sc);
    destroy_swapchain(s);
    if (r != VK_SUCCESS && s->direct) {
        /* The surface refused the frame's scale: the window's, from now on. */
        rlog("swapchain %ux%u for a %ux%u window: %d; the window's size instead", s->extent.width, s->extent.height,
             s->window_extent.width, s->window_extent.height, r);
        s->no_direct = 1, s->direct = 0, s->why_not = "the surface refused a swapchain of the frame's scale";
        ci.imageExtent = s->extent = s->window_extent, ci.oldSwapchain = VK_NULL_HANDLE;
        r = vkCreateSwapchainKHR(s->dev, &ci, NULL, &sc);
    }
    if (r != VK_SUCCESS) { rlog("swapchain: %d", r); return -1; }
    s->swapchain = sc;
    s->made_w = w, s->made_h = h;
    s->bar_x = s->direct ? (s->extent.width - w) / 2 : 0, s->bar_y = s->direct ? (s->extent.height - h) / 2 : 0;
    s->nimages = MAX_IMAGES;
    vkGetSwapchainImagesKHR(s->dev, sc, &s->nimages, s->images);
    for (uint32_t i = 0; i < s->nimages; i++) s->layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    if (s->direct)
        rlog("swapchain %ux%u for the %ux%u frame in a %ux%u window (the compositor scales it; a present is one copy), %u images, format %d",
             s->extent.width, s->extent.height, w, h, s->window_extent.width, s->window_extent.height, s->nimages, s->format);
    else
        rlog("swapchain %ux%u, %u images, format %d%s%s", s->extent.width, s->extent.height, s->nimages, s->format,
             w && h ? " (the frame is scaled into it by a blit): " : "", w && h && s->why_not ? s->why_not : "");
    return 0;
}

/* Tests: the off-screen image as a window of cw x ch would have its swapchain, for frames of w x h. */
static int make_offscreen(Screen *s, uint32_t w, uint32_t h) {
    VkExtent2D at_frame;
    s->direct = !s->no_direct && w && h && direct_extent(s->window_extent.width, s->window_extent.height, w, h, &at_frame);
    VkExtent2D e = s->direct ? at_frame : s->window_extent;
    if (!s->off || e.width != s->extent.width || e.height != s->extent.height) {
        vkDeviceWaitIdle(s->dev);
        if (s->off) vkDestroyImage(s->dev, s->off, NULL);
        if (s->off_mem) vkFreeMemory(s->dev, s->off_mem, NULL);
        s->off_mem = VK_NULL_HANDLE;
        s->off = make_image(s, e.width, e.height, s->format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->off_mem);
        if (!s->off) return -1;
    }
    s->extent = e, s->made_w = w, s->made_h = h;
    s->bar_x = s->direct ? (e.width - w) / 2 : 0, s->bar_y = s->direct ? (e.height - h) / 2 : 0;
    s->nimages = 1, s->images[0] = s->off, s->layouts[0] = VK_IMAGE_LAYOUT_UNDEFINED;
    return 0;
}

static Screen *create(void *window, uint32_t off_w, uint32_t off_h, int off_direct) {
    Screen *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->window = window;
    s->offscreen = !window;
    /* HENGE_SCREEN_DIRECT=0: the window-sized swapchain and the blit, as before 0.3.53. */
    const char *direct = getenv("HENGE_SCREEN_DIRECT");
    s->no_direct = (direct && *direct == '0') || (!window && !off_direct);
    /* HENGE_SCREEN_SHARP=1 (Settings > Screen scaling: Sharp): where the window is a whole number of
       frames across or down (the Mac's screen at half the panel, 1600x1068 on the Mi Pad 7's 3200x2136),
       the blit repeats each pixel 2x2 (nearest) instead of blending neighbours, as macOS-uke's scanout
       shader does (gpu/plugin/scanout_x2.ll). Any other scale stays linear, with the same geometry, so
       touches map as before. */
    const char *sharp = getenv("HENGE_SCREEN_SHARP");
    s->sharp = sharp && *sharp == '1';
    if (direct && *direct == '0') s->why_not = s->off_by = s->sharp ? "Screen scaling: Sharp" : "Screen scaling: Blit";
    const char *exts[2];
    uint32_t next = 0;
#ifdef __ANDROID__
    if (window) exts[next++] = VK_KHR_SURFACE_EXTENSION_NAME, exts[next++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Henge", 1, "henge-gpu", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, next, exts};
    if (vkCreateInstance(&ici, NULL, &s->inst) != VK_SUCCESS) { rlog("no Vulkan instance"); free(s); return NULL; }
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(s->inst, &n, &s->phys) < 0 || !n) { rlog("no Vulkan device"); screen_destroy(s); return NULL; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s->phys, &props);
    vkGetPhysicalDeviceMemoryProperties(s->phys, &s->mem);
#ifdef __ANDROID__
    if (window) {
        VkAndroidSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, window};
        if (vkCreateAndroidSurfaceKHR(s->inst, &sci, NULL, &s->surface) != VK_SUCCESS) { rlog("no Vulkan surface"); screen_destroy(s); return NULL; }
    }
#endif
    VkQueueFamilyProperties fams[16];
    uint32_t nf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(s->phys, &nf, fams);
    s->family = UINT32_MAX;
    for (uint32_t i = 0; i < nf && s->family == UINT32_MAX; i++) {
        VkBool32 present = VK_TRUE;
        if (s->surface) vkGetPhysicalDeviceSurfaceSupportKHR(s->phys, i, s->surface, &present);
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) s->family = i;
    }
    if (s->family == UINT32_MAX) { rlog("no graphics queue"); screen_destroy(s); return NULL; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, s->family, 1, &prio};
    const char *dexts[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, s->surface ? 1u : 0u, dexts, NULL};
    if (vkCreateDevice(s->phys, &dci, NULL, &s->dev) != VK_SUCCESS) { rlog("no Vulkan logical device"); screen_destroy(s); return NULL; }
    vkGetDeviceQueue(s->dev, s->family, 0, &s->queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, s->family};
    vkCreateCommandPool(s->dev, &pci, NULL, &s->pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(s->dev, &cai, &s->cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(s->dev, &fci, NULL, &s->fence);
    VkSemaphoreCreateInfo sci2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(s->dev, &sci2, NULL, &s->acquired);
    vkCreateSemaphore(s->dev, &sci2, NULL, &s->rendered);
    if (s->surface) {
        /* The swapchain waits for the first frame, whose size decides its own. */
    } else {
        s->format = VK_FORMAT_R8G8B8A8_UNORM, s->window_extent = (VkExtent2D){off_w, off_h};
        if (make_offscreen(s, 0, 0)) { rlog("no off-screen image"); screen_destroy(s); return NULL; }
    }
    rlog("Vulkan on %s", props.deviceName);
    return s;
}

Screen *screen_create(void *window) { return create(window, OFFSCREEN_W, OFFSCREEN_H, 0); }
Screen *screen_create_offscreen(uint32_t w, uint32_t h) { return create(NULL, w, h, 1); }

static void destroy_image(Screen *s) {
    if (s->frame) vkDestroyImage(s->dev, s->frame, NULL);
    if (s->frame_mem) vkFreeMemory(s->dev, s->frame_mem, NULL);
    s->frame = VK_NULL_HANDLE, s->frame_mem = VK_NULL_HANDLE;
}

static void destroy_frame(Screen *s) {
    if (s->staging_ptr) vkUnmapMemory(s->dev, s->staging_mem);
    if (s->staging) vkDestroyBuffer(s->dev, s->staging, NULL);
    if (s->staging_mem) vkFreeMemory(s->dev, s->staging_mem, NULL);
    destroy_image(s);
    s->staging_ptr = NULL, s->staging = VK_NULL_HANDLE, s->staging_mem = VK_NULL_HANDLE;
    s->frame_w = s->frame_h = 0;
}

void screen_destroy(Screen *s) {
    if (!s) return;
    if (s->dev) {
        vkDeviceWaitIdle(s->dev);
        destroy_frame(s);
        destroy_swapchain(s);
        if (s->off) vkDestroyImage(s->dev, s->off, NULL);
        if (s->off_mem) vkFreeMemory(s->dev, s->off_mem, NULL);
        if (s->fence) vkDestroyFence(s->dev, s->fence, NULL);
        if (s->acquired) vkDestroySemaphore(s->dev, s->acquired, NULL);
        if (s->rendered) vkDestroySemaphore(s->dev, s->rendered, NULL);
        if (s->pool) vkDestroyCommandPool(s->dev, s->pool, NULL);
        vkDestroyDevice(s->dev, NULL);
    }
    if (s->surface) vkDestroySurfaceKHR(s->inst, s->surface, NULL);
    if (s->inst) vkDestroyInstance(s->inst, NULL);
    free(s);
}

/* The frame's buffer, for w x h. Returns 0 with it as it was, 1 with a new one (its bytes anything), -1 without. */
static int ensure_frame(Screen *s, uint32_t w, uint32_t h) {
    if (s->staging_ptr && s->frame_w == w && s->frame_h == h) return 0;
    vkDeviceWaitIdle(s->dev);
    destroy_frame(s);
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(s->dev, &bci, NULL, &s->staging) != VK_SUCCESS) { s->staging = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s->dev, s->staging, &req);
    /* Cached memory where the device has it: the pointer is drawn over these pixels by reading them. */
    int t = memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)t};
    if (t < 0 || vkAllocateMemory(s->dev, &ai, NULL, &s->staging_mem) != VK_SUCCESS) { s->staging_mem = VK_NULL_HANDLE; destroy_frame(s); return -1; }
    vkBindBufferMemory(s->dev, s->staging, s->staging_mem, 0);
    if (vkMapMemory(s->dev, s->staging_mem, 0, bytes, 0, &s->staging_ptr) != VK_SUCCESS) { s->staging_ptr = NULL; destroy_frame(s); return -1; }
    s->staging_size = bytes;
    s->frame_w = w, s->frame_h = h;
    return 1;
}

/* The image the blit's form uploads the frame to. */
static int ensure_image(Screen *s) {
    if (s->frame) return 0;
    s->frame = make_image(s, s->frame_w, s->frame_h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->frame_mem);
    s->frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return s->frame ? 0 : -1;
}

uint8_t *screen_frame(Screen *s, uint32_t w, uint32_t h, int *fresh) {
    if (!s || !w || !h) return NULL;
    int made = ensure_frame(s, w, h);
    if (made < 0) return NULL;
    if (fresh) *fresh = made;
    return s->staging_ptr;
}

/* A swapchain that could not take a frame, or that the surface wants remade. In the direct form, three in a
   row without a frame shown between them end that form here: a surface that calls every swapchain of another
   size than its own suboptimal would otherwise never show anything. */
static void remake_swapchain(Screen *s, uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(s->dev);
    if (s->direct && ++s->strikes >= 3) {
        rlog("the swapchain at the frame's scale was remade %u times with no frame shown: the window's size and a blit from now on", s->strikes);
        s->no_direct = 1, s->strikes = 0;
    }
    make_swapchain(s, w, h);
}

int screen_present(Screen *s, uint32_t w, uint32_t h) {
    if (!s || !w || !h || !s->staging_ptr || s->frame_w != w || s->frame_h != h) return -1;
    if (s->surface) {
        if ((!s->swapchain || s->made_w != w || s->made_h != h) && (vkDeviceWaitIdle(s->dev), make_swapchain(s, w, h))) return -1;
    } else if ((s->made_w != w || s->made_h != h) && make_offscreen(s, w, h)) return -1;
    if (!s->direct) {
        if (ensure_image(s)) return -1;
    } else if (s->frame) vkDeviceWaitIdle(s->dev), destroy_image(s);
    uint32_t index = 0;
    if (s->swapchain) {
        VkResult r = vkAcquireNextImageKHR(s->dev, s->swapchain, 1000000000ull, s->acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) { /* the acquire still signals: let it go by */
                VkPipelineStageFlags st = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &s->acquired, &st, 0, NULL, 0, NULL};
                vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE);
            }
            remake_swapchain(s, w, h);
            return -1;
        }
        if (r != VK_SUCCESS) return -1;
    }
    VkImage target = s->images[index];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(s->cb, 0);
    vkBeginCommandBuffer(s->cb, &bi);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier(s->cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (s->direct) {
        /* The frame's pixels as they are, between the bars (cleared only where there are any). */
        if (s->extent.width != w || s->extent.height != h) {
            vkCmdClearColorImage(s->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
            vkCmdPipelineBarrier(s->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int32_t)s->bar_x, (int32_t)s->bar_y, 0}, {w, h, 1}};
        vkCmdCopyBufferToImage(s->cb, s->staging, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        barrier(s->cb, s->frame, s->frame_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyBufferToImage(s->cb, s->staging, s->frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(s->cb, s->frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        s->frame_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        /* letterbox: the Mac's aspect kept */
        uint32_t tw = s->extent.width, th = s->extent.height;
        double scale = (double)tw / w < (double)th / h ? (double)tw / w : (double)th / h;
        int32_t dw = (int32_t)(w * scale), dh = (int32_t)(h * scale), dx = ((int32_t)tw - dw) / 2, dy = ((int32_t)th - dh) / 2;
        /* Sharp: only where the fit is itself a whole number (one side filled exactly), so the picture lies
           exactly where the linear blit puts it. */
        uint32_t k = (uint32_t)(scale + 1e-9);
        s->sharp_now = s->sharp && k >= 1 && (w * k == tw || h * k == th);
        if (s->sharp_now)
            dw = (int32_t)(w * k), dh = (int32_t)(h * k), dx = ((int32_t)tw - dw) / 2, dy = ((int32_t)th - dh) / 2;
        /* The bars only: a picture that covers the whole window is not cleared under it first (a window-sized
           write saved at every frame). */
        if (dw != (int32_t)tw || dh != (int32_t)th)
            vkCmdClearColorImage(s->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(s->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkCmdBlitImage(s->cb, s->frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       s->sharp_now ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    VkImageLayout final = s->swapchain ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier(s->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    s->layouts[index] = final;
    vkEndCommandBuffer(s->cb);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, s->swapchain ? 1u : 0u, &s->acquired, &wait, 1, &s->cb,
                       s->swapchain ? 1u : 0u, &s->rendered};
    vkResetFences(s->dev, 1, &s->fence);
    if (vkQueueSubmit(s->queue, 1, &si, s->fence) != VK_SUCCESS) return -1;
    int ok = 0;
    if (s->swapchain) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &s->rendered, 1, &s->swapchain, &index, NULL};
        VkResult r = vkQueuePresentKHR(s->queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vkWaitForFences(s->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
            /* A suboptimal present was shown all the same; one that was out of date was not. */
            remake_swapchain(s, w, h);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) ok = -1;
        } else if (r != VK_SUCCESS) ok = -1;
        else s->strikes = 0;
    }
    /* The buffer is the next frame's too: nothing of this one is left reading it. */
    vkWaitForFences(s->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
    if (!ok) s->presents++, s->direct_presents += s->direct ? 1 : 0;
    return ok;
}

int screen_show(Screen *s, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint8_t *to = screen_frame(s, w, h, NULL);
    if (!to) return -1;
    memcpy(to, rgba, (size_t)w * h * 4);
    return screen_present(s, w, h);
}

void screen_words(Screen *s, char *out, size_t n) {
    if (!n) return;
    if (!s) snprintf(out, n, "no screen");
    else if (!s->swapchain && !s->offscreen) snprintf(out, n, "no swapchain yet");
    else if (s->direct)
        snprintf(out, n, "a %ux%u swapchain at the frame's scale in a %ux%u window, scaled by the compositor", s->extent.width,
                 s->extent.height, s->window_extent.width, s->window_extent.height);
    else
        snprintf(out, n, "a %ux%u swapchain the frame is scaled into by a blit%s (%s)", s->extent.width, s->extent.height,
                 s->sharp_now ? ", sharp: each pixel repeated, a whole number of times" : "", s->why_not ? s->why_not : "?");
}

const uint8_t *screen_frame_now(Screen *s, uint32_t *w, uint32_t *h) {
    if (!s || !s->staging_ptr) return NULL;
    *w = s->frame_w, *h = s->frame_h;
    return s->staging_ptr;
}

int screen_readback(Screen *s, uint8_t **out, uint32_t *w, uint32_t *h) {
    if (!s || !s->off) return -1;
    VkDeviceSize bytes = (VkDeviceSize)s->extent.width * s->extent.height * 4;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(s->dev, &bci, NULL, &buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s->dev, buf, &req);
    int t = memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)t};
    vkAllocateMemory(s->dev, &ai, NULL, &mem);
    vkBindBufferMemory(s->dev, buf, mem, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(s->cb, 0);
    vkBeginCommandBuffer(s->cb, &bi);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {s->extent.width, s->extent.height, 1}};
    vkCmdCopyImageToBuffer(s->cb, s->off, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    vkEndCommandBuffer(s->cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &s->cb, 0, NULL};
    vkResetFences(s->dev, 1, &s->fence);
    vkQueueSubmit(s->queue, 1, &si, s->fence);
    vkWaitForFences(s->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
    void *p;
    vkMapMemory(s->dev, mem, 0, bytes, 0, &p);
    *out = malloc(bytes);
    if (*out) memcpy(*out, p, bytes);
    vkUnmapMemory(s->dev, mem);
    vkDestroyBuffer(s->dev, buf, NULL);
    vkFreeMemory(s->dev, mem, NULL);
    *w = s->extent.width, *h = s->extent.height;
    return *out ? 0 : -1;
}
