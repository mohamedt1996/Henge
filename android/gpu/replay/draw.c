/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Render passes that draw, on the GPU (Vulkan 1.3, dynamic rendering).

   A pass is run whole when it ends: its attachments are uploaded from guest memory (or cleared), each
   draw's buffers are copied into one host-visible arena and handed to the shaders as device addresses
   (air2spv's push-constant ABI), its textures uploaded, and what the pass stores is read back into guest
   memory afterwards. Slow, but guest memory stays the one truth, so the CPU paths (blits, clears) and
   the screen keep working on it unchanged. Keeping objects resident on the GPU is the next step.

   What a pipeline description holds is not decoded yet (nobody has published its format): its vertex
   and fragment functions are found by scanning it for refs to function objects, as libapplegfx-vulkan
   does, and everything else is a default: blending is premultiplied source-over, vertex attributes are
   packed in one buffer in attribute order, and the colour formats come from the pass. Draws that read
   the framebuffer (fb fetch) or use tessellation are skipped. */
#include <vulkan/vulkan.h>

#include "replay.h"

/* Vulkan 1.2/1.3 entry points: Android's libvulkan exports only 1.0/1.1 at minSdk 29. */
static PFN_vkCmdBeginRendering p_vkCmdBeginRendering;
#define vkCmdBeginRendering p_vkCmdBeginRendering
static PFN_vkCmdEndRendering p_vkCmdEndRendering;
#define vkCmdEndRendering p_vkCmdEndRendering
static PFN_vkCmdSetCullMode p_vkCmdSetCullMode;
#define vkCmdSetCullMode p_vkCmdSetCullMode
static PFN_vkCmdSetDepthCompareOp p_vkCmdSetDepthCompareOp;
#define vkCmdSetDepthCompareOp p_vkCmdSetDepthCompareOp
static PFN_vkCmdSetDepthTestEnable p_vkCmdSetDepthTestEnable;
#define vkCmdSetDepthTestEnable p_vkCmdSetDepthTestEnable
static PFN_vkCmdSetDepthWriteEnable p_vkCmdSetDepthWriteEnable;
#define vkCmdSetDepthWriteEnable p_vkCmdSetDepthWriteEnable
static PFN_vkCmdSetFrontFace p_vkCmdSetFrontFace;
#define vkCmdSetFrontFace p_vkCmdSetFrontFace
static PFN_vkCmdSetPrimitiveTopology p_vkCmdSetPrimitiveTopology;
#define vkCmdSetPrimitiveTopology p_vkCmdSetPrimitiveTopology
static PFN_vkCmdSetStencilOp p_vkCmdSetStencilOp;
#define vkCmdSetStencilOp p_vkCmdSetStencilOp
static PFN_vkCmdSetStencilTestEnable p_vkCmdSetStencilTestEnable;
#define vkCmdSetStencilTestEnable p_vkCmdSetStencilTestEnable
static PFN_vkGetBufferDeviceAddress p_vkGetBufferDeviceAddress;
#define vkGetBufferDeviceAddress p_vkGetBufferDeviceAddress

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BYTES (96u << 20)
#define MAX_DRAW_IMAGES 256

/* ------------------------------------------------------------------ the layout file -- */
enum { PUSH_CBUF, PUSH_TABLE, PUSH_CONST };

typedef struct {
    int stage; /* 0 vertex, 1 fragment */
    struct { int slot, kind, metal; } push[32];
    int npush;
    struct { int metal, off; int writes; } dbuf[31];
    int ndbuf;
    struct { int metal, set, binding, dim, array; char base; } tex[16];
    int ntex;
    struct { int metal, set, binding, cs; unsigned f[10]; float lod[2]; } smp[16];
    int nsmp;
    int nimg, nfbf;
    struct { int index, comps, bits; char base; } attr[16];
    int nattr;
} Layout;

static int parse_layout(const char *t, Layout *L) {
    memset(L, 0, sizeof *L);
    int ver = 0;
    if (sscanf(t, "air2spv-layout %d", &ver) != 1 || ver != 2) return -1;
    for (const char *line = t; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        char w[16], x[16], base[16];
        int a, b, c, d, e;
        if (sscanf(line, "%15s", w) != 1) continue;
        if (!strcmp(w, "stage")) { sscanf(line, "stage %15s", x); L->stage = !strcmp(x, "vertex") ? 0 : !strcmp(x, "fragment") ? 1 : 2; }
        else if (!strcmp(w, "push") && L->npush < 32 && sscanf(line, "push %d %15s %d", &a, x, &b) >= 2) {
            L->push[L->npush].slot = a, L->push[L->npush].metal = b;
            L->push[L->npush++].kind = !strcmp(x, "cbuf") ? PUSH_CBUF : !strcmp(x, "table") ? PUSH_TABLE : PUSH_CONST;
        } else if (!strcmp(w, "dbuf") && L->ndbuf < 31 && sscanf(line, "dbuf %d %d %15s", &a, &b, x) == 3) {
            L->dbuf[L->ndbuf].metal = a, L->dbuf[L->ndbuf].off = b, L->dbuf[L->ndbuf++].writes = strchr(x, 'w') != NULL;
        } else if (!strcmp(w, "tex") && L->ntex < 16 && sscanf(line, "tex %d %d %d %d %d %15s", &a, &b, &c, &d, &e, base) == 6) {
            L->tex[L->ntex].metal = a, L->tex[L->ntex].set = b, L->tex[L->ntex].binding = c;
            L->tex[L->ntex].dim = d, L->tex[L->ntex].array = e, L->tex[L->ntex++].base = base[0];
        } else if (!strcmp(w, "smp") && L->nsmp < 16 && sscanf(line, "smp %d %d %d", &a, &b, &c) == 3) {
            L->smp[L->nsmp].metal = a, L->smp[L->nsmp].set = b, L->smp[L->nsmp++].binding = c;
        } else if (!strcmp(w, "csmp") && L->nsmp < 16) {
            unsigned f[10];
            float lo, hi;
            if (sscanf(line, "csmp %d %d %d %u %u %u %u %u %u %u %u %u %u %g %g", &a, &b, &c, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7],
                       &f[8], &f[9], &lo, &hi) != 15) continue;
            L->smp[L->nsmp].metal = a, L->smp[L->nsmp].set = b, L->smp[L->nsmp].binding = c, L->smp[L->nsmp].cs = 1;
            memcpy(L->smp[L->nsmp].f, f, sizeof f);
            L->smp[L->nsmp].lod[0] = lo, L->smp[L->nsmp++].lod[1] = hi;
        } else if (!strcmp(w, "img")) L->nimg++;
        else if (!strcmp(w, "fbf")) L->nfbf++;
        else if (!strcmp(w, "attr") && L->nattr < 16 && sscanf(line, "attr %d %d %d %15s", &a, &b, &c, base) == 4) {
            L->attr[L->nattr].index = a, L->attr[L->nattr].comps = b, L->attr[L->nattr].bits = c, L->attr[L->nattr++].base = base[0];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ state -- */
typedef struct Shader {
    uint64_t hash;
    int ok;
    VkShaderModule mod;
    Layout L;
    uint8_t *cdata;
    size_t clen;
    struct Shader *next;
} Shader;

typedef struct Pipe {
    uint64_t vs, fs;
    VkFormat color[8], depth, stencil;
    int topo_class, nattr;
    VkPipeline p;
    struct Pipe *next;
} Pipe;

typedef struct { Obj *o; uint64_t off; } BufBind;
typedef struct { BufBind buf[31]; Obj *tex[32]; Obj *smp[16]; } StageBind;

typedef struct { Obj *o; VkImage img; VkImageView view; VkDeviceMemory mem; VkFormat fmt; } PassImage;
typedef struct { Obj *o; uint64_t at, len; int write_back; } ArenaCopy;
typedef struct { uint64_t key; VkSampler s; } SamplerEntry;

typedef struct {
    Obj *tex;                 /* where it draws (the resolve texture when there is one) */
    uint32_t level, slice, load, store;
    double clear[4];
    PassImage *pi;
} Attach;

struct Gpu {
    Objects *os;
    Shaders *shaders;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandPool pool;
    VkCommandBuffer pre, main;
    VkFence fence;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    VkDescriptorPool dpool;
    /* the arena */
    VkBuffer arena;
    VkDeviceMemory arena_mem;
    uint8_t *arena_ptr;
    VkDeviceAddress arena_addr;
    uint64_t arena_used;
    ArenaCopy copies[512];
    int ncopies;
    PassImage images[MAX_DRAW_IMAGES];
    int nimages;
    PassImage dummy; /* 1x1 transparent black, for a texture the CPU cannot see */
    VkSampler default_sampler;
    SamplerEntry samplers[128];
    int nsamplers;
    Shader *shader_list;
    Pipe *pipes;
    /* the pass */
    int open, running;
    int lost; /* a pass hung or the device was lost: no more passes on the GPU */
    uint32_t task;
    uint8_t pass_rec[0x248];
    uint32_t pass_size;
    Attach color[8], depth;
    uint32_t width, height;
    StageBind st[2];
    Obj *pipeline, *dss;
    uint32_t cull, winding, stencil_ref[2];
    VkViewport viewport;
    VkRect2D scissor;
    int have_viewport, have_scissor;
    /* counts */
    uint64_t gpu_passes, draws, skipped, npipes;
    char why[160];
};

static int memory_type(Gpu *g, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g->mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void skip(Gpu *g, const char *why) {
    g->skipped++;
    if (strcmp(g->why, why)) {
        snprintf(g->why, sizeof g->why, "%s", why);
        if (g->skipped < 2000) rlog("draw skipped: %s", why);
    }
}

/* ------------------------------------------------------------------ the device -- */
Gpu *gpu_create(Objects *os, Shaders *shaders) {
    Gpu *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->os = os, g->shaders = shaders;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Henge", 1, "henge-draw", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    uint32_t n = 1;
    VkPhysicalDeviceProperties props;
    if (vkCreateInstance(&ici, NULL, &g->inst) != VK_SUCCESS || vkEnumeratePhysicalDevices(g->inst, &n, &g->phys) < 0 || !n) goto fail;
    vkGetPhysicalDeviceProperties(g->phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) { rlog("draws off: Vulkan %u.%u, 1.3 needed", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion)); goto fail; }
    if (props.limits.maxPushConstantsSize < 256) { rlog("draws off: push constants %u bytes, 256 needed", props.limits.maxPushConstantsSize); goto fail; }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(g->phys, &f);
    if (!f12.bufferDeviceAddress || !f.features.shaderInt64 || !f13.dynamicRendering) {
        rlog("draws off: needs bufferDeviceAddress %d, shaderInt64 %d, dynamicRendering %d", f12.bufferDeviceAddress, f.features.shaderInt64, f13.dynamicRendering);
        goto fail;
    }
    /* Ask for what the translated shaders may use, where the device has it. */
    VkPhysicalDeviceVulkan13Features w13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    w13.dynamicRendering = VK_TRUE, w13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;
    VkPhysicalDeviceVulkan12Features w12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &w13};
    w12.bufferDeviceAddress = VK_TRUE, w12.shaderFloat16 = f12.shaderFloat16, w12.shaderInt8 = f12.shaderInt8;
    w12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess, w12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess;
    w12.scalarBlockLayout = f12.scalarBlockLayout, w12.vulkanMemoryModel = f12.vulkanMemoryModel;
    w12.vulkanMemoryModelDeviceScope = f12.vulkanMemoryModelDeviceScope;
    VkPhysicalDeviceVulkan11Features w11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &w12};
    w11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess, w11.uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess;
    w11.storagePushConstant16 = f11.storagePushConstant16;
    VkPhysicalDeviceFeatures2 want = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &w11};
    want.features.shaderInt64 = VK_TRUE, want.features.shaderInt16 = f.features.shaderInt16;
    want.features.fragmentStoresAndAtomics = f.features.fragmentStoresAndAtomics;
    want.features.vertexPipelineStoresAndAtomics = f.features.vertexPipelineStoresAndAtomics;
    want.features.shaderStorageImageWriteWithoutFormat = f.features.shaderStorageImageWriteWithoutFormat;
    want.features.shaderStorageImageReadWithoutFormat = f.features.shaderStorageImageReadWithoutFormat;
    want.features.samplerAnisotropy = f.features.samplerAnisotropy;
    want.features.independentBlend = f.features.independentBlend;
    want.features.shaderImageGatherExtended = f.features.shaderImageGatherExtended;
    VkQueueFamilyProperties fams[16];
    uint32_t nf = 16, family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nf, fams);
    for (uint32_t i = 0; i < nf && family == UINT32_MAX; i++)
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    if (family == UINT32_MAX) goto fail;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(g->phys, &dci, NULL, &g->dev) != VK_SUCCESS) goto fail;
    p_vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g->dev, "vkCmdBeginRendering");
    p_vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g->dev, "vkCmdEndRendering");
    p_vkCmdSetCullMode = (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(g->dev, "vkCmdSetCullMode");
    p_vkCmdSetDepthCompareOp = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(g->dev, "vkCmdSetDepthCompareOp");
    p_vkCmdSetDepthTestEnable = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(g->dev, "vkCmdSetDepthTestEnable");
    p_vkCmdSetDepthWriteEnable = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(g->dev, "vkCmdSetDepthWriteEnable");
    p_vkCmdSetFrontFace = (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(g->dev, "vkCmdSetFrontFace");
    p_vkCmdSetPrimitiveTopology = (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(g->dev, "vkCmdSetPrimitiveTopology");
    p_vkCmdSetStencilOp = (PFN_vkCmdSetStencilOp)vkGetDeviceProcAddr(g->dev, "vkCmdSetStencilOp");
    p_vkCmdSetStencilTestEnable = (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(g->dev, "vkCmdSetStencilTestEnable");
    p_vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(g->dev, "vkGetBufferDeviceAddress");
    if (!p_vkCmdBeginRendering || !p_vkGetBufferDeviceAddress || !p_vkCmdSetStencilOp) goto fail;
    vkGetDeviceQueue(g->dev, family, 0, &g->queue);
    vkGetPhysicalDeviceMemoryProperties(g->phys, &g->mem);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    vkCreateCommandPool(g->dev, &pci, NULL, &g->pool);
    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    vkAllocateCommandBuffers(g->dev, &cai, cbs);
    g->pre = cbs[0], g->main = cbs[1];
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g->dev, &fci, NULL, &g->fence);

    /* One layout for every pipeline: set 0 the fragment stage's resources, set 1 the vertex stage's
       (air2spv ABI v2), 256 bytes of push constants (16 buffer addresses a stage). */
    for (int s = 0; s < 2; s++) {
        VkDescriptorSetLayoutBinding b[48];
        uint32_t nb = 0;
        VkShaderStageFlags stage = s == 0 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        for (uint32_t i = 0; i < 16; i++) b[nb++] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, NULL};
        for (uint32_t i = 0; i < 16; i++) b[nb++] = (VkDescriptorSetLayoutBinding){16 + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage, NULL};
        for (uint32_t i = 0; i < 16; i++) b[nb++] = (VkDescriptorSetLayoutBinding){32 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b};
        if (vkCreateDescriptorSetLayout(g->dev, &lci, NULL, &g->set_layout[s]) != VK_SUCCESS) goto fail;
    }
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 256};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, g->set_layout, 1, &pr};
    if (vkCreatePipelineLayout(g->dev, &plci, NULL, &g->layout) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 4096, 3, sizes};
    if (vkCreateDescriptorPool(g->dev, &dpci, NULL, &g->dpool) != VK_SUCCESS) goto fail;

    /* The arena: buffers the draws read, as device addresses, plus staging for images. */
    VkBufferUsageFlags use = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, ARENA_BYTES, use, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(g->dev, &bci, NULL, &g->arena) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->dev, g->arena, &req);
    int t = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateFlagsInfo afi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &afi, req.size, (uint32_t)t};
    if (t < 0 || vkAllocateMemory(g->dev, &ai, NULL, &g->arena_mem) != VK_SUCCESS) goto fail;
    vkBindBufferMemory(g->dev, g->arena, g->arena_mem, 0);
    if (vkMapMemory(g->dev, g->arena_mem, 0, ARENA_BYTES, 0, (void **)&g->arena_ptr) != VK_SUCCESS) goto fail;
    VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, g->arena};
    g->arena_addr = vkGetBufferDeviceAddress(g->dev, &bai);

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(g->dev, &sci, NULL, &g->default_sampler);
    rlog("draws on %s (Vulkan %u.%u)", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    return g;
fail:
    rlog("draws off: no usable Vulkan 1.3 device");
    gpu_destroy(g);
    return NULL;
}

static void free_pass_images(Gpu *g) {
    for (int i = 0; i < g->nimages; i++) {
        vkDestroyImageView(g->dev, g->images[i].view, NULL);
        vkDestroyImage(g->dev, g->images[i].img, NULL);
        vkFreeMemory(g->dev, g->images[i].mem, NULL);
    }
    g->nimages = 0;
}

void gpu_destroy(Gpu *g) {
    if (!g) return;
    if (g->dev) {
        vkDeviceWaitIdle(g->dev);
        free_pass_images(g);
        if (g->dummy.view) vkDestroyImageView(g->dev, g->dummy.view, NULL), vkDestroyImage(g->dev, g->dummy.img, NULL), vkFreeMemory(g->dev, g->dummy.mem, NULL);
        for (Pipe *p = g->pipes; p;) { Pipe *n = p->next; vkDestroyPipeline(g->dev, p->p, NULL); free(p); p = n; }
        for (Shader *s = g->shader_list; s;) { Shader *n = s->next; if (s->mod) vkDestroyShaderModule(g->dev, s->mod, NULL); free(s->cdata); free(s); s = n; }
        for (int i = 0; i < g->nsamplers; i++) vkDestroySampler(g->dev, g->samplers[i].s, NULL);
        if (g->default_sampler) vkDestroySampler(g->dev, g->default_sampler, NULL);
        if (g->arena_ptr) vkUnmapMemory(g->dev, g->arena_mem);
        if (g->arena) vkDestroyBuffer(g->dev, g->arena, NULL);
        if (g->arena_mem) vkFreeMemory(g->dev, g->arena_mem, NULL);
        if (g->dpool) vkDestroyDescriptorPool(g->dev, g->dpool, NULL);
        if (g->layout) vkDestroyPipelineLayout(g->dev, g->layout, NULL);
        for (int s = 0; s < 2; s++) if (g->set_layout[s]) vkDestroyDescriptorSetLayout(g->dev, g->set_layout[s], NULL);
        if (g->fence) vkDestroyFence(g->dev, g->fence, NULL);
        if (g->pool) vkDestroyCommandPool(g->dev, g->pool, NULL);
        vkDestroyDevice(g->dev, NULL);
    }
    if (g->inst) vkDestroyInstance(g->inst, NULL);
    free(g);
}

void gpu_stats(Gpu *g, uint64_t out[4]) {
    out[0] = g ? g->gpu_passes : 0, out[1] = g ? g->draws : 0, out[2] = g ? g->skipped : 0, out[3] = g ? g->npipes : 0;
}

/* --------------------------------------------------------------------- formats -- */
static VkFormat vk_format(uint32_t mtl) {
    switch (mtl) {
    case 1: return VK_FORMAT_R8_UNORM; /* A8: the shader sees alpha in .r; close enough for coverage masks */
    case 10: return VK_FORMAT_R8_UNORM;
    case 11: return VK_FORMAT_R8_SRGB;
    case 12: return VK_FORMAT_R8_SNORM;
    case 13: return VK_FORMAT_R8_UINT;
    case 14: return VK_FORMAT_R8_SINT;
    case 20: return VK_FORMAT_R16_UNORM;
    case 22: return VK_FORMAT_R16_SNORM;
    case 23: return VK_FORMAT_R16_UINT;
    case 24: return VK_FORMAT_R16_SINT;
    case 25: return VK_FORMAT_R16_SFLOAT;
    case 30: return VK_FORMAT_R8G8_UNORM;
    case 31: return VK_FORMAT_R8G8_SRGB;
    case 32: return VK_FORMAT_R8G8_SNORM;
    case 33: return VK_FORMAT_R8G8_UINT;
    case 34: return VK_FORMAT_R8G8_SINT;
    case 53: return VK_FORMAT_R32_UINT;
    case 54: return VK_FORMAT_R32_SINT;
    case 55: return VK_FORMAT_R32_SFLOAT;
    case 60: return VK_FORMAT_R16G16_UNORM;
    case 62: return VK_FORMAT_R16G16_SNORM;
    case 63: return VK_FORMAT_R16G16_UINT;
    case 64: return VK_FORMAT_R16G16_SINT;
    case 65: return VK_FORMAT_R16G16_SFLOAT;
    case 70: return VK_FORMAT_R8G8B8A8_UNORM;
    case 71: return VK_FORMAT_R8G8B8A8_SRGB;
    case 72: return VK_FORMAT_R8G8B8A8_SNORM;
    case 73: return VK_FORMAT_R8G8B8A8_UINT;
    case 74: return VK_FORMAT_R8G8B8A8_SINT;
    case 80: return VK_FORMAT_B8G8R8A8_UNORM;
    case 81: return VK_FORMAT_B8G8R8A8_SRGB;
    case 90: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 91: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 92: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 93: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 94: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case 103: return VK_FORMAT_R32G32_UINT;
    case 104: return VK_FORMAT_R32G32_SINT;
    case 105: return VK_FORMAT_R32G32_SFLOAT;
    case 110: return VK_FORMAT_R16G16B16A16_UNORM;
    case 112: return VK_FORMAT_R16G16B16A16_SNORM;
    case 113: return VK_FORMAT_R16G16B16A16_UINT;
    case 114: return VK_FORMAT_R16G16B16A16_SINT;
    case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 123: return VK_FORMAT_R32G32B32A32_UINT;
    case 124: return VK_FORMAT_R32G32B32A32_SINT;
    case 125: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 250: return VK_FORMAT_D16_UNORM;
    case 252: return VK_FORMAT_D32_SFLOAT;
    case 253: return VK_FORMAT_S8_UINT;
    case 255: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 260: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 552: case 553: return VK_FORMAT_R16G16B16A16_SFLOAT; /* XR10 formats: 16 bits a channel (the CPU sees them as 8 bytes) */
    case 554: case 555: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static int is_depth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkImageAspectFlags aspect_of(VkFormat f) {
    if (f == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

/* --------------------------------------------------------------------- the arena -- */
static int64_t arena_alloc(Gpu *g, uint64_t n, uint64_t align) {
    uint64_t at = (g->arena_used + align - 1) & ~(align - 1);
    if (at + n > ARENA_BYTES) return -1;
    g->arena_used = at + n;
    return (int64_t)at;
}

/* A buffer's bytes in the arena, copied once a pass. Returns its arena offset, or -1. */
static int64_t arena_buffer(Gpu *g, Obj *o, int write_back) {
    for (int i = 0; i < g->ncopies; i++)
        if (g->copies[i].o == o) { g->copies[i].write_back |= write_back; return (int64_t)g->copies[i].at; }
    uint64_t len = 0;
    obj_refresh(g->os, o);
    uint8_t *bytes = obj_bytes(o, &len);
    if (!bytes || !len || g->ncopies == 512) return -1;
    if (len > (32u << 20)) len = 32u << 20;
    int64_t at = arena_alloc(g, len, 256);
    if (at < 0) return -1;
    memcpy(g->arena_ptr + at, bytes, len);
    g->copies[g->ncopies++] = (ArenaCopy){o, (uint64_t)at, len, write_back};
    return at;
}

/* -------------------------------------------------------------------- images -- */
static PassImage *make_image(Gpu *g, Obj *o, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips, int attach, VkImageViewType vt) {
    if (g->nimages == MAX_DRAW_IMAGES) return NULL;
    PassImage *pi = &g->images[g->nimages];
    memset(pi, 0, sizeof *pi);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = fmt, ci.extent = (VkExtent3D){w ? w : 1, h ? h : 1, 1};
    ci.mipLevels = mips ? mips : 1, ci.arrayLayers = layers ? layers : 1, ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (attach ? (is_depth(fmt) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0);
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(g->dev, &ci, NULL, &pi->img) != VK_SUCCESS) return NULL;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g->dev, pi->img, &req);
    int t = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)(t < 0 ? 0 : t)};
    if (vkAllocateMemory(g->dev, &ai, NULL, &pi->mem) != VK_SUCCESS) { vkDestroyImage(g->dev, pi->img, NULL); return NULL; }
    vkBindImageMemory(g->dev, pi->img, pi->mem, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, pi->img, vt, fmt};
    vci.subresourceRange = (VkImageSubresourceRange){aspect_of(fmt) & ~(attach ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT), 0, ci.mipLevels, 0, ci.arrayLayers};
    if (!vci.subresourceRange.aspectMask) vci.subresourceRange.aspectMask = aspect_of(fmt);
    if (vkCreateImageView(g->dev, &vci, NULL, &pi->view) != VK_SUCCESS) { vkDestroyImage(g->dev, pi->img, NULL); vkFreeMemory(g->dev, pi->mem, NULL); return NULL; }
    pi->o = o, pi->fmt = fmt;
    g->nimages++;
    return pi;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static uint8_t *texel_row(TexView *t, uint32_t level, uint32_t slice, uint32_t y, uint64_t bytes) {
    if (!t->mem || !*t->mem || level >= t->mips || slice >= t->slices) return NULL;
    uint64_t at = t->level_off[level] + (uint64_t)slice * t->slice_stride + (uint64_t)y * t->level_row[level];
    return at + bytes <= t->limit ? *t->mem + at : NULL;
}

/* Copies level/slice rows of a texture into the arena and records the upload into the image. */
static int upload(Gpu *g, VkCommandBuffer cb, PassImage *pi, TexView *t, uint32_t level, uint32_t slice, uint32_t dst_level, uint32_t dst_layer) {
    unsigned bpp = px_bytes(t->format);
    uint32_t w = t->w >> level ? t->w >> level : 1, h = t->h >> level ? t->h >> level : 1;
    if (!bpp) return -1;
    uint64_t line = (uint64_t)w * bpp;
    int64_t at = arena_alloc(g, line * h, 16);
    if (at < 0) return -1;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = texel_row(t, level, slice, y, line);
        if (row) memcpy(g->arena_ptr + at + y * line, row, line);
        else memset(g->arena_ptr + at + y * line, 0, line);
    }
    VkBufferImageCopy c = {(VkDeviceSize)at, 0, 0, {aspect_of(pi->fmt) & ~VK_IMAGE_ASPECT_STENCIL_BIT, dst_level, dst_layer, 1}, {0, 0, 0}, {w, h, 1}};
    if (!c.imageSubresource.aspectMask) c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyBufferToImage(cb, g->arena, pi->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return 0;
}

static PassImage *dummy_image(Gpu *g) {
    if (g->dummy.view) return &g->dummy;
    int n = g->nimages;
    PassImage *pi = make_image(g, NULL, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 0, VK_IMAGE_VIEW_TYPE_2D);
    if (!pi) return NULL;
    g->dummy = *pi;
    g->nimages = n; /* not a pass image: kept */
    barrier(g->pre, g->dummy.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g->pre, g->dummy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &r);
    barrier(g->pre, g->dummy.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &g->dummy;
}

/* A texture a draw samples, uploaded once a pass. */
static PassImage *sampled(Gpu *g, Obj *o, int dim, int array) {
    if (!o || !o->has_tex) return dummy_image(g);
    for (int i = 0; i < g->nimages; i++)
        if (g->images[i].o == o) return &g->images[i];
    for (int a = 0; a < 8; a++)
        if (g->color[a].tex == o) return dummy_image(g); /* drawn into in this pass: not readable here */
    TexView *t = &o->tex;
    VkFormat f = vk_format(t->format);
    if (!f || is_depth(f)) return dummy_image(g);
    int cube = dim == 3;
    VkImageViewType vt = cube ? (array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    uint32_t layers = t->slices;
    if (cube && layers % 6) return dummy_image(g);
    if (!array && !cube) layers = 1;
    obj_refresh(g->os, o);
    PassImage *pi = make_image(g, o, f, t->w, t->h, layers, t->mips, 0, vt);
    if (!pi) return dummy_image(g);
    barrier(g->pre, pi->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t m = 0; m < t->mips; m++)
        for (uint32_t s = 0; s < layers; s++) upload(g, g->pre, pi, t, m, s, m, s);
    barrier(g->pre, pi->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return pi;
}

/* ------------------------------------------------------------------- samplers -- */
static VkSamplerAddressMode metal_address(unsigned m) {
    switch (m) { /* MTLSamplerAddressMode */
    case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; /* clamp to zero, clamp to border colour */
    }
}

static VkSamplerAddressMode air_address(unsigned m) {
    switch (m) { /* AIR: 0 clamp to zero or border, 1 clamp to edge, 2 repeat, 3 mirrored repeat */
    case 1: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

static VkSampler sampler_for(Gpu *g, uint64_t key, VkSamplerCreateInfo *ci) {
    for (int i = 0; i < g->nsamplers; i++)
        if (g->samplers[i].key == key) return g->samplers[i].s;
    if (ci->unnormalizedCoordinates) { /* what Vulkan allows with pixel coordinates */
        ci->minFilter = ci->magFilter, ci->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, ci->minLod = ci->maxLod = 0;
        ci->anisotropyEnable = VK_FALSE, ci->compareEnable = VK_FALSE;
        if (ci->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ci->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (ci->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ci->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci->addressModeW = ci->addressModeU;
    }
    VkSampler s = VK_NULL_HANDLE;
    if (g->nsamplers == 128 || vkCreateSampler(g->dev, ci, NULL, &s) != VK_SUCCESS) return g->default_sampler;
    g->samplers[g->nsamplers++] = (SamplerEntry){key, s};
    return s;
}

/* A sampler object (serialized tag 3: {tag, size, ref, bits 64, lod min, lod max}), as devices/pvg_metal.m reads it. */
static VkSampler api_sampler(Gpu *g, Obj *o) {
    if (!o || o->ser_tag != 3 || o->ser_len < 28) return g->default_sampler;
    uint64_t bits;
    float lod[2];
    memcpy(&bits, o->ser + 12, 8), memcpy(lod, o->ser + 20, 8);
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.minFilter = (bits & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.magFilter = ((bits >> 2) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    unsigned cmp = (bits >> 5) & 7, mip = (bits >> 24) & 3, aniso = (bits >> 26) & 31, border = (bits >> 20) & 15;
    ci.compareEnable = cmp != 0, ci.compareOp = (VkCompareOp)cmp; /* MTLCompareFunction is Vulkan's order */
    ci.addressModeU = metal_address((bits >> 8) & 15), ci.addressModeV = metal_address((bits >> 12) & 15), ci.addressModeW = metal_address((bits >> 16) & 15);
    ci.borderColor = border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = mip ? lod[0] : 0, ci.maxLod = mip ? lod[1] : 0.25f;
    ci.anisotropyEnable = aniso > 1, ci.maxAnisotropy = (float)(aniso > 1 ? aniso : 1);
    ci.unnormalizedCoordinates = !((bits >> 31) & 1);
    return sampler_for(g, bits ^ ((uint64_t)o->unique_id << 40) ^ 0x5a5a, &ci);
}

static VkSampler constexpr_sampler(Gpu *g, const unsigned f[10], const float lod[2]) {
    /* f: s, t, r address, mag linear, min linear, mip filter, pixel coords, compare, aniso, border */
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.addressModeU = air_address(f[0]), ci.addressModeV = air_address(f[1]), ci.addressModeW = air_address(f[2]);
    ci.magFilter = f[3] ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, ci.minFilter = f[4] ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = f[5] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = f[5] ? lod[0] : 0, ci.maxLod = f[5] ? lod[1] : 0.25f;
    ci.unnormalizedCoordinates = f[6] != 0;
    static const VkCompareOp air_cmp[9] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                           VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER};
    ci.compareEnable = f[7] > 0 && f[7] < 8, ci.compareOp = air_cmp[f[7] < 9 ? f[7] : 8];
    ci.anisotropyEnable = f[8] > 1, ci.maxAnisotropy = (float)(f[8] > 1 ? f[8] : 1);
    ci.borderColor = f[9] == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : f[9] == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    uint64_t key = 0xc0ull << 56;
    for (int i = 0; i < 10; i++) key = key * 31 + f[i];
    uint32_t l0, l1;
    memcpy(&l0, &lod[0], 4), memcpy(&l1, &lod[1], 4);
    key = key * 31 + l0, key = key * 31 + l1;
    return sampler_for(g, key, &ci);
}

/* ---------------------------------------------------------------------- shaders -- */
static Shader *shader(Gpu *g, Obj *fn) {
    if (!fn || !fn->fn_bc) return NULL;
    for (Shader *s = g->shader_list; s; s = s->next)
        if (s->hash == fn->fn_hash) return s->ok ? s : NULL;
    Shader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->hash = fn->fn_hash;
    s->next = g->shader_list, g->shader_list = s;
    uint8_t *spv;
    size_t n;
    char *layout;
    if (shaders_get(g->shaders, fn->fn_bc, fn->fn_len, &spv, &n, &layout, &s->cdata, &s->clen)) return NULL;
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, (const uint32_t *)spv};
    s->ok = parse_layout(layout, &s->L) == 0 && vkCreateShaderModule(g->dev, &ci, NULL, &s->mod) == VK_SUCCESS;
    if (!s->ok) rlog("shader %s: no module", fn->fn_name);
    free(spv);
    free(layout);
    return s->ok ? s : NULL;
}

/* The functions of a render pipeline (serialized tag 0xe), found by scanning its description for refs
   to function objects: any 1-4 byte little-endian value at any offset that names one. A vertex and a
   fragment function are taken; the first of each found wins. */
static void scan_pipeline(Gpu *g, Obj *p) {
    if (p->scanned) return;
    p->scanned = 1;
    if (!p->ser || p->ser_tag != 0xe || p->ser_len < 16) return;
    uint32_t seen[64], nseen = 0;
    for (uint32_t i = 16; i + 1 <= p->ser_len; i++) {
        for (int width = 4; width >= 1; width /= 2) {
            if (i + (uint32_t)width > p->ser_len) continue;
            uint32_t v = 0;
            memcpy(&v, p->ser + i, (size_t)width);
            if (!v || v >= (1u << 20)) continue;
            int dup = 0;
            for (uint32_t k = 0; k < nseen; k++) dup |= seen[k] == v;
            if (dup || nseen == 64) continue;
            if (obj_type(g->os, p->task, v) != OBJ_FUNCTION) continue;
            seen[nseen++] = v;
            Obj *f = obj_get(g->os, p->task, v);
            if (!f) continue;
            p->fn_candidates++;
            if (f->fn_stage == 0 && !p->vs_ref) p->vs_ref = v;
            if (f->fn_stage == 1 && !p->fs_ref) p->fs_ref = v;
        }
    }
    if (p->fn_candidates > 2) rlog("pipeline %u: %u functions in its description; took vertex %u, fragment %u", p->ref, p->fn_candidates, p->vs_ref, p->fs_ref);
}

static VkFormat attr_format(int comps, int bits, char base) {
    static const VkFormat f32[3][4] = {{VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
                                       {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT},
                                       {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT}};
    static const VkFormat f16[3][4] = {{VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
                                       {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT},
                                       {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT}};
    int k = base == 'i' ? 1 : base == 'u' ? 2 : 0;
    if (comps < 1 || comps > 4) return VK_FORMAT_UNDEFINED;
    return bits == 16 ? f16[k][comps - 1] : f32[k][comps - 1];
}

static int topo_class(uint32_t prim) { return prim == 0 ? 0 : prim <= 2 ? 1 : 2; } /* points, lines, triangles */

static VkPipeline pipeline(Gpu *g, Shader *vs, Shader *fs, uint32_t prim, uint32_t *stride) {
    VkFormat colors[8], depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
    uint32_t ncolor = 0;
    for (int i = 0; i < 8; i++) {
        colors[i] = g->color[i].pi ? g->color[i].pi->fmt : VK_FORMAT_UNDEFINED;
        if (g->color[i].pi) ncolor = (uint32_t)i + 1;
    }
    if (g->depth.pi) {
        VkImageAspectFlags a = aspect_of(g->depth.pi->fmt);
        if (a & VK_IMAGE_ASPECT_DEPTH_BIT) depth = g->depth.pi->fmt;
        if (a & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = g->depth.pi->fmt;
    }
    int tc = topo_class(prim);
    /* Vertex attributes, packed in attribute order in one buffer. */
    VkVertexInputAttributeDescription attrs[16];
    uint32_t nattr = 0, off = 0;
    for (int i = 0; i < vs->L.nattr; i++) {
        VkFormat f = attr_format(vs->L.attr[i].comps, vs->L.attr[i].bits, vs->L.attr[i].base);
        if (!f) return VK_NULL_HANDLE;
        attrs[nattr++] = (VkVertexInputAttributeDescription){(uint32_t)vs->L.attr[i].index, 0, f, off};
        off += (uint32_t)(vs->L.attr[i].comps * vs->L.attr[i].bits / 8);
    }
    *stride = off;
    for (Pipe *p = g->pipes; p; p = p->next)
        if (p->vs == vs->hash && p->fs == (fs ? fs->hash : 0) && !memcmp(p->color, colors, sizeof colors) && p->depth == depth &&
            p->stencil == stencil && p->topo_class == tc)
            return p->p;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->mod, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs ? fs->mod : VK_NULL_HANDLE, "main", NULL}};
    VkVertexInputBindingDescription vb = {0, off, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nattr ? 1u : 0u, &vb, nattr, attrs};
    static const VkPrimitiveTopology tops[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, tops[tc], VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1;
    VkPipelineColorBlendAttachmentState blend[8];
    for (uint32_t i = 0; i < 8; i++) { /* the default: premultiplied source-over (WindowServer composites) */
        blend[i] = (VkPipelineColorBlendAttachmentState){VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                                         VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xf};
        VkFormat f = colors[i];
        if (f == VK_FORMAT_UNDEFINED || f == VK_FORMAT_R8_UINT || f == VK_FORMAT_R8_SINT || f == VK_FORMAT_R16_UINT || f == VK_FORMAT_R16_SINT ||
            f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32_SINT || f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R16G16B16A16_UINT ||
            f == VK_FORMAT_R32G32B32A32_UINT || f == VK_FORMAT_R32G32_UINT || f == VK_FORMAT_R16G16_UINT || f == VK_FORMAT_R8G8_UINT ||
            f == VK_FORMAT_A2B10G10R10_UINT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 || f == VK_FORMAT_R8G8B8A8_SINT ||
            f == VK_FORMAT_R16G16B16A16_SINT || f == VK_FORMAT_R32G32B32A32_SINT || f == VK_FORMAT_R32G32_SINT || f == VK_FORMAT_R16G16_SINT ||
            f == VK_FORMAT_R8G8_SINT)
            blend[i].blendEnable = VK_FALSE;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, ncolor, blend};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, ncolor, colors, depth, stencil};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, fs ? 2u : 1u, stages, &vi, &ia, NULL, &vp, &rs, &ms,
                                        &ds, &cb, &dy, g->layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline out;
    if (vkCreateGraphicsPipelines(g->dev, VK_NULL_HANDLE, 1, &pci, NULL, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    Pipe *p = calloc(1, sizeof *p);
    if (!p) { vkDestroyPipeline(g->dev, out, NULL); return VK_NULL_HANDLE; }
    p->vs = vs->hash, p->fs = fs ? fs->hash : 0;
    memcpy(p->color, colors, sizeof colors);
    p->depth = depth, p->stencil = stencil, p->topo_class = tc, p->nattr = (int)nattr, p->p = out;
    p->next = g->pipes, g->pipes = p;
    g->npipes++;
    return out;
}

/* ------------------------------------------------------------------------ passes -- */
#define R32(o) ({ uint32_t _v = 0; if ((o) + 4 <= size) memcpy(&_v, rec + (o), 4); _v; })
#define R16(o) ({ uint16_t _v = 0; if ((o) + 2 <= size) memcpy(&_v, rec + (o), 2); _v; })
#define R64(o) ({ uint64_t _v = 0; if ((o) + 8 <= size) memcpy(&_v, rec + (o), 8); _v; })

static void read_attach(Gpu *g, Attach *a, const uint8_t *rec, uint32_t size, uint32_t o) {
    memset(a, 0, sizeof *a);
    uint32_t ref = R32(o), resolve = R32(o + 4);
    a->level = R16(o + 8), a->slice = R16(o + 10), a->load = R16(o + 20), a->store = R16(o + 22);
    Obj *t = ref ? obj_get(g->os, g->task, ref) : NULL, *r = resolve ? obj_get(g->os, g->task, resolve) : NULL;
    if (r && r->has_tex && (a->store == 2 || a->store == 3)) { /* draw straight into the resolve texture: one sample */
        t = r;
        a->level = R16(o + 14), a->slice = R16(o + 16);
        a->store = 1;
    }
    a->tex = t && t->has_tex ? t : NULL;
}

void gpu_pass_begin(Gpu *g, uint32_t task, const uint8_t *rec, uint32_t size) {
    if (!g || g->lost) return;
    g->open = 1, g->running = 0, g->task = task;
    g->pass_size = size < sizeof g->pass_rec ? size : (uint32_t)sizeof g->pass_rec;
    memcpy(g->pass_rec, rec, g->pass_size);
    memset(g->st, 0, sizeof g->st);
    g->pipeline = g->dss = NULL;
    g->cull = 0, g->winding = 0, g->stencil_ref[0] = g->stencil_ref[1] = 0;
    g->have_viewport = g->have_scissor = 0;
}

/* The pass goes onto the GPU at its first draw: attachments created, loaded or cleared, rendering begun. */
static int start(Gpu *g) {
    const uint8_t *rec = g->pass_rec;
    uint32_t size = g->pass_size;
    vkResetCommandBuffer(g->pre, 0), vkResetCommandBuffer(g->main, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g->pre, &bi), vkBeginCommandBuffer(g->main, &bi);
    g->arena_used = 0, g->ncopies = 0;
    vkResetDescriptorPool(g->dev, g->dpool, 0);
    g->width = (uint32_t)R64(0x238), g->height = (uint32_t)R64(0x240);
    for (int i = 0; i < 8; i++) {
        uint32_t o = 0x4c + (uint32_t)i * 0x3c;
        if (o + 0x3c > size) { memset(&g->color[i], 0, sizeof g->color[i]); continue; }
        read_attach(g, &g->color[i], rec, size, o);
        memcpy(g->color[i].clear, rec + o + 0x1c, 32);
    }
    read_attach(g, &g->depth, rec, size, 0);
    double clear_depth;
    memcpy(&clear_depth, rec + 0x1c, 8);
    g->depth.clear[0] = clear_depth;
    if (!g->depth.tex) { /* a stencil-only pass */
        read_attach(g, &g->depth, rec, size, 0x28);
        g->depth.clear[0] = 1;
    }
    g->depth.clear[1] = R32(0x44);
    VkRenderingAttachmentInfo ca[8];
    uint32_t ncolor = 0;
    for (int i = 0; i < 8; i++) {
        Attach *a = &g->color[i];
        memset(&ca[i], 0, sizeof ca[i]);
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!a->tex) continue;
        TexView *t = &a->tex->tex;
        VkFormat f = vk_format(t->format);
        uint32_t w = t->w >> a->level ? t->w >> a->level : 1, h = t->h >> a->level ? t->h >> a->level : 1;
        if (!f || is_depth(f)) { a->tex = NULL; continue; }
        if (!g->width) g->width = w, g->height = h;
        obj_refresh(g->os, a->tex);
        a->pi = make_image(g, a->tex, f, w, h, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D);
        if (!a->pi) { a->tex = NULL; continue; }
        barrier(g->pre, a->pi->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (a->load == 1) upload(g, g->pre, a->pi, t, a->level, a->slice, 0, 0);
        barrier(g->pre, a->pi->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca[i].imageView = a->pi->view, ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = a->load == 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : a->load == 1 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        for (int c = 0; c < 4; c++) ca[i].clearValue.color.float32[c] = (float)a->clear[c];
        ncolor = (uint32_t)i + 1;
    }
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    int has_depth = 0, has_stencil = 0;
    if (g->depth.tex) {
        /* Depth and stencil live on the GPU for the pass only: never read back (nothing shows them). */
        TexView *t = &g->depth.tex->tex;
        VkFormat f = vk_format(t->format);
        uint32_t w = t->w >> g->depth.level ? t->w >> g->depth.level : 1, h = t->h >> g->depth.level ? t->h >> g->depth.level : 1;
        if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT) f = VK_FORMAT_D32_SFLOAT_S8_UINT; /* what every device has */
        g->depth.pi = is_depth(f) ? make_image(g, g->depth.tex, f, w, h, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D) : NULL;
        if (g->depth.pi) {
            VkImageAspectFlags asp = aspect_of(f);
            barrier(g->pre, g->depth.pi->img, asp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.imageView = sa.imageView = g->depth.pi->view;
            da.imageLayout = sa.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = sa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.clearValue.depthStencil.depth = (float)g->depth.clear[0];
            sa.clearValue.depthStencil.stencil = (uint32_t)g->depth.clear[1];
            has_depth = (asp & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, has_stencil = (asp & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        }
    }
    if (!ncolor && !g->depth.pi) return -1;
    if (!g->width || !g->height) return -1;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {g->width, g->height}}, 1, 0, ncolor, ca,
                          has_depth ? &da : NULL, has_stencil ? &sa : NULL};
    vkCmdBeginRendering(g->main, &ri);
    g->running = 1;
    return 0;
}

/* The push block and descriptors of one stage for a draw. */
static int bind_stage(Gpu *g, Shader *s, int stage, uint64_t push[32], VkDescriptorSet *set) {
    StageBind *b = &g->st[stage];
    Layout *L = &s->L;
    if (L->nimg || L->nfbf) return -1;
    for (int i = 0; i < L->npush; i++) {
        int slot = L->push[i].slot;
        if (slot < 0 || slot >= 32) return -1;
        if (L->push[i].kind == PUSH_CBUF) {
            int m = L->push[i].metal;
            Obj *o = m >= 0 && m < 31 ? b->buf[m].o : NULL;
            int64_t at = o ? arena_buffer(g, o, 0) : -1;
            push[slot] = at < 0 ? 0 : g->arena_addr + (uint64_t)at + b->buf[m].off;
        } else if (L->push[i].kind == PUSH_TABLE) {
            int64_t t = arena_alloc(g, 512, 64);
            if (t < 0) return -1;
            memset(g->arena_ptr + t, 0, 512);
            for (int k = 0; k < L->ndbuf; k++) {
                int m = L->dbuf[k].metal, off = L->dbuf[k].off;
                Obj *o = m >= 0 && m < 31 ? b->buf[m].o : NULL;
                int64_t at = o ? arena_buffer(g, o, L->dbuf[k].writes) : -1;
                uint64_t addr = at < 0 ? 0 : g->arena_addr + (uint64_t)at + b->buf[m].off;
                if (off >= 0 && off + 8 <= 512) memcpy(g->arena_ptr + t + off, &addr, 8);
            }
            push[slot] = g->arena_addr + (uint64_t)t;
        } else {
            int64_t at = s->clen ? arena_alloc(g, s->clen, 64) : -1;
            if (at >= 0) memcpy(g->arena_ptr + at, s->cdata, s->clen);
            push[slot] = at < 0 ? 0 : g->arena_addr + (uint64_t)at;
        }
    }
    if (!L->ntex && !L->nsmp) { *set = VK_NULL_HANDLE; return 0; }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g->dpool, 1, &g->set_layout[stage == 0 ? 1 : 0]};
    if (vkAllocateDescriptorSets(g->dev, &ai, set) != VK_SUCCESS) return -1;
    VkWriteDescriptorSet w[32];
    VkDescriptorImageInfo info[32];
    uint32_t n = 0;
    for (int i = 0; i < L->ntex && n < 32; i++) {
        int m = L->tex[i].metal;
        PassImage *pi = sampled(g, m >= 0 && m < 32 ? b->tex[m] : NULL, L->tex[i].dim, L->tex[i].array);
        if (!pi) return -1;
        info[n] = (VkDescriptorImageInfo){VK_NULL_HANDLE, pi->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, *set, (uint32_t)L->tex[i].binding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &info[n], NULL, NULL};
        n++;
    }
    for (int i = 0; i < L->nsmp && n < 32; i++) {
        int m = L->smp[i].metal;
        VkSampler smp = L->smp[i].cs ? constexpr_sampler(g, L->smp[i].f, L->smp[i].lod) : api_sampler(g, m >= 0 && m < 16 ? b->smp[m] : NULL);
        info[n] = (VkDescriptorImageInfo){smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, *set, (uint32_t)L->smp[i].binding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &info[n], NULL, NULL};
        n++;
    }
    vkUpdateDescriptorSets(g->dev, n, w, 0, NULL);
    return 0;
}

static void draw(Gpu *g, uint32_t prim, uint64_t start_v, uint64_t count, uint64_t instances, uint64_t base_instance,
                 Obj *index_buf, uint64_t index_off, uint32_t index_type) {
    if (!g->pipeline) { skip(g, "no pipeline"); return; }
    if (!count || !instances) return;
    if (prim > 4) { skip(g, "primitive type"); return; }
    if (!g->running && start(g)) { skip(g, "pass has no attachment the GPU can draw into"); g->open = 0; return; }
    scan_pipeline(g, g->pipeline);
    if (!g->pipeline->vs_ref) { skip(g, g->pipeline->ser_tag == 0xe ? "no vertex function found in the pipeline" : "not a render pipeline"); return; }
    Shader *vs = shader(g, obj_get(g->os, g->task, g->pipeline->vs_ref));
    Shader *fs = g->pipeline->fs_ref ? shader(g, obj_get(g->os, g->task, g->pipeline->fs_ref)) : NULL;
    if (!vs || (g->pipeline->fs_ref && !fs)) { skip(g, "a function air2spv refused"); return; }
    uint32_t stride = 0;
    VkPipeline p = pipeline(g, vs, fs, prim, &stride);
    if (!p) { skip(g, "pipeline creation failed"); return; }
    uint64_t push[32] = {0};
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (bind_stage(g, vs, 0, push, &sets[1]) || (fs && bind_stage(g, fs, 1, push, &sets[0]))) {
        skip(g, "a draw that reads the framebuffer or writes a texture");
        return;
    }
    VkCommandBuffer cb = g->main;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    for (uint32_t s = 0; s < 2; s++)
        if (sets[s]) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, s, 1, &sets[s], 0, NULL);
    vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 256, push);
    /* Metal's clip space has +y up: the viewport is flipped. */
    VkViewport v = g->have_viewport ? g->viewport : (VkViewport){0, 0, (float)g->width, (float)g->height, 0, 1};
    VkViewport flipped = {v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth};
    vkCmdSetViewport(cb, 0, 1, &flipped);
    VkRect2D sc = g->have_scissor ? g->scissor : (VkRect2D){{0, 0}, {g->width, g->height}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetCullMode(cb, g->cull == 1 ? VK_CULL_MODE_FRONT_BIT : g->cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, g->winding ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    static const VkPrimitiveTopology tops[5] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    vkCmdSetPrimitiveTopology(cb, tops[prim]);
    /* Depth and stencil: {tag, size, ref, flags, front {ops, read, write}, back {...}}, as devices/pvg_metal.m reads it. */
    uint32_t f = 0, sides[2][3] = {{0}};
    if (g->dss && g->dss->ser_tag == 4 && g->dss->ser_len >= 40) {
        memcpy(&f, g->dss->ser + 12, 4);
        memcpy(sides, g->dss->ser + 16, 24);
    }
    int depth_on = g->depth.pi && (aspect_of(g->depth.pi->fmt) & VK_IMAGE_ASPECT_DEPTH_BIT);
    vkCmdSetDepthTestEnable(cb, depth_on && (f & 7) != 7);
    vkCmdSetDepthWriteEnable(cb, depth_on && ((f >> 3) & 1));
    vkCmdSetDepthCompareOp(cb, (VkCompareOp)(f & 7));
    int stencil_on = g->depth.pi && (aspect_of(g->depth.pi->fmt) & VK_IMAGE_ASPECT_STENCIL_BIT) && (f & 0x30);
    vkCmdSetStencilTestEnable(cb, stencil_on);
    static const VkStencilOp ops[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
    for (int s = 0; s < 2; s++) {
        VkStencilFaceFlags face = s ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        uint32_t o = ((f >> (4 + s)) & 1) ? sides[s][0] : 7; /* an unset side: always pass, keep */
        vkCmdSetStencilOp(cb, face, ops[(o >> 3) & 7], ops[(o >> 9) & 7], ops[(o >> 6) & 7], (VkCompareOp)(o & 7));
        vkCmdSetStencilCompareMask(cb, face, ((f >> (4 + s)) & 1) ? sides[s][1] : 0xff);
        vkCmdSetStencilWriteMask(cb, face, ((f >> (4 + s)) & 1) ? sides[s][2] : 0);
        vkCmdSetStencilReference(cb, face, g->stencil_ref[s]);
    }
    if (vs->L.nattr) {
        /* The vertex buffer: the lowest bound Metal buffer the shader does not read as a buffer argument. */
        int pick = -1;
        for (int m = 0; m < 31 && pick < 0; m++) {
            if (!g->st[0].buf[m].o) continue;
            int used = 0;
            for (int i = 0; i < vs->L.npush; i++) used |= vs->L.push[i].kind == PUSH_CBUF && vs->L.push[i].metal == m;
            for (int i = 0; i < vs->L.ndbuf; i++) used |= vs->L.dbuf[i].metal == m;
            if (!used) pick = m;
        }
        int64_t at = pick >= 0 ? arena_buffer(g, g->st[0].buf[pick].o, 0) : -1;
        if (at < 0) { skip(g, "no vertex buffer for the vertex attributes"); return; }
        VkDeviceSize off = (VkDeviceSize)at + g->st[0].buf[pick].off;
        vkCmdBindVertexBuffers(cb, 0, 1, &g->arena, &off);
    }
    if (index_buf) {
        int64_t at = arena_buffer(g, index_buf, 0);
        if (at < 0) { skip(g, "index buffer"); return; }
        vkCmdBindIndexBuffer(cb, g->arena, (VkDeviceSize)at + index_off, index_type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, (uint32_t)count, (uint32_t)instances, 0, 0, (uint32_t)base_instance);
    } else
        vkCmdDraw(cb, (uint32_t)count, (uint32_t)instances, (uint32_t)start_v, (uint32_t)base_instance);
    g->draws++;
}

static void set_buffers(Gpu *g, StageBind *b, const uint8_t *rec, uint32_t size, uint32_t entry) {
    uint32_t index = R32(0), count = R32(4);
    if (count > 31 || index + count > 31 || size < 8 + count * entry) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ref = R32(8 + i * entry);
        b->buf[index + i].o = ref ? obj_get(g->os, g->task, ref) : NULL;
        b->buf[index + i].off = R64(12 + i * entry);
    }
}

static void set_refs(Gpu *g, Obj **slots, uint32_t max, const uint8_t *rec, uint32_t size) {
    uint32_t index = R32(0), count = R32(4);
    if (index >= max || count > max - index || size < 8 + count * 4) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ref = R32(8 + i * 4);
        slots[index + i] = ref ? obj_get(g->os, g->task, ref) : NULL;
    }
}

int gpu_record(Gpu *g, uint32_t tag, const uint8_t *rec, uint32_t size) {
    if (!g || !g->open) return 0;
    switch (tag) {
    case 116: g->pipeline = R32(0) ? obj_get(g->os, g->task, R32(0)) : NULL; return 1;
    case 125: set_buffers(g, &g->st[0], rec, size, 12); return 1;
    case 165: set_buffers(g, &g->st[0], rec, size, 20); return 1;
    case 110: set_buffers(g, &g->st[1], rec, size, 12); return 1;
    case 126: case 166: if (R32(0) < 31) g->st[0].buf[R32(0)].off = R64(4); return 1;
    case 111: if (R32(0) < 31) g->st[1].buf[R32(0)].off = R64(4); return 1;
    case 114: set_refs(g, g->st[1].tex, 32, rec, size); return 1;
    case 129: set_refs(g, g->st[0].tex, 32, rec, size); return 1;
    case 112: case 113: set_refs(g, g->st[1].smp, 16, rec, size); return 1;
    case 127: case 128: set_refs(g, g->st[0].smp, 16, rec, size); return 1;
    case 104: g->dss = R32(0) ? obj_get(g->os, g->task, R32(0)) : NULL; return 1;
    case 107: g->cull = R32(0); return 1;
    case 115: g->winding = R32(0); return 1;
    case 119: g->stencil_ref[0] = R32(0), g->stencil_ref[1] = R32(4); return 1;
    case 130: case 131: { /* MTLViewport: six doubles (the first, for several) */
        uint32_t head = tag == 131 ? size % 48 : 0;
        if (size < head + 48) return 1;
        double v[6];
        memcpy(v, rec + head, sizeof v);
        g->viewport = (VkViewport){(float)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4], (float)v[5]};
        g->have_viewport = 1;
        return 1;
    }
    case 117: case 118: { /* MTLScissorRect: four NSUInteger */
        uint32_t head = tag == 118 ? size % 32 : 0;
        if (size < head + 32) return 1;
        uint64_t s[4];
        memcpy(s, rec + head, sizeof s);
        g->scissor = (VkRect2D){{(int32_t)s[0], (int32_t)s[1]}, {(uint32_t)s[2], (uint32_t)s[3]}};
        g->have_scissor = 1;
        return 1;
    }
    case 0: draw(g, R32(0), R64(4), R64(12), 1, 0, NULL, 0, 0); return 1;
    case 1: draw(g, R32(0), R16(4), R16(6), 1, 0, NULL, 0, 0); return 1;
    case 2: draw(g, R16(24), R64(0), R64(8), R64(16), 0, NULL, 0, 0); return 1;
    case 3: draw(g, R16(6), R16(0), R16(2), R16(4), 0, NULL, 0, 0); return 1;
    case 4: draw(g, R16(32), R64(0), R64(8), R64(16), R64(24), NULL, 0, 0); return 1;
    case 5: draw(g, R16(8), R16(0), R16(2), R16(4), R16(6), NULL, 0, 0); return 1;
    case 6: draw(g, R16(0), 0, R64(8), 1, 0, obj_get(g->os, g->task, R32(4)), R64(16), R16(2)); return 1;
    case 7: draw(g, R16(0), 0, R16(8), 1, 0, obj_get(g->os, g->task, R32(4)), R16(10), R16(2)); return 1;
    case 8: draw(g, R16(0), 0, R64(8), R64(24), 0, obj_get(g->os, g->task, R32(4)), R64(16), R16(2)); return 1;
    case 9: draw(g, R16(0), 0, R16(8), R16(12), 0, obj_get(g->os, g->task, R32(4)), R16(10), R16(2)); return 1;
    default:
        if (tag <= 19 || tag == 28 || tag == 29 || tag == 155 || tag == 162) { skip(g, "tessellation or indirect draw"); return 1; }
        return 0;
    }
}

/* Reads back what the pass stores. */
static void finish(Gpu *g) {
    vkCmdEndRendering(g->main);
    int64_t back_at[8];
    for (int i = 0; i < 8; i++) {
        Attach *a = &g->color[i];
        back_at[i] = -1;
        if (!a->pi || (a->store != 1 && a->store != 3)) continue;
        unsigned bpp = px_bytes(a->tex->tex.format);
        uint32_t w = a->tex->tex.w >> a->level ? a->tex->tex.w >> a->level : 1, h = a->tex->tex.h >> a->level ? a->tex->tex.h >> a->level : 1;
        back_at[i] = arena_alloc(g, (uint64_t)w * h * bpp, 16);
        if (back_at[i] < 0) continue;
        barrier(g->main, a->pi->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {(VkDeviceSize)back_at[i], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(g->main, a->pi->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->arena, 1, &c);
    }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(g->main, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(g->pre), vkEndCommandBuffer(g->main);
    VkCommandBuffer cbs[2] = {g->pre, g->main};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, cbs, 0, NULL};
    vkResetFences(g->dev, 1, &g->fence);
    VkResult waited = VK_ERROR_UNKNOWN;
    if (vkQueueSubmit(g->queue, 1, &si, g->fence) == VK_SUCCESS &&
        (waited = vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, 2000000000ull)) != VK_SUCCESS) {
        /* A translated shader that never finishes (or a lost device): nothing is read back into guest
           memory, and draws stay off rather than hang every pass after it. */
        rlog("pass: the GPU %s; draws off for this run", waited == VK_TIMEOUT ? "did not finish it in 2 s" : "was lost");
        g->lost = 1;
        free_pass_images(g);
        return;
    }
    if (waited == VK_SUCCESS) {
        for (int i = 0; i < 8; i++) {
            Attach *a = &g->color[i];
            if (back_at[i] < 0) continue;
            TexView *t = &a->tex->tex;
            unsigned bpp = px_bytes(t->format);
            uint32_t w = t->w >> a->level ? t->w >> a->level : 1, h = t->h >> a->level ? t->h >> a->level : 1;
            uint64_t line = (uint64_t)w * bpp;
            for (uint32_t y = 0; y < h; y++) {
                uint8_t *row = texel_row(t, a->level, a->slice, y, line);
                if (row) memcpy(row, g->arena_ptr + back_at[i] + y * line, line);
            }
        }
        for (int i = 0; i < g->ncopies; i++) { /* what the shaders wrote to device buffers */
            ArenaCopy *c = &g->copies[i];
            uint64_t len = 0;
            uint8_t *bytes = c->write_back ? obj_bytes(c->o, &len) : NULL;
            if (bytes) memcpy(bytes, g->arena_ptr + c->at, len < c->len ? len : c->len);
        }
        g->gpu_passes++;
    } else rlog("pass: submit failed");
    free_pass_images(g);
}

int gpu_pass_end(Gpu *g) {
    if (!g || !g->open) return 0;
    g->open = 0;
    if (!g->running) return 0;
    g->running = 0;
    finish(g);
    return 1;
}
