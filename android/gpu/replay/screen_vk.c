/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Mac's screen on Vulkan: each frame is uploaded to an image and scaled into the swapchain with
   a blit (letterboxed, linear filtering). With no window (tests) it draws into an off-screen image. */
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
    VkExtent2D extent;
    uint32_t nimages;
    VkImage images[MAX_IMAGES];
    VkImageLayout layouts[MAX_IMAGES];
    /* off-screen target (tests) */
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

static int make_swapchain(Screen *s) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->phys, s->surface, &caps) != VK_SUCCESS) return -1;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys, s->surface, &n, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (n > 32) n = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys, s->surface, &n, fmts);
    s->format = n ? fmts[0].format : VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR space = n ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    for (uint32_t i = 0; i < n; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            s->format = fmts[i].format, space = fmts[i].colorSpace;
            break;
        }
    s->extent = caps.currentExtent;
#ifdef __ANDROID__
    if (s->extent.width == 0xffffffff && s->window)
        s->extent = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(s->window), (uint32_t)ANativeWindow_getHeight(s->window)};
#endif
    if (!s->extent.width || !s->extent.height) return -1;
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
    if (r != VK_SUCCESS) { rlog("swapchain: %d", r); return -1; }
    s->swapchain = sc;
    s->nimages = MAX_IMAGES;
    vkGetSwapchainImagesKHR(s->dev, sc, &s->nimages, s->images);
    for (uint32_t i = 0; i < s->nimages; i++) s->layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    rlog("swapchain %ux%u, %u images, format %d", s->extent.width, s->extent.height, s->nimages, s->format);
    return 0;
}

Screen *screen_create(void *window) {
    Screen *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->window = window;
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
        if (make_swapchain(s)) rlog("no swapchain yet");
    } else {
        s->format = VK_FORMAT_R8G8B8A8_UNORM, s->extent = (VkExtent2D){OFFSCREEN_W, OFFSCREEN_H};
        s->off = make_image(s, OFFSCREEN_W, OFFSCREEN_H, s->format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->off_mem);
        s->nimages = 1, s->images[0] = s->off, s->layouts[0] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    rlog("Vulkan on %s", props.deviceName);
    return s;
}

static void destroy_frame(Screen *s) {
    if (s->staging_ptr) vkUnmapMemory(s->dev, s->staging_mem);
    if (s->staging) vkDestroyBuffer(s->dev, s->staging, NULL);
    if (s->staging_mem) vkFreeMemory(s->dev, s->staging_mem, NULL);
    if (s->frame) vkDestroyImage(s->dev, s->frame, NULL);
    if (s->frame_mem) vkFreeMemory(s->dev, s->frame_mem, NULL);
    s->staging_ptr = NULL, s->staging = VK_NULL_HANDLE, s->staging_mem = VK_NULL_HANDLE;
    s->frame = VK_NULL_HANDLE, s->frame_mem = VK_NULL_HANDLE, s->frame_w = s->frame_h = 0;
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

static int ensure_frame(Screen *s, uint32_t w, uint32_t h) {
    if (s->frame && s->frame_w == w && s->frame_h == h) return 0;
    vkDeviceWaitIdle(s->dev);
    destroy_frame(s);
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(s->dev, &bci, NULL, &s->staging) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s->dev, s->staging, &req);
    int t = memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)t};
    if (t < 0 || vkAllocateMemory(s->dev, &ai, NULL, &s->staging_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(s->dev, s->staging, s->staging_mem, 0);
    if (vkMapMemory(s->dev, s->staging_mem, 0, bytes, 0, &s->staging_ptr) != VK_SUCCESS) return -1;
    s->staging_size = bytes;
    s->frame = make_image(s, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->frame_mem);
    if (!s->frame) return -1;
    s->frame_w = w, s->frame_h = h, s->frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 0;
}

int screen_show(Screen *s, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (!s || !w || !h) return -1;
    if (s->surface && !s->swapchain && make_swapchain(s)) return -1;
    if (ensure_frame(s, w, h)) return -1;
    memcpy(s->staging_ptr, rgba, (size_t)w * h * 4);
    uint32_t index = 0;
    if (s->swapchain) {
        VkResult r = vkAcquireNextImageKHR(s->dev, s->swapchain, 1000000000ull, s->acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) { /* the acquire still signals: let it go by */
                VkPipelineStageFlags st = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &s->acquired, &st, 0, NULL, 0, NULL};
                vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE);
            }
            vkDeviceWaitIdle(s->dev);
            make_swapchain(s);
            return -1;
        }
        if (r != VK_SUCCESS) return -1;
    }
    VkImage target = s->images[index];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(s->cb, 0);
    vkBeginCommandBuffer(s->cb, &bi);
    barrier(s->cb, s->frame, s->frame_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(s->cb, s->staging, s->frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(s->cb, s->frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    s->frame_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier(s->cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    /* letterbox: the Mac's aspect kept */
    uint32_t tw = s->extent.width, th = s->extent.height;
    double scale = (double)tw / w < (double)th / h ? (double)tw / w : (double)th / h;
    int32_t dw = (int32_t)(w * scale), dh = (int32_t)(h * scale), dx = ((int32_t)tw - dw) / 2, dy = ((int32_t)th - dh) / 2;
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(s->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBlitImage(s->cb, s->frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
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
            vkDeviceWaitIdle(s->dev);
            make_swapchain(s);
        } else if (r != VK_SUCCESS) ok = -1;
    }
    vkWaitForFences(s->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
    return ok;
}

int screen_readback(Screen *s, uint8_t **out, uint32_t *w, uint32_t *h) {
    if (!s || !s->off) return -1;
    VkDeviceSize bytes = (VkDeviceSize)OFFSCREEN_W * OFFSCREEN_H * 4;
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
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {OFFSCREEN_W, OFFSCREEN_H, 1}};
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
    *w = OFFSCREEN_W, *h = OFFSCREEN_H;
    return *out ? 0 : -1;
}
