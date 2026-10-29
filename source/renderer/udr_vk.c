/* udr_vk.c - UD renderer Vulkan backend: executes the IR (udr_ir.h). Ours; independent of E6.
 *
 * Model:
 *  - resources by identity (UdrId), created and destroyed by IR records; images live in VK_IMAGE_LAYOUT_GENERAL;
 *  - pipelines fully described by the IR (no state guessed from registers); one descriptor set per draw / dispatch
 *    from a pool that is recycled after each submit;
 *  - ordering: UDR_OP_BARRIER records become pipeline barriers. The backend adds barriers only around its own uploads
 *    and readbacks. It tracks, per resource, whether it was written since the last barrier and reports a read of such
 *    data as a hazard (and, with UDR_CFG_HAZARD_BARRIER, inserts a barrier there);
 *  - work is recorded into one command buffer and submitted at READBACK / FLUSH / udr_finish (synchronous in v1).
 * No C library: memory and logging come from UdrHost. */
#include "udr.h"
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan_core.h"

typedef udr_u8 u8; typedef udr_u32 u32; typedef udr_u64 u64; typedef udr_i32 i32; typedef long long i64; typedef udr_f32 f32;

/* ---------------------------------------------------------------- small helpers (no libc) */
static void m_copy(void *d, const void *s, u64 n) { u8 *a = (u8 *)d; const u8 *b = (const u8 *)s; while (n--) *a++ = *b++; }
static void m_zero(void *d, u64 n) { u8 *a = (u8 *)d; while (n--) *a++ = 0; }
typedef struct { char *b; u32 n, cap; } Str;
static void s_put(Str *s, const char *t) { while (*t && s->n + 1 < s->cap) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void s_dec(Str *s, u64 v) { char t[24]; int i = 23; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); s_put(s, t + i); }
static void s_hex(Str *s, u64 v) { char t[20]; int i = 19; t[i] = 0; do { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); t[--i] = 'x'; t[--i] = '0'; s_put(s, t + i); }

/* ---------------------------------------------------------------- Vulkan entry points */
#define UDR_VK_INST(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFormatProperties)
#define UDR_VK_DEV(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBindIndexBuffer) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) \
    X(vkCmdFillBuffer) X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering)
#define UDR_DECL(f) static PFN_##f f;
UDR_VK_INST(UDR_DECL) UDR_VK_DEV(UDR_DECL)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr; static PFN_vkCreateInstance vkCreateInstance; static PFN_vkDestroyInstance vkDestroyInstance_;
/* presentation (optional: only when the host gives a window) */
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *hinstance; void *hwnd; } UdrWin32SurfaceCreateInfo;
typedef VkResult (VKAPI_PTR *UdrPfnCreateWin32Surface)(VkInstance, const UdrWin32SurfaceCreateInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
static UdrPfnCreateWin32Surface vkCreateWin32SurfaceKHR_;
#define UDR_VK_INST_PRESENT(X) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define UDR_VK_DEV_PRESENT(X) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkCmdBlitImage)
UDR_VK_INST_PRESENT(UDR_DECL) UDR_VK_DEV_PRESENT(UDR_DECL)

/* ---------------------------------------------------------------- formats */
typedef struct { VkFormat vk; u32 bytes, block; VkImageAspectFlags aspect; } FmtInfo;
static const FmtInfo g_fmt[UDR_FMT_COUNT] = {
    [UDR_FMT_UNDEFINED] = {VK_FORMAT_UNDEFINED, 0, 1, 0},
    [UDR_FMT_R8_UNORM] = {VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8_UNORM] = {VK_FORMAT_R8G8_UNORM, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8B8A8_UNORM] = {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8B8A8_SRGB] = {VK_FORMAT_R8G8B8A8_SRGB, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_B8G8R8A8_UNORM] = {VK_FORMAT_B8G8R8A8_UNORM, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_B8G8R8A8_SRGB] = {VK_FORMAT_B8G8R8A8_SRGB, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16_FLOAT] = {VK_FORMAT_R16_SFLOAT, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16_FLOAT] = {VK_FORMAT_R16G16_SFLOAT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16B16A16_FLOAT] = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16_UNORM] = {VK_FORMAT_R16_UNORM, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16_UNORM] = {VK_FORMAT_R16G16_UNORM, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16B16A16_UNORM] = {VK_FORMAT_R16G16B16A16_UNORM, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32_FLOAT] = {VK_FORMAT_R32_SFLOAT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32_FLOAT] = {VK_FORMAT_R32G32_SFLOAT, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32B32A32_FLOAT] = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32_UINT] = {VK_FORMAT_R32_UINT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32_UINT] = {VK_FORMAT_R32G32_UINT, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32B32A32_UINT] = {VK_FORMAT_R32G32B32A32_UINT, 16, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R11G11B10_FLOAT] = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_A2B10G10R10_UNORM] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R5G6B5_UNORM] = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_D16_UNORM] = {VK_FORMAT_D16_UNORM, 2, 1, VK_IMAGE_ASPECT_DEPTH_BIT},
    [UDR_FMT_D32_FLOAT] = {VK_FORMAT_D32_SFLOAT, 4, 1, VK_IMAGE_ASPECT_DEPTH_BIT},
    [UDR_FMT_D32_FLOAT_S8_UINT] = {VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
    [UDR_FMT_BC1_UNORM] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC1_SRGB] = {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC2_UNORM] = {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC2_SRGB] = {VK_FORMAT_BC2_SRGB_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC3_UNORM] = {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC3_SRGB] = {VK_FORMAT_BC3_SRGB_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC4_UNORM] = {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC5_UNORM] = {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC6H_UFLOAT] = {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC7_UNORM] = {VK_FORMAT_BC7_UNORM_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC7_SRGB] = {VK_FORMAT_BC7_SRGB_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8_SNORM] = {VK_FORMAT_R8_SNORM, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8_SNORM] = {VK_FORMAT_R8G8_SNORM, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8B8A8_SNORM] = {VK_FORMAT_R8G8B8A8_SNORM, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16_SNORM] = {VK_FORMAT_R16_SNORM, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16_SNORM] = {VK_FORMAT_R16G16_SNORM, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16B16A16_SNORM] = {VK_FORMAT_R16G16B16A16_SNORM, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8_UINT] = {VK_FORMAT_R8_UINT, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8_UINT] = {VK_FORMAT_R8G8_UINT, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8B8A8_UINT] = {VK_FORMAT_R8G8B8A8_UINT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16_UINT] = {VK_FORMAT_R16_UINT, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16_UINT] = {VK_FORMAT_R16G16_UINT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16B16A16_UINT] = {VK_FORMAT_R16G16B16A16_UINT, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8_SINT] = {VK_FORMAT_R8_SINT, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8_SINT] = {VK_FORMAT_R8G8_SINT, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R8G8B8A8_SINT] = {VK_FORMAT_R8G8B8A8_SINT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16_SINT] = {VK_FORMAT_R16_SINT, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16_SINT] = {VK_FORMAT_R16G16_SINT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R16G16B16A16_SINT] = {VK_FORMAT_R16G16B16A16_SINT, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32_SINT] = {VK_FORMAT_R32_SINT, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32_SINT] = {VK_FORMAT_R32G32_SINT, 8, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32B32A32_SINT] = {VK_FORMAT_R32G32B32A32_SINT, 16, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R32G32B32_FLOAT] = {VK_FORMAT_R32G32B32_SFLOAT, 12, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_A2B10G10R10_UINT] = {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_A1R5G5B5_UNORM] = {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_R4G4B4A4_UNORM] = {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC4_SNORM] = {VK_FORMAT_BC4_SNORM_BLOCK, 8, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC5_SNORM] = {VK_FORMAT_BC5_SNORM_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_BC6H_SFLOAT] = {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    [UDR_FMT_D16_UNORM_S8_UINT] = {VK_FORMAT_D16_UNORM_S8_UINT, 2, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
};
udr_u32 udr_format_bytes(udr_u32 f) { return f < UDR_FMT_COUNT ? g_fmt[f].bytes : 0; }
udr_u32 udr_format_vk(udr_u32 f) { return f < UDR_FMT_COUNT ? (udr_u32)g_fmt[f].vk : 0; }   /* the VkFormat of an IR format (test tools) */

/* ---------------------------------------------------------------- objects */
enum { R_NONE = 0, R_IMAGE, R_BUFFER, R_SAMPLER, R_SHADER, R_PIPELINE };
typedef struct { u64 key; u32 swz; VkImageView view; } ViewEnt;
typedef struct {
    u32 kind;
    /* image */
    VkImage img; VkDeviceMemory mem; u32 fmt, type, w, h, d, layers, mips, usage; VkImageAspectFlags aspect;
    ViewEnt views[8]; u32 nviews;
    /* buffer */
    VkBuffer buf; u64 size;
    /* sampler / shader */
    VkSampler smp; VkShaderModule mod; u32 stage;
    /* pipeline */
    VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; int compute; u32 push_bytes, nbind; UdrBindingDesc bind[UDR_MAX_BINDINGS];
    u32 ncolor; u32 color_format[UDR_MAX_TARGETS]; u32 depth_format;
    /* hazard tracking: last write since the last barrier */
    u64 wr_epoch; u32 wr_cmd, wr_op; int wr_atomic_only;
    /* placement in a memory pool block (pblock = block index + 1; 0 = none) */
    u32 pblock; u64 poff, psize;
} Res;
typedef struct { u64 off, size; } FreeRange;

struct UdrCtx {
    UdrHost h; UdrConfig cfg; UdrStats st;
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; u32 qfam;
    VkPhysicalDeviceMemoryProperties mp; char dev_name[256];
    int aniso; float max_aniso;   /* samplerAnisotropy enabled, and the device's maxSamplerAnisotropy */
    int bias_clamp_ok;            /* depthBiasClamp enabled */
    u64 max_storage_range;        /* maxStorageBufferRange */
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; int recording;
    VkDescriptorPool dpool; u32 dsets_in_pool;
    VkBuffer stage; VkDeviceMemory stage_mem; u8 *stage_map; u64 stage_size, stage_used;
    Res *res; u32 nres;
    /* device memory pool: resources are placed in large blocks (one allocation per resource hit the drivers' limit of
       about 4,000 allocations with the many small buffers of a captured frame). Freed ranges go to a sorted, coalesced
       free list per block and are reused first-fit. */
    struct { VkDeviceMemory mem; u64 size, used; u32 type; FreeRange *fr; u32 nfr, cfr; } mpool[256]; u32 nmpool;
    /* DESTROY while commands are recorded: the objects are freed after that submission completes */
    Res *dead; u32 ndead, cdead; u64 dead_bytes;
    /* command state */
    u32 cmd; u64 epoch;
    int in_pass; UdrBeginPass pass;
    UdrId cur_pipe; int pipe_bound;
    UdrBinding binds[UDR_MAX_BINDINGS]; u64 bind_valid; int binds_dirty;
    u8 push[128]; int push_dirty;
    int have_viewport, have_scissor;
    UdrHazard haz[64]; u32 nhaz;
    char err[256];
    /* presentation (udr_present) */
    VkSurfaceKHR surf; VkSwapchainKHR sw; VkFormat sw_fmt; VkExtent2D sw_ext; VkImage sw_img[8]; u32 sw_n; VkFence acq_fence; int present_ok;
};

static void logs(UdrCtx *c, const char *t) { if (c->h.log) c->h.log(t); }
static void fail(UdrCtx *c, const char *what, i32 vkr, u64 extra) {
    Str s = {c->err, 0, sizeof c->err}; s_put(&s, "udr: "); s_put(&s, what);
    if (vkr) { s_put(&s, " (VkResult "); if (vkr < 0) s_put(&s, "-"); s_dec(&s, (u64)(vkr < 0 ? -(i64)vkr : (i64)vkr)); s_put(&s, ")"); }
    if (extra) { s_put(&s, " "); s_hex(&s, extra); }
    s_put(&s, "\n"); logs(c, c->err); c->st.errors++;
}
static Res *res_get(UdrCtx *c, UdrId id, u32 kind) {
    if (!id || id >= c->nres || c->res[id].kind != kind) return 0;
    return &c->res[id];
}
static Res *res_new(UdrCtx *c, UdrId id, u32 kind) {
    if (!id || id > 0xFFFFFF) { fail(c, "resource id out of range", 0, id); return 0; }
    if (id >= c->nres) {
        u32 n = c->nres ? c->nres : 1024; while (n <= id) n *= 2;
        Res *r = (Res *)c->h.alloc((u64)n * sizeof(Res)); if (!r) { fail(c, "out of memory", 0, 0); return 0; }
        if (c->res) { m_copy(r, c->res, (u64)c->nres * sizeof(Res)); c->h.free(c->res); }
        c->res = r; c->nres = n;
    }
    if (c->res[id].kind) { fail(c, "resource id already in use", 0, id); return 0; }
    m_zero(&c->res[id], sizeof(Res)); c->res[id].kind = kind; return &c->res[id];
}
static int mem_type(UdrCtx *c, u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < c->mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (c->mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
#define POOL_BLOCK (64ull << 20)
static int fr_insert(UdrCtx *c, u32 b, u32 at, u64 off, u64 size) {
    if (c->mpool[b].nfr == c->mpool[b].cfr) {
        u32 n = c->mpool[b].cfr ? c->mpool[b].cfr * 2 : 256;
        FreeRange *f = (FreeRange *)c->h.alloc((u64)n * sizeof(FreeRange)); if (!f) return 0;
        if (c->mpool[b].fr) { m_copy(f, c->mpool[b].fr, (u64)c->mpool[b].nfr * sizeof(FreeRange)); c->h.free(c->mpool[b].fr); }
        c->mpool[b].fr = f; c->mpool[b].cfr = n;
    }
    FreeRange *f = c->mpool[b].fr;
    for (u32 j = c->mpool[b].nfr; j > at; j--) f[j] = f[j - 1];
    f[at].off = off; f[at].size = size; c->mpool[b].nfr++;
    return 1;
}
static void fr_remove(UdrCtx *c, u32 b, u32 at) {
    FreeRange *f = c->mpool[b].fr;
    for (u32 j = at; j + 1 < c->mpool[b].nfr; j++) f[j] = f[j + 1];
    c->mpool[b].nfr--;
}
#define DEDICATED_MIN (8ull << 20)
static int submit_wait(UdrCtx *c);
static int pool_alloc(UdrCtx *c, const VkMemoryRequirements *mr, VkDeviceMemory *mem, u64 *off, Res *r) {
    int t = mem_type(c, mr->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (t < 0) t = mem_type(c, mr->memoryTypeBits, 0);
    if (t < 0) return 0;
    /* large resources get their own allocation, freed with them (few enough for the drivers' allocation limit); in the
       shared blocks their holes fragmented memory that the many small buffers then could not fill */
    if (mr->size >= DEDICATED_MIN) {
        if (c->dead_bytes >= DEDICATED_MIN && c->recording && !c->in_pass) submit_wait(c);   /* destroyed memory back first */
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr->size, (u32)t};
        if (vkAllocateMemory(c->dev, &mai, 0, &r->mem) == VK_SUCCESS) { *mem = r->mem; *off = 0; r->psize = mr->size; c->st.dedicated++; return 1; }
        r->mem = 0;   /* (fall back to a block) */
    }
    /* small ones: blocks of their own kind (images apart from buffers: different lifetimes, fewer holes) */
    t |= (r->kind == R_IMAGE ? 0x100 : 0);
    int retried = 0;
again:;
    u64 al = mr->alignment ? mr->alignment : 1;
    if (al < 1024) al = 1024;   /* also keeps linear and optimal resources apart (bufferImageGranularity <= 1 KB on the devices used; see vkGetPhysicalDeviceProperties) */
    u64 need = (mr->size + 1023) & ~1023ull;   /* whole 1 KB granules, so a freed range never shares one with a live resource */
    for (u32 i = 0; i < c->nmpool; i++) if (c->mpool[i].type == (u32)t) {   /* freed ranges first (first fit) */
        for (u32 j = 0; j < c->mpool[i].nfr; j++) {
            FreeRange *f = &c->mpool[i].fr[j];
            u64 o = (f->off + al - 1) / al * al, end = f->off + f->size;
            if (o + need > end) continue;
            u64 pre = o - f->off, suf = end - (o + need);
            if (pre && suf) { f->size = pre; if (!fr_insert(c, i, j + 1, o + need, suf)) return 0; }
            else if (pre) f->size = pre;
            else if (suf) { f->off = o + need; f->size = suf; }
            else fr_remove(c, i, j);
            *mem = c->mpool[i].mem; *off = o; r->pblock = i + 1; r->poff = o; r->psize = need; c->st.pool_reused++;
            return 1;
        }
    }
    for (u32 i = 0; i < c->nmpool; i++) if (c->mpool[i].type == (u32)t) {
        u64 o = (c->mpool[i].used + al - 1) / al * al;
        if (o + need <= c->mpool[i].size) {
            if (o > c->mpool[i].used) {   /* the alignment gap is free space too */
                u32 n = c->mpool[i].nfr;
                if (n && c->mpool[i].fr[n - 1].off + c->mpool[i].fr[n - 1].size == c->mpool[i].used) c->mpool[i].fr[n - 1].size += o - c->mpool[i].used;
                else fr_insert(c, i, n, c->mpool[i].used, o - c->mpool[i].used);
            }
            c->mpool[i].used = o + need; *mem = c->mpool[i].mem; *off = o; r->pblock = i + 1; r->poff = o; r->psize = need; return 1;
        }
    }
    if (c->ndead && c->recording && !c->in_pass && !retried) {   /* before growing: let the destroyed resources go (after the GPU is done with them) and look again */
        submit_wait(c); retried = 1; goto again;
    }
    if (c->nmpool >= 256) return 0;
    u64 sz = need > POOL_BLOCK ? need : POOL_BLOCK;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, sz, (u32)t & 0xFF};
    VkDeviceMemory m; if (vkAllocateMemory(c->dev, &mai, 0, &m)) return 0;
    m_zero(&c->mpool[c->nmpool], sizeof c->mpool[0]);
    c->mpool[c->nmpool].mem = m; c->mpool[c->nmpool].size = sz; c->mpool[c->nmpool].used = need; c->mpool[c->nmpool].type = (u32)t;
    r->pblock = c->nmpool + 1; r->poff = 0; r->psize = need; c->nmpool++;
    *mem = m; *off = 0; return 1;
}
/* return a resource's range to its block: the bump pointer shrinks when it was the last one, otherwise a sorted, coalesced insert */
static void pool_free(UdrCtx *c, Res *r) {
    if (!r->pblock || r->pblock > c->nmpool) return;
    u32 b = r->pblock - 1; u64 off = r->poff, size = r->psize;
    r->pblock = 0;
    if (off + size == c->mpool[b].used) {
        c->mpool[b].used = off;
        while (c->mpool[b].nfr) { FreeRange *l = &c->mpool[b].fr[c->mpool[b].nfr - 1]; if (l->off + l->size != c->mpool[b].used) break; c->mpool[b].used = l->off; c->mpool[b].nfr--; }
        return;
    }
    u32 n = c->mpool[b].nfr, at = 0;
    FreeRange *f = c->mpool[b].fr;
    { u32 lo = 0, hi = n; while (lo < hi) { u32 mid = (lo + hi) / 2; if (f[mid].off < off) lo = mid + 1; else hi = mid; } at = lo; }
    int mprev = at > 0 && f[at - 1].off + f[at - 1].size == off, mnext = at < n && off + size == f[at].off;
    if (mprev && mnext) { f[at - 1].size += size + f[at].size; fr_remove(c, b, at); }
    else if (mprev) f[at - 1].size += size;
    else if (mnext) { f[at].off = off; f[at].size += size; }
    else fr_insert(c, b, at, off, size);
}

/* ---------------------------------------------------------------- command buffer / submission */
static void full_barrier(UdrCtx *c) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
}
static int begin(UdrCtx *c) {
    if (c->recording) return 1;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
    VkResult r = vkBeginCommandBuffer(c->cb, &bi); if (r) { fail(c, "vkBeginCommandBuffer", r, 0); return 0; }
    c->recording = 1; return 1;
}
static void end_pass_if_open(UdrCtx *c);
static void free_dead(UdrCtx *c);
/* submit what is recorded and wait (v1: synchronous). The descriptor pool and staging ring are free again after it. */
static int submit_wait(UdrCtx *c) {
    if (!c->recording) return 1;
    if (c->in_pass) { fail(c, "submit inside a pass (missing END_PASS)", 0, c->cmd); end_pass_if_open(c); }
    VkResult r = vkEndCommandBuffer(c->cb); c->recording = 0;
    if (r) { fail(c, "vkEndCommandBuffer", r, 0); return 0; }
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &c->cb, 0, 0};
    vkResetFences(c->dev, 1, &c->fence);
    r = vkQueueSubmit(c->q, 1, &si, c->fence); if (r) { fail(c, "vkQueueSubmit", r, 0); return 0; }
    r = vkWaitForFences(c->dev, 1, &c->fence, VK_TRUE, ~0ull); if (r) { fail(c, "vkWaitForFences", r, 0); return 0; }
    vkResetCommandBuffer(c->cb, 0);
    free_dead(c);
    vkResetDescriptorPool(c->dev, c->dpool, 0); c->dsets_in_pool = 0;
    c->stage_used = 0; c->st.submits++; c->pipe_bound = 0; c->binds_dirty = 1; c->push_dirty = 1;
    return 1;
}
/* staging memory for uploads / readbacks; grows by submitting and reallocating */
static int stage_ensure(UdrCtx *c, u64 bytes) {
    if (c->stage_used + bytes <= c->stage_size) return 1;
    if (!submit_wait(c)) return 0;
    if (bytes <= c->stage_size) return 1;
    if (c->stage) { vkUnmapMemory(c->dev, c->stage_mem); vkDestroyBuffer(c->dev, c->stage, 0); vkFreeMemory(c->dev, c->stage_mem, 0); c->stage = 0; }
    u64 n = c->stage_size ? c->stage_size : (64ull << 20); while (n < bytes) n *= 2;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    VkResult r = vkCreateBuffer(c->dev, &bci, 0, &c->stage); if (r) { fail(c, "staging buffer", r, n); return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c->dev, c->stage, &mr);
    /* cached host memory when the device has it: the staging buffer is also where readbacks land, and reading uncached
       (write-combined) memory with the CPU runs at a small fraction of the speed (STATUS 30e: per-step traces on the RTX
       took 0.4 s per step, mostly that) */
    int t = mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, (u32)t};
    if (t < 0 || (r = vkAllocateMemory(c->dev, &mai, 0, &c->stage_mem))) { fail(c, "staging memory", r, n); return 0; }
    vkBindBufferMemory(c->dev, c->stage, c->stage_mem, 0);
    void *p; r = vkMapMemory(c->dev, c->stage_mem, 0, VK_WHOLE_SIZE, 0, &p); if (r) { fail(c, "map staging", r, 0); return 0; }
    c->stage_map = (u8 *)p; c->stage_size = n; c->stage_used = 0; return 1;
}

/* ---------------------------------------------------------------- hazards */
static void note_hazard(UdrCtx *c, Res *r, UdrId id, u32 op) {
    c->st.hazards++;
    if (c->nhaz < 64) { UdrHazard *z = &c->haz[c->nhaz++]; z->cmd = c->cmd; z->op = op; z->resource = id; z->writer_cmd = r->wr_cmd; z->writer_op = r->wr_op; }
    /* safety net: a barrier here (not possible inside a pass: the hazard is only reported there) */
    if ((c->cfg.flags & UDR_CFG_HAZARD_BARRIER) && !c->in_pass) { full_barrier(c); c->st.barriers_auto++; c->epoch++; }
}
/* a command reads resource id: written since the last barrier by another command? */
static void check_read(UdrCtx *c, UdrId id, Res *r, u32 op, int atomic_only) {
    if (!(c->cfg.flags & (UDR_CFG_HAZARD_REPORT | UDR_CFG_HAZARD_BARRIER))) return;
    if (r->wr_epoch != c->epoch || r->wr_cmd == c->cmd) return;
    if (atomic_only && r->wr_atomic_only) return;   /* atomics against atomics: order-independent */
    note_hazard(c, r, id, op);
}
static void mark_write(UdrCtx *c, Res *r, u32 op, int atomic_only) {
    if (r->wr_epoch == c->epoch && r->wr_cmd != c->cmd) atomic_only = atomic_only && r->wr_atomic_only;
    r->wr_epoch = c->epoch; r->wr_cmd = c->cmd; r->wr_op = op; r->wr_atomic_only = atomic_only;
}

/* ---------------------------------------------------------------- creation */
static void init_layout(UdrCtx *c, Res *r) {   /* new images go to GENERAL once */
    if (!begin(c)) return;
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, r->img,
        {r->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &ib);
}
static void op_image_create(UdrCtx *c, const UdrImageCreate *a) {
    if (a->format == UDR_FMT_UNDEFINED || a->format >= UDR_FMT_COUNT || !a->width || !a->height) { fail(c, "IMAGE_CREATE: bad description", 0, a->id); return; }
    Res *r = res_new(c, a->id, R_IMAGE); if (!r) return;
    const FmtInfo *fi = &g_fmt[a->format];
    r->fmt = a->format; r->type = a->type; r->w = a->width; r->h = a->height; r->mips = a->mips ? a->mips : 1; r->usage = a->usage; r->aspect = fi->aspect;
    r->d = a->type == UDR_IMG_3D ? (a->depth_or_layers ? a->depth_or_layers : 1) : 1;
    r->layers = a->type == UDR_IMG_3D ? 1 : (a->depth_or_layers ? a->depth_or_layers : 1);
    if (a->type == UDR_IMG_CUBE && r->layers % 6) { fail(c, "IMAGE_CREATE: cube needs 6n layers", 0, a->id); r->kind = R_NONE; return; }
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* usages the format does not support on this device are dropped (the producer asks for what an image of the game
       may be used as; a use it does not support fails later, where it happens) */
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(c->pd, fi->vk, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    if ((a->usage & UDR_USE_SAMPLED) && (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((a->usage & UDR_USE_STORAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((a->usage & UDR_USE_COLOR) && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && a->type != UDR_IMG_3D) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((a->usage & UDR_USE_DEPTH) && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!(ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) && !(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { fail(c, "IMAGE_CREATE: format not supported on this device", 0, a->format); r->kind = R_NONE; return; }
    r->usage = ((u & VK_IMAGE_USAGE_SAMPLED_BIT) ? UDR_USE_SAMPLED : 0) | ((u & VK_IMAGE_USAGE_STORAGE_BIT) ? UDR_USE_STORAGE : 0) | ((u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? UDR_USE_COLOR : 0)
             | ((u & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? UDR_USE_DEPTH : 0) | UDR_USE_TRANSFER;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0,
        (VkImageCreateFlags)((a->type == UDR_IMG_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT),
        a->type == UDR_IMG_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, fi->vk, {r->w, r->h, r->d}, r->mips, r->layers, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult vr = vkCreateImage(c->dev, &ci, 0, &r->img);
    if (vr) { fail(c, "vkCreateImage", vr, a->id); r->kind = R_NONE; return; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->dev, r->img, &mr);
    if (mr.size >= (c->cfg.log_image_mb ? (u64)c->cfg.log_image_mb << 20 : (64ull << 20))) {   /* large images are worth knowing about (device memory diagnostics) */
        char t[160]; Str q = {t, 0, sizeof t}; s_put(&q, "udr: large image "); s_dec(&q, a->id); s_put(&q, ": "); s_dec(&q, r->w); s_put(&q, "x"); s_dec(&q, r->h);
        s_put(&q, "x"); s_dec(&q, r->d); s_put(&q, " layers "); s_dec(&q, r->layers); s_put(&q, " mips "); s_dec(&q, r->mips); s_put(&q, " format "); s_dec(&q, a->format);
        s_put(&q, ": "); s_dec(&q, mr.size >> 20); s_put(&q, " MB\n"); logs(c, t);
    }
    VkDeviceMemory pm; u64 po;
    if (!pool_alloc(c, &mr, &pm, &po, r)) { fail(c, "image memory", 0, a->id); vkDestroyImage(c->dev, r->img, 0); r->kind = R_NONE; return; }
    vkBindImageMemory(c->dev, r->img, pm, po);
    init_layout(c, r);
}
static void op_buffer_create(UdrCtx *c, const UdrBufferCreate *a) {
    if (!a->size) { fail(c, "BUFFER_CREATE: size 0", 0, a->id); return; }
    Res *r = res_new(c, a->id, R_BUFFER); if (!r) return;
    r->size = a->size;
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (a->usage & UDR_BUF_STORAGE) u |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (a->usage & UDR_BUF_UNIFORM) u |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (a->usage & UDR_BUF_INDEX) u |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, (a->size + 3) & ~3ull, u, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    VkResult vr = vkCreateBuffer(c->dev, &bci, 0, &r->buf); if (vr) { fail(c, "vkCreateBuffer", vr, a->id); r->kind = R_NONE; return; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c->dev, r->buf, &mr);
    VkDeviceMemory pm; u64 po;
    if (!pool_alloc(c, &mr, &pm, &po, r)) { fail(c, "buffer memory", 0, a->id); vkDestroyBuffer(c->dev, r->buf, 0); r->kind = R_NONE; return; }
    vkBindBufferMemory(c->dev, r->buf, pm, po);
}
static const VkFilter k_filter[] = {VK_FILTER_NEAREST, VK_FILTER_LINEAR};
static const VkSamplerAddressMode k_addr[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE /* mirror-once needs an extension: approximated */};
static const VkBorderColor k_border[] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
static const VkCompareOp k_cmp[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
    VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
static void op_sampler_create(UdrCtx *c, const UdrSamplerCreate *a) {
    Res *r = res_new(c, a->id, R_SAMPLER); if (!r) return;
    /* anisotropic filtering as the IR asks for it (max_aniso > 1), when the device has the feature (E6 vk_sampler:
       anisotropyEnable when V.aniso_ok). It was ignored here until 19 October: every texture of the G-buffer differed
       slightly from E6 at oblique angles. */
    float an = (float)a->max_aniso; if (an > c->max_aniso) an = c->max_aniso;
    int use_an = c->aniso && !(c->cfg.flags & UDR_CFG_NO_ANISO) && a->max_aniso > 1 && an > 1.0f;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, k_filter[a->mag & 1], k_filter[a->min & 1],
        (a->mip & 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        k_addr[a->addr_u % 5], k_addr[a->addr_v % 5], k_addr[a->addr_w % 5], a->lod_bias, use_an ? VK_TRUE : VK_FALSE, use_an ? an : 1.0f,
        a->compare ? VK_TRUE : VK_FALSE, a->compare ? k_cmp[(a->compare - 1) & 7] : VK_COMPARE_OP_NEVER, a->min_lod, a->max_lod, k_border[a->border % 3], VK_FALSE};
    VkResult vr = vkCreateSampler(c->dev, &ci, 0, &r->smp); if (vr) { fail(c, "vkCreateSampler", vr, a->id); r->kind = R_NONE; }
}
static void op_shader_create(UdrCtx *c, const UdrShaderCreate *a, const u32 *words, u64 avail) {
    if (!a->nwords || (u64)a->nwords * 4 > avail || words[0] != 0x07230203) { fail(c, "SHADER_CREATE: not SPIR-V", 0, a->id); return; }
    Res *r = res_new(c, a->id, R_SHADER); if (!r) return;
    r->stage = a->stage;
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, (u64)a->nwords * 4, words};
    VkResult vr = vkCreateShaderModule(c->dev, &ci, 0, &r->mod); if (vr) { fail(c, "vkCreateShaderModule", vr, a->id); r->kind = R_NONE; }
}
static const VkDescriptorType k_desc[] = {VK_DESCRIPTOR_TYPE_MAX_ENUM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
static const VkPrimitiveTopology k_topo[] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
static const VkBlendFactor k_bf[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
static const VkBlendOp k_bo[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
static const VkStencilOp k_so[] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
    VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
static VkStencilOpState stencil_face(const UdrStencilFace *f) {
    VkStencilOpState s = {k_so[f->fail & 7], k_so[f->pass & 7], k_so[f->depth_fail & 7], k_cmp[f->compare & 7], f->read_mask, f->write_mask, 0};
    return s;
}
static VkShaderStageFlags stage_bits(u32 stages) {
    VkShaderStageFlags f = 0;
    if (stages & (1u << UDR_STAGE_VS)) f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (stages & (1u << UDR_STAGE_PS)) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stages & (1u << UDR_STAGE_CS)) f |= VK_SHADER_STAGE_COMPUTE_BIT;
    return f ? f : VK_SHADER_STAGE_ALL;
}
static void op_pipeline_create(UdrCtx *c, const UdrPipelineCreate *a) {
    int compute = a->cs != 0;
    Res *vs = compute ? 0 : res_get(c, a->vs, R_SHADER), *ps = (!compute && a->ps) ? res_get(c, a->ps, R_SHADER) : 0, *cs = compute ? res_get(c, a->cs, R_SHADER) : 0;
    if ((compute && !cs) || (!compute && (!vs || (a->ps && !ps)))) { fail(c, "PIPELINE_CREATE: unknown shader", 0, a->id); return; }
    if (a->nbind > UDR_MAX_BINDINGS || a->ncolor > UDR_MAX_TARGETS || a->push_bytes > 128 || (a->push_bytes & 3)) { fail(c, "PIPELINE_CREATE: bad limits", 0, a->id); return; }
    /* the modules are taken now: res_new may grow (move) the resource table, and the shader pointers would then point
       into the freed old table. On Windows the old table (over 1 MB) is returned to the system at once, so reading it
       faulted (the silent stop at 15284 step 52 and 6162 step 58 on the RTX, STATUS 30a); glibc kept it mapped. */
    VkShaderModule vs_mod = vs ? vs->mod : 0, ps_mod = ps ? ps->mod : 0, cs_mod = cs ? cs->mod : 0;
    vs = ps = cs = 0;
    Res *r = res_new(c, a->id, R_PIPELINE); if (!r) return;
    r->compute = compute; r->push_bytes = a->push_bytes; r->nbind = a->nbind; m_copy(r->bind, a->bind, sizeof a->bind);
    r->ncolor = a->ncolor; m_copy(r->color_format, a->color_format, sizeof a->color_format); r->depth_format = a->depth_format;
    VkDescriptorSetLayoutBinding lb[UDR_MAX_BINDINGS];
    for (u32 i = 0; i < a->nbind; i++) {
        if (a->bind[i].type < 1 || a->bind[i].type > 6) { fail(c, "PIPELINE_CREATE: bad binding type", 0, a->id); r->kind = R_NONE; return; }
        VkDescriptorSetLayoutBinding b = {a->bind[i].binding, k_desc[a->bind[i].type], 1, compute ? VK_SHADER_STAGE_COMPUTE_BIT : stage_bits(a->bind[i].stages), 0};
        lb[i] = b;
    }
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, a->nbind, lb};
    VkResult vr = vkCreateDescriptorSetLayout(c->dev, &dl, 0, &r->dsl); if (vr) { fail(c, "vkCreateDescriptorSetLayout", vr, a->id); r->kind = R_NONE; return; }
    VkPushConstantRange pr = {compute ? VK_SHADER_STAGE_COMPUTE_BIT : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT), 0, a->push_bytes};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &r->dsl, a->push_bytes ? 1u : 0u, &pr};
    if ((vr = vkCreatePipelineLayout(c->dev, &pl, 0, &r->layout))) { fail(c, "vkCreatePipelineLayout", vr, a->id); r->kind = R_NONE; return; }
    if (compute) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs_mod, "main", 0}, r->layout, 0, -1};
        vr = vkCreateComputePipelines(c->dev, 0, 1, &ci, 0, &r->pipe);
    } else {
        VkPipelineShaderStageCreateInfo st[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_mod, "main", 0},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps_mod, "main", 0}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, 0, 0, 0, 0, 0, 0};   /* vertex data is fetched by the shaders */
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, k_topo[a->topology % 6], a->primitive_restart ? VK_TRUE : VK_FALSE};
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, a->depth_clamp ? VK_TRUE : VK_FALSE, VK_FALSE,
            VK_POLYGON_MODE_FILL, a->cull == UDR_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : a->cull == UDR_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
            a->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, a->depth_bias ? VK_TRUE : VK_FALSE,
            a->bias_constant, c->bias_clamp_ok ? a->bias_clamp : 0.0f, a->bias_slope, 1.0f};   /* (no clamp without the feature) */
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 1.0f, 0, VK_FALSE, VK_FALSE};
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, 0, 0, a->depth_test ? VK_TRUE : VK_FALSE,
            a->depth_write ? VK_TRUE : VK_FALSE, k_cmp[a->depth_compare & 7], VK_FALSE, a->stencil_test ? VK_TRUE : VK_FALSE,
            stencil_face(&a->stencil_front), stencil_face(&a->stencil_back), 0.0f, 1.0f};
        VkPipelineColorBlendAttachmentState cba[UDR_MAX_TARGETS];
        VkFormat cf[UDR_MAX_TARGETS];
        for (u32 i = 0; i < a->ncolor; i++) {
            const UdrBlend *b = &a->blend[i];
            VkPipelineColorBlendAttachmentState s = {b->enable ? VK_TRUE : VK_FALSE, k_bf[b->src_color % 15], k_bf[b->dst_color % 15], k_bo[b->op_color % 5],
                k_bf[b->src_alpha % 15], k_bf[b->dst_alpha % 15], k_bo[b->op_alpha % 5], b->write_mask & 15};
            if (a->color_format[i] == UDR_FMT_UNDEFINED) { s.blendEnable = VK_FALSE; s.colorWriteMask = 0; }   /* unused slot */
            cba[i] = s;
            cf[i] = a->color_format[i] < UDR_FMT_COUNT ? g_fmt[a->color_format[i]].vk : VK_FORMAT_UNDEFINED;
        }
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, VK_FALSE, VK_LOGIC_OP_COPY, a->ncolor, cba, {0, 0, 0, 0}};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 4, dyn};
        VkFormat dfmt = a->depth_format < UDR_FMT_COUNT ? g_fmt[a->depth_format].vk : VK_FORMAT_UNDEFINED;
        VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, a->ncolor, cf, dfmt,
            (a->depth_format == UDR_FMT_D32_FLOAT_S8_UINT || a->depth_format == UDR_FMT_D16_UNORM_S8_UINT) ? dfmt : VK_FORMAT_UNDEFINED};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, ps_mod ? 2u : 1u, st, &vi, &ia, 0, &vp, &rs, &ms, &ds, &cb, &dy,
            r->layout, 0, 0, 0, -1};
        vr = vkCreateGraphicsPipelines(c->dev, 0, 1, &ci, 0, &r->pipe);
    }
    if (vr) { fail(c, "pipeline creation", vr, a->id); r->kind = R_NONE; return; }
    c->st.pipelines++;
}
static void res_free(UdrCtx *c, Res *r) {
    for (u32 i = 0; i < r->nviews; i++) vkDestroyImageView(c->dev, r->views[i].view, 0);
    if (r->img) vkDestroyImage(c->dev, r->img, 0);
    if (r->buf) vkDestroyBuffer(c->dev, r->buf, 0);
    if (r->mem) vkFreeMemory(c->dev, r->mem, 0);
    if (r->smp) vkDestroySampler(c->dev, r->smp, 0);
    if (r->mod) vkDestroyShaderModule(c->dev, r->mod, 0);
    if (r->pipe) vkDestroyPipeline(c->dev, r->pipe, 0);
    if (r->layout) vkDestroyPipelineLayout(c->dev, r->layout, 0);
    if (r->dsl) vkDestroyDescriptorSetLayout(c->dev, r->dsl, 0);
    pool_free(c, r);
    m_zero(r, sizeof *r);
}
/* the id is gone at once; the Vulkan objects (which recorded commands may still use) are freed after the submission completes */
static void op_destroy(UdrCtx *c, UdrId id) {
    if (!id || id >= c->nres || !c->res[id].kind) { fail(c, "DESTROY: unknown id", 0, id); return; }
    c->st.destroyed++;
    if (!c->recording) { res_free(c, &c->res[id]); return; }
    if (c->ndead == c->cdead) {
        u32 n = c->cdead ? c->cdead * 2 : 256;
        Res *d = (Res *)c->h.alloc((u64)n * sizeof(Res)); if (!d) { submit_wait(c); res_free(c, &c->res[id]); return; }
        if (c->dead) { m_copy(d, c->dead, (u64)c->ndead * sizeof(Res)); c->h.free(c->dead); }
        c->dead = d; c->cdead = n;
    }
    c->dead[c->ndead++] = c->res[id]; c->dead_bytes += c->res[id].psize; m_zero(&c->res[id], sizeof(Res));
}
static void free_dead(UdrCtx *c) { for (u32 i = 0; i < c->ndead; i++) res_free(c, &c->dead[i]); c->ndead = 0; c->dead_bytes = 0; }

/* ---------------------------------------------------------------- views */
/* swz: UdrBinding.swizzle (the T#'s DST_SEL, 0 = identity), sampled views only */
static VkComponentSwizzle swz_comp(u32 sel) {
    static const VkComponentSwizzle map[8] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                              VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    return map[sel & 7];
}
static VkImageView image_view(UdrCtx *c, Res *r, u32 base_mip, u32 nmip, u32 base_layer, u32 nlayer, u32 fmt, int as_target, u32 swz) {
    if (!nmip) nmip = r->mips - base_mip;
    if (!nlayer) nlayer = r->layers - base_layer;
    if (!fmt) fmt = r->fmt;
    swz &= 0xFFF; if (swz == 0xFAC || as_target) swz = 0;   /* X=R Y=G Z=B W=A is the identity */
    u64 key = (u64)base_mip | ((u64)nmip << 8) | ((u64)base_layer << 16) | ((u64)(nlayer & 0xFFF) << 32) | ((u64)fmt << 44) | ((u64)as_target << 52);
    for (u32 i = 0; i < r->nviews; i++) if (r->views[i].key == key && r->views[i].swz == swz) return r->views[i].view;
    VkImageViewType vt = r->type == UDR_IMG_3D ? VK_IMAGE_VIEW_TYPE_3D : (r->type == UDR_IMG_CUBE && !as_target && nlayer % 6 == 0) ?
        (nlayer > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : (nlayer > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    VkImageAspectFlags asp = r->aspect;
    if (!as_target && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) asp = VK_IMAGE_ASPECT_DEPTH_BIT;   /* sampling depth+stencil: the depth part */
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (swz) { cm.r = swz_comp(swz); cm.g = swz_comp(swz >> 3); cm.b = swz_comp(swz >> 6); cm.a = swz_comp(swz >> 9); }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, r->img, vt, g_fmt[fmt].vk, cm,
        {asp, base_mip, nmip, base_layer, nlayer}};
    VkImageView v; VkResult vr = vkCreateImageView(c->dev, &ci, 0, &v);
    if (vr) { fail(c, "vkCreateImageView", vr, 0); return 0; }
    if (r->nviews == 8) { vkDestroyImageView(c->dev, r->views[0].view, 0); for (int i = 1; i < 8; i++) r->views[i - 1] = r->views[i]; r->nviews = 7; }   /* only after a submit in practice */
    r->views[r->nviews].key = key; r->views[r->nviews].swz = swz; r->views[r->nviews].view = v; r->nviews++;
    return v;
}

/* ---------------------------------------------------------------- transfers */
static void op_buffer_write(UdrCtx *c, const UdrBufferWrite *a, const u8 *data, u64 avail) {
    Res *r = res_get(c, a->buffer, R_BUFFER);
    if (!r || a->bytes > avail || a->offset + a->bytes > r->size) { fail(c, "BUFFER_WRITE: bad buffer or range", 0, a->buffer); return; }
    if (!a->bytes) return;
    if (c->in_pass) { fail(c, "BUFFER_WRITE inside a pass", 0, a->buffer); return; }
    if (!stage_ensure(c, (a->bytes + 15) & ~15ull) || !begin(c)) return;
    m_copy(c->stage_map + c->stage_used, data, a->bytes);
    full_barrier(c); c->st.barriers_auto++;   /* uploads are ordered after everything before them ... */
    VkBufferCopy bc = {c->stage_used, a->offset, a->bytes}; vkCmdCopyBuffer(c->cb, c->stage, r->buf, 1, &bc);
    full_barrier(c); c->st.barriers_auto++;   /* ... and visible to everything after them */
    c->stage_used += (a->bytes + 15) & ~15ull; c->st.uploads_bytes += a->bytes;
}
static void op_image_write(UdrCtx *c, const UdrImageWrite *a, const u8 *data, u64 avail) {
    Res *r = res_get(c, a->image, R_IMAGE);
    u32 mip = a->mip & ~UDR_WRITE_STENCIL; int sten = (a->mip & UDR_WRITE_STENCIL) != 0;
    if (!r || a->bytes > avail || mip >= r->mips || a->layer >= r->layers || !a->w || !a->h || (sten && !(r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))) { fail(c, "IMAGE_WRITE: bad image or region", 0, a->image); return; }
    FmtInfo fs = g_fmt[r->fmt]; if (sten) { fs.bytes = 1; fs.block = 1; fs.aspect = VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT; }
    const FmtInfo *fi = &fs;
    u32 bw = (a->w + fi->block - 1) / fi->block, bh = (a->h + fi->block - 1) / fi->block, d = a->d ? a->d : 1;
    u64 need = (u64)a->row_bytes * bh * d;
    if (c->in_pass) { fail(c, "IMAGE_WRITE inside a pass", 0, a->image); return; }
    if (a->row_bytes < (u64)bw * fi->bytes || need > a->bytes || (a->row_bytes % fi->bytes)) { fail(c, "IMAGE_WRITE: data too small", 0, a->image); return; }
    if (!stage_ensure(c, (need + 15) & ~15ull) || !begin(c)) return;
    m_copy(c->stage_map + c->stage_used, data, need);
    full_barrier(c); c->st.barriers_auto++;
    VkBufferImageCopy bic = {c->stage_used, a->row_bytes / fi->bytes * fi->block, bh * fi->block, {sten ? VK_IMAGE_ASPECT_STENCIL_BIT : fi->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT, mip, a->layer, 1},
        {(i32)a->x, (i32)a->y, (i32)a->z}, {a->w, a->h, d}};
    vkCmdCopyBufferToImage(c->cb, c->stage, r->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
    full_barrier(c); c->st.barriers_auto++;
    c->stage_used += (need + 15) & ~15ull; c->st.uploads_bytes += need;
}
static void op_buffer_fill(UdrCtx *c, const UdrBufferFill *a) {
    Res *r = res_get(c, a->buffer, R_BUFFER);
    if (!r || a->offset + a->bytes > r->size || (a->offset & 3)) { fail(c, "BUFFER_FILL: bad buffer or range", 0, a->buffer); return; }
    if (c->in_pass) { fail(c, "BUFFER_FILL inside a pass", 0, a->buffer); return; }
    if (!begin(c)) return;
    vkCmdFillBuffer(c->cb, r->buf, a->offset, a->bytes ? ((a->bytes + 3) & ~3ull) : VK_WHOLE_SIZE, a->value);
    mark_write(c, r, UDR_OP_BUFFER_FILL, 0);
}
static void op_buffer_copy(UdrCtx *c, const UdrBufferCopy *a) {
    Res *s = res_get(c, a->src, R_BUFFER), *d = res_get(c, a->dst, R_BUFFER);
    if (!s || !d || a->src_offset + a->bytes > s->size || a->dst_offset + a->bytes > d->size) { fail(c, "BUFFER_COPY: bad buffers or range", 0, a->src); return; }
    if (c->in_pass) { fail(c, "BUFFER_COPY inside a pass", 0, a->src); return; }
    if (!begin(c)) return;
    check_read(c, a->src, s, UDR_OP_BUFFER_COPY, 0);
    VkBufferCopy bc = {a->src_offset, a->dst_offset, a->bytes}; vkCmdCopyBuffer(c->cb, s->buf, d->buf, 1, &bc);
    mark_write(c, d, UDR_OP_BUFFER_COPY, 0);
}
static void op_image_to_buffer(UdrCtx *c, const UdrImageToBuffer *a) {
    Res *s = res_get(c, a->image, R_IMAGE), *d = res_get(c, a->buffer, R_BUFFER);
    u64 tb = s ? g_fmt[s->fmt].bytes : 0;
    if (!s || !d || !a->w || !a->h || a->mip >= s->mips || a->layer >= s->layers || a->w > s->w || a->h > s->h || a->row_texels < a->w || g_fmt[s->fmt].block != 1 || (s->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        || a->offset + ((u64)a->row_texels * (a->h - 1) + a->w) * tb > d->size) { fail(c, "IMAGE_TO_BUFFER: bad image, buffer or range", 0, a->image); return; }
    if (c->in_pass) { fail(c, "IMAGE_TO_BUFFER inside a pass", 0, a->image); return; }
    if (!begin(c)) return;
    check_read(c, a->image, s, UDR_OP_IMAGE_TO_BUFFER, 0);
    VkBufferImageCopy bic = {a->offset, a->row_texels, 0, {VK_IMAGE_ASPECT_COLOR_BIT, a->mip, a->layer, 1}, {0, 0, 0}, {a->w, a->h, 1}};
    vkCmdCopyImageToBuffer(c->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->buf, 1, &bic);
    mark_write(c, d, UDR_OP_IMAGE_TO_BUFFER, 0);
}
static void op_image_copy(UdrCtx *c, const UdrImageCopy *a) {
    Res *s = res_get(c, a->src, R_IMAGE), *d = res_get(c, a->dst, R_IMAGE);
    if (!s || !d || a->src_mip >= s->mips || a->dst_mip >= d->mips) { fail(c, "IMAGE_COPY: bad images", 0, a->src); return; }
    if (c->in_pass) { fail(c, "IMAGE_COPY inside a pass", 0, a->src); return; }
    if (!begin(c)) return;
    check_read(c, a->src, s, UDR_OP_IMAGE_COPY, 0);
    VkImageCopy ic = {{s->aspect, a->src_mip, a->src_layer, 1}, {(i32)a->sx, (i32)a->sy, 0}, {d->aspect, a->dst_mip, a->dst_layer, 1}, {(i32)a->dx, (i32)a->dy, 0}, {a->w, a->h, 1}};
    vkCmdCopyImage(c->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    mark_write(c, d, UDR_OP_IMAGE_COPY, 0);
}
static void op_image_clear(UdrCtx *c, const UdrImageClear *a) {
    Res *r = res_get(c, a->image, R_IMAGE);
    if (!r || a->mip >= r->mips || a->layer >= r->layers) { fail(c, "IMAGE_CLEAR: bad image", 0, a->image); return; }
    if (c->in_pass) { fail(c, "IMAGE_CLEAR inside a pass", 0, a->image); return; }
    if (!begin(c)) return;
    VkImageSubresourceRange sr = {r->aspect, a->mip, 1, a->layer, 1};
    if (r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { VkClearDepthStencilValue v = {a->depth, a->stencil}; vkCmdClearDepthStencilImage(c->cb, r->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &sr); }
    else { VkClearColorValue v; m_copy(v.float32, a->color, 16); vkCmdClearColorImage(c->cb, r->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &sr); }
    mark_write(c, r, UDR_OP_IMAGE_CLEAR, 0);
}

/* ---------------------------------------------------------------- passes */
static const VkAttachmentLoadOp k_load[] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE};
static void end_pass_if_open(UdrCtx *c) {
    if (!c->in_pass) return;
    vkCmdEndRendering(c->cb); c->in_pass = 0;
    for (u32 i = 0; i < c->pass.ncolor; i++) { Res *r = res_get(c, c->pass.color[i].image, R_IMAGE); if (r) mark_write(c, r, UDR_OP_END_PASS, 0); }
    if (c->pass.depth.image && !c->pass.depth.read_only) { Res *r = res_get(c, c->pass.depth.image, R_IMAGE); if (r) mark_write(c, r, UDR_OP_END_PASS, 0); }
}
static void op_begin_pass(UdrCtx *c, const UdrBeginPass *a) {
    if (c->in_pass) { fail(c, "BEGIN_PASS while a pass is open", 0, c->cmd); end_pass_if_open(c); }
    if (a->ncolor > UDR_MAX_TARGETS || !a->width || !a->height) { fail(c, "BEGIN_PASS: bad description", 0, c->cmd); return; }
    if (!begin(c)) return;
    /* Render-target writes are ordered on the PS4 without any request from the game (the colour / depth pipeline
       keeps draw order), also from one "pass" to the next. Vulkan needs a barrier between two renderings that use the
       same attachment, so the backend adds it. (Reading a render target as a texture is different: there the game
       asks for a cache flush, and the IR must contain the barrier.) */
    int need = 0;
    for (u32 i = 0; i < a->ncolor; i++) { Res *r = res_get(c, a->color[i].image, R_IMAGE); if (r && r->wr_epoch == c->epoch && r->wr_op == UDR_OP_END_PASS) need = 1; }
    if (a->depth.image) { Res *r = res_get(c, a->depth.image, R_IMAGE); if (r && r->wr_epoch == c->epoch && r->wr_op == UDR_OP_END_PASS) need = 1; }
    if (need) { full_barrier(c); c->st.barriers_auto++; }
    VkRenderingAttachmentInfo ca[UDR_MAX_TARGETS], da; m_zero(&da, sizeof da);
    for (u32 i = 0; i < a->ncolor; i++) {
        const UdrColorTarget *t = &a->color[i];
        if (!t->image) {   /* an unused slot (a gap in the PS4 MRT list): no image view, nothing loaded or stored */
            VkRenderingAttachmentInfo none = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_RESOLVE_MODE_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, {{{0}}}};
            ca[i] = none; continue;
        }
        Res *r = res_get(c, t->image, R_IMAGE);
        if (!r || !(r->usage & UDR_USE_COLOR)) { fail(c, "BEGIN_PASS: color target is not a color image", 0, t->image); return; }
        if (t->load == UDR_LOAD_LOAD && r->wr_op != UDR_OP_END_PASS) check_read(c, t->image, r, UDR_OP_BEGIN_PASS, 0);
        VkRenderingAttachmentInfo ai = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, image_view(c, r, t->mip, 1, t->layer, 1, t->view_format < UDR_FMT_COUNT ? t->view_format : 0, 1, 0), VK_IMAGE_LAYOUT_GENERAL,
            VK_RESOLVE_MODE_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, k_load[t->load % 3], t->store == UDR_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        m_copy(ai.clearValue.color.float32, t->clear, 16);
        ca[i] = ai;
    }
    int has_d = a->depth.image != 0, has_s = 0;
    if (has_d) {
        const UdrDepthTarget *t = &a->depth; Res *r = res_get(c, t->image, R_IMAGE);
        if (!r || !(r->usage & UDR_USE_DEPTH)) { fail(c, "BEGIN_PASS: depth target is not a depth image", 0, t->image); return; }
        if (t->load == UDR_LOAD_LOAD && r->wr_op != UDR_OP_END_PASS) check_read(c, t->image, r, UDR_OP_BEGIN_PASS, 0);
        has_s = (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        VkRenderingAttachmentInfo ai = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, image_view(c, r, t->mip, 1, t->layer, 1, 0, 1, 0), VK_IMAGE_LAYOUT_GENERAL,
            VK_RESOLVE_MODE_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, k_load[t->load % 3], t->store == UDR_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        ai.clearValue.depthStencil.depth = t->clear_depth; ai.clearValue.depthStencil.stencil = t->clear_stencil;
        da = ai;
    }
    VkRenderingAttachmentInfo sa = da;
    if (has_s) { sa.loadOp = k_load[a->depth.stencil_load % 3]; sa.storeOp = a->depth.stencil_store == UDR_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE; }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, {a->width, a->height}}, 1, 0, a->ncolor, ca, has_d ? &da : 0, has_s ? &sa : 0};
    vkCmdBeginRendering(c->cb, &ri);
    c->in_pass = 1; c->pass = *a; c->have_viewport = c->have_scissor = 0; c->pipe_bound = 0; c->st.passes++;
}

/* ---------------------------------------------------------------- draws */
static int bind_for_command(UdrCtx *c, Res *p, u32 op) {
    if (!c->pipe_bound) { vkCmdBindPipeline(c->cb, p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe); c->pipe_bound = 1; c->binds_dirty = c->push_dirty = 1; }
    /* hazards: everything this command reads */
    for (u32 i = 0; i < p->nbind; i++) {
        const UdrBindingDesc *bd = &p->bind[i];
        if (bd->binding >= UDR_MAX_BINDINGS || !(c->bind_valid & (1ull << bd->binding))) { fail(c, "draw/dispatch: pipeline binding not bound", 0, bd->binding); return 0; }
        const UdrBinding *b = &c->binds[bd->binding];
        Res *r = b->type == UDR_BIND_SAMPLER ? 0 : res_get(c, b->id, (b->type == UDR_BIND_UNIFORM_BUFFER || b->type == UDR_BIND_STORAGE_BUFFER) ? R_BUFFER : R_IMAGE);
        if (b->type != UDR_BIND_SAMPLER && !r) { fail(c, "draw/dispatch: bound resource does not exist", 0, b->id); return 0; }
        u32 acc = bd->access ? bd->access : (bd->type == UDR_BIND_STORAGE_BUFFER || bd->type == UDR_BIND_STORAGE_IMAGE) ? (UDR_ACCESS_READ | UDR_ACCESS_WRITE) : UDR_ACCESS_READ;
        if (r && (acc & (UDR_ACCESS_READ | UDR_ACCESS_ATOMIC))) check_read(c, b->id, r, op, (acc & UDR_ACCESS_ATOMIC) && !(acc & UDR_ACCESS_READ));
        if (r && c->in_pass) {   /* sampling an attachment of the open pass: a feedback loop the IR must not contain */
            for (u32 k = 0; k < c->pass.ncolor; k++) if (c->pass.color[k].image == b->id) { fail(c, "draw reads an image that is a color target of the open pass", 0, b->id); return 0; }
        }
    }
    if (c->binds_dirty && p->nbind) {
        if (c->dsets_in_pool >= 4000) { fail(c, "descriptor pool exhausted within one submit", 0, c->cmd); return 0; }
        VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, c->dpool, 1, &p->dsl};
        VkResult vr = vkAllocateDescriptorSets(c->dev, &ai, &ds); if (vr) { fail(c, "vkAllocateDescriptorSets", vr, 0); return 0; }
        c->dsets_in_pool++; c->st.descriptor_sets++;
        VkWriteDescriptorSet w[UDR_MAX_BINDINGS]; VkDescriptorImageInfo ii[UDR_MAX_BINDINGS]; VkDescriptorBufferInfo bi[UDR_MAX_BINDINGS];
        for (u32 i = 0; i < p->nbind; i++) {
            const UdrBinding *b = &c->binds[p->bind[i].binding];
            if (b->type != p->bind[i].type) { fail(c, "draw/dispatch: binding type differs from the pipeline layout", 0, p->bind[i].binding); return 0; }
            VkWriteDescriptorSet x = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, p->bind[i].binding, 0, 1, k_desc[b->type], 0, 0, 0};
            if (b->type == UDR_BIND_UNIFORM_BUFFER || b->type == UDR_BIND_STORAGE_BUFFER) {
                Res *r = res_get(c, b->id, R_BUFFER);
                bi[i].buffer = r->buf; bi[i].offset = b->offset; bi[i].range = b->range ? b->range : VK_WHOLE_SIZE; x.pBufferInfo = &bi[i];
            } else {
                Res *r = b->type == UDR_BIND_SAMPLER ? 0 : res_get(c, b->id, R_IMAGE);
                Res *s = (b->type == UDR_BIND_SAMPLER) ? res_get(c, b->id, R_SAMPLER) : (b->type == UDR_BIND_COMBINED) ? res_get(c, b->sampler, R_SAMPLER) : 0;
                if ((b->type == UDR_BIND_SAMPLER || b->type == UDR_BIND_COMBINED) && !s) { fail(c, "draw/dispatch: sampler does not exist", 0, b->sampler); return 0; }
                ii[i].sampler = s ? s->smp : 0;
                ii[i].imageView = r ? image_view(c, r, b->base_mip, b->mip_count, b->base_layer, b->layer_count, b->view_format, 0, b->type == UDR_BIND_STORAGE_IMAGE ? 0 : b->swizzle) : 0;
                ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; x.pImageInfo = &ii[i];
            }
            w[i] = x;
        }
        vkUpdateDescriptorSets(c->dev, p->nbind, w, 0, 0);
        vkCmdBindDescriptorSets(c->cb, p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &ds, 0, 0);
        c->binds_dirty = 0;
    }
    if (c->push_dirty && p->push_bytes) {
        vkCmdPushConstants(c->cb, p->layout, p->compute ? VK_SHADER_STAGE_COMPUTE_BIT : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT), 0, p->push_bytes, c->push);
        c->push_dirty = 0;
    }
    return 1;
}
static void after_command(UdrCtx *c, Res *p, u32 op) {   /* storage writes of this command */
    for (u32 i = 0; i < p->nbind; i++) {
        const UdrBindingDesc *bd = &p->bind[i];
        u32 acc = bd->access ? bd->access : (bd->type == UDR_BIND_STORAGE_BUFFER || bd->type == UDR_BIND_STORAGE_IMAGE) ? (UDR_ACCESS_READ | UDR_ACCESS_WRITE) : UDR_ACCESS_READ;
        if (!(acc & (UDR_ACCESS_WRITE | UDR_ACCESS_ATOMIC))) continue;
        const UdrBinding *b = &c->binds[bd->binding];
        Res *r = res_get(c, b->id, (b->type == UDR_BIND_STORAGE_BUFFER) ? R_BUFFER : R_IMAGE);
        if (r) mark_write(c, r, op, !(acc & UDR_ACCESS_WRITE));
    }
}
static Res *cur_pipeline(UdrCtx *c, int compute) {
    Res *p = res_get(c, c->cur_pipe, R_PIPELINE);
    if (!p) { fail(c, "draw/dispatch without a pipeline", 0, c->cmd); return 0; }
    if (p->compute != compute) { fail(c, compute ? "dispatch with a graphics pipeline" : "draw with a compute pipeline", 0, c->cur_pipe); return 0; }
    return p;
}
static int draw_prologue(UdrCtx *c, Res **pp, u32 op) {
    if (!c->in_pass) { fail(c, "draw outside a pass", 0, c->cmd); return 0; }
    Res *p = cur_pipeline(c, 0); if (!p) return 0;
    if (p->ncolor != c->pass.ncolor) { fail(c, "draw: pipeline target count differs from the pass", 0, c->cur_pipe); return 0; }
    for (u32 i = 0; i < p->ncolor; i++) { Res *t = res_get(c, c->pass.color[i].image, R_IMAGE); u32 vf = c->pass.color[i].view_format ? c->pass.color[i].view_format : t ? t->fmt : 0;
        if (t && vf != p->color_format[i]) { fail(c, "draw: pipeline target format differs from the pass", 0, i); return 0; }
        if (!c->pass.color[i].image && p->color_format[i] != UDR_FMT_UNDEFINED) { fail(c, "draw: pipeline writes a target slot the pass leaves empty", 0, i); return 0; } }
    if (!c->have_viewport) { VkViewport v = {0, 0, (f32)c->pass.width, (f32)c->pass.height, 0, 1}; vkCmdSetViewport(c->cb, 0, 1, &v); c->have_viewport = 1; }
    if (!c->have_scissor) { VkRect2D s = {{0, 0}, {c->pass.width, c->pass.height}}; vkCmdSetScissor(c->cb, 0, 1, &s); c->have_scissor = 1; }
    if (!bind_for_command(c, p, op)) return 0;
    *pp = p; return 1;
}
static void op_draw(UdrCtx *c, const UdrDraw *a) {
    Res *p; if (!draw_prologue(c, &p, UDR_OP_DRAW)) return;
    vkCmdDraw(c->cb, a->vertex_count, a->instance_count ? a->instance_count : 1, a->first_vertex, a->first_instance);
    after_command(c, p, UDR_OP_DRAW); c->st.draws++;
}
static void op_draw_indexed(UdrCtx *c, const UdrDrawIndexed *a) {
    Res *ib = res_get(c, a->index_buffer, R_BUFFER);
    if (!ib || a->offset + (u64)(a->first_index + a->index_count) * (a->index_32bit ? 4 : 2) > ib->size) { fail(c, "DRAW_INDEXED: bad index buffer or range", 0, a->index_buffer); return; }
    Res *p; if (!draw_prologue(c, &p, UDR_OP_DRAW_INDEXED)) return;
    check_read(c, a->index_buffer, ib, UDR_OP_DRAW_INDEXED, 0);
    vkCmdBindIndexBuffer(c->cb, ib->buf, a->offset, a->index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(c->cb, a->index_count, a->instance_count ? a->instance_count : 1, a->first_index, a->vertex_offset, a->first_instance);
    after_command(c, p, UDR_OP_DRAW_INDEXED); c->st.draws++;
}
static void op_dispatch(UdrCtx *c, const UdrDispatch *a) {
    if (c->in_pass) { fail(c, "dispatch inside a pass", 0, c->cmd); return; }
    if (!begin(c)) return;
    Res *p = cur_pipeline(c, 1); if (!p || !bind_for_command(c, p, UDR_OP_DISPATCH)) return;
    vkCmdDispatch(c->cb, a->x, a->y, a->z);
    after_command(c, p, UDR_OP_DISPATCH); c->st.dispatches++;
}
static void op_barrier(UdrCtx *c, const UdrBarrier *a, const UdrId *ids, u64 avail) {
    if (c->in_pass) { fail(c, "BARRIER inside a pass (end the pass first)", 0, c->cmd); return; }
    if (!begin(c)) return;
    full_barrier(c); c->st.barriers_ir++;
    if (a->kind == UDR_BARRIER_ALL || !a->count) { c->epoch++; return; }
    for (u32 i = 0; i < a->count && (u64)(i + 1) * 4 <= avail; i++) { if (ids[i] < c->nres) c->res[ids[i]].wr_epoch = 0; }
}
static void op_readback(UdrCtx *c, const UdrReadback *a) {
    Res *r = res_get(c, a->image, R_IMAGE);
    if (!r || a->mip >= r->mips || (a->layer != UDR_READBACK_ALL && a->layer >= r->layers)) { fail(c, "READBACK: bad image", 0, a->image); return; }
    if (c->in_pass) { fail(c, "READBACK inside a pass", 0, a->image); return; }
    const FmtInfo *fi = &g_fmt[r->fmt];
    u32 w = r->w >> a->mip, h = r->h >> a->mip; if (!w) w = 1; if (!h) h = 1;
    u32 bw = (w + fi->block - 1) / fi->block, bh = (h + fi->block - 1) / fi->block;
    u64 bytes = (u64)bw * bh * fi->bytes;
    if (a->layer == UDR_READBACK_ALL) {   /* whole level: all layers / slices, then stencil */
        u32 layers = r->layers ? r->layers : 1, d3 = r->type == UDR_IMG_3D ? (r->d >> a->mip ? r->d >> a->mip : 1) : 1;
        u64 main = bytes * layers * d3, sten = (fi->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? (u64)w * h * layers * d3 : 0;
        if (!submit_wait(c) || !stage_ensure(c, main + sten) || !begin(c)) return;
        full_barrier(c); c->st.barriers_auto++;
        VkBufferImageCopy bic = {0, 0, 0, {fi->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT, a->mip, 0, r->type == UDR_IMG_3D ? 1 : layers}, {0, 0, 0}, {w, h, d3}};
        vkCmdCopyImageToBuffer(c->cb, r->img, VK_IMAGE_LAYOUT_GENERAL, c->stage, 1, &bic);
        if (sten) { bic.bufferOffset = main; bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; vkCmdCopyImageToBuffer(c->cb, r->img, VK_IMAGE_LAYOUT_GENERAL, c->stage, 1, &bic); }
        if (!submit_wait(c)) return;
        if (c->h.readback) c->h.readback(c->h.user, a->tag, w, h, r->fmt, c->stage_map, main + sten);
        return;
    }
    if (!submit_wait(c) || !stage_ensure(c, bytes) || !begin(c)) return;
    full_barrier(c); c->st.barriers_auto++;   /* readbacks are ordered after everything before them */
    VkBufferImageCopy bic = {0, 0, 0, {fi->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT, a->mip, a->layer, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(c->cb, r->img, VK_IMAGE_LAYOUT_GENERAL, c->stage, 1, &bic);
    if (!submit_wait(c)) return;
    if (c->h.readback) c->h.readback(c->h.user, a->tag, w, h, r->fmt, c->stage_map, bytes);
}

static void op_readback_buffer(UdrCtx *c, const UdrReadbackBuffer *a) {
    Res *r = res_get(c, a->buffer, R_BUFFER);
    if (!r || !a->bytes || a->offset + a->bytes > r->size) { fail(c, "READBACK_BUFFER: bad range", 0, a->buffer); return; }
    if (c->in_pass) { fail(c, "READBACK_BUFFER inside a pass", 0, a->buffer); return; }
    if (!submit_wait(c) || !stage_ensure(c, a->bytes) || !begin(c)) return;
    full_barrier(c); c->st.barriers_auto++;
    VkBufferCopy bc = {a->offset, 0, a->bytes};
    vkCmdCopyBuffer(c->cb, r->buf, c->stage, 1, &bc);
    if (!submit_wait(c)) return;
    if (c->h.readback) c->h.readback(c->h.user, a->tag, 0, 0, 0, c->stage_map, a->bytes);
}

/* ---------------------------------------------------------------- record loop */
#define NEED(T) if (len < 8 + sizeof(T)) { fail(c, "record too short", 0, rec->op); break; }
udr_u64 udr_execute(UdrCtx *c, const void *ir, udr_u64 bytes) {
    const u8 *p = (const u8 *)ir, *e = p + bytes;
    if (bytes >= 8 && p[0] == 'U' && p[1] == 'D' && p[2] == 'R' && p[3] == 'I' && p[4] == 'R' && p[5] == '0' && p[6] == '0' && p[7] == '1') p += 8;
    while (p + 8 <= e) {
        const UdrRec *rec = (const UdrRec *)p; u32 len = rec->size;
        if (len < 8 || (len & 3) || p + len > e) { fail(c, "malformed record", 0, (u64)(p - (const u8 *)ir)); break; }
        const u8 *pl = p + 8; u64 plen = len - 8;
        c->cmd++; c->st.records++;
        switch (rec->op) {
        case UDR_OP_NOP: case UDR_OP_MARKER: break;
        case UDR_OP_IMAGE_CREATE: NEED(UdrImageCreate) op_image_create(c, (const UdrImageCreate *)pl); break;
        case UDR_OP_BUFFER_CREATE: NEED(UdrBufferCreate) op_buffer_create(c, (const UdrBufferCreate *)pl); break;
        case UDR_OP_SAMPLER_CREATE: NEED(UdrSamplerCreate) op_sampler_create(c, (const UdrSamplerCreate *)pl); break;
        case UDR_OP_SHADER_CREATE: NEED(UdrShaderCreate) op_shader_create(c, (const UdrShaderCreate *)pl, (const u32 *)(pl + sizeof(UdrShaderCreate)), plen - sizeof(UdrShaderCreate)); break;
        case UDR_OP_PIPELINE_CREATE: NEED(UdrPipelineCreate) op_pipeline_create(c, (const UdrPipelineCreate *)pl); break;
        case UDR_OP_DESTROY: NEED(UdrDestroy) op_destroy(c, ((const UdrDestroy *)pl)->id); break;
        case UDR_OP_BUFFER_WRITE: NEED(UdrBufferWrite) op_buffer_write(c, (const UdrBufferWrite *)pl, pl + sizeof(UdrBufferWrite), plen - sizeof(UdrBufferWrite)); break;
        case UDR_OP_IMAGE_WRITE: NEED(UdrImageWrite) op_image_write(c, (const UdrImageWrite *)pl, pl + sizeof(UdrImageWrite), plen - sizeof(UdrImageWrite)); break;
        case UDR_OP_BUFFER_FILL: NEED(UdrBufferFill) op_buffer_fill(c, (const UdrBufferFill *)pl); break;
        case UDR_OP_BUFFER_COPY: NEED(UdrBufferCopy) op_buffer_copy(c, (const UdrBufferCopy *)pl); break;
        case UDR_OP_IMAGE_COPY: NEED(UdrImageCopy) op_image_copy(c, (const UdrImageCopy *)pl); break;
        case UDR_OP_IMAGE_CLEAR: NEED(UdrImageClear) op_image_clear(c, (const UdrImageClear *)pl); break;
        case UDR_OP_IMAGE_TO_BUFFER: NEED(UdrImageToBuffer) op_image_to_buffer(c, (const UdrImageToBuffer *)pl); break;
        case UDR_OP_BEGIN_PASS: NEED(UdrBeginPass) op_begin_pass(c, (const UdrBeginPass *)pl); break;
        case UDR_OP_END_PASS: if (!c->in_pass) fail(c, "END_PASS without a pass", 0, c->cmd); end_pass_if_open(c); break;
        case UDR_OP_BIND_PIPELINE: NEED(UdrBindPipeline) { UdrId id = ((const UdrBindPipeline *)pl)->pipeline; if (id != c->cur_pipe) { c->cur_pipe = id; c->pipe_bound = 0; } } break;
        case UDR_OP_BIND_RESOURCES: { NEED(UdrBindResources)
            u32 n = ((const UdrBindResources *)pl)->count; const UdrBinding *b = (const UdrBinding *)(pl + sizeof(UdrBindResources));
            if ((u64)n * sizeof(UdrBinding) + sizeof(UdrBindResources) > plen) { fail(c, "BIND_RESOURCES: truncated", 0, n); break; }
            for (u32 i = 0; i < n; i++) { if (b[i].binding >= UDR_MAX_BINDINGS) { fail(c, "BIND_RESOURCES: binding out of range", 0, b[i].binding); continue; }
                c->binds[b[i].binding] = b[i]; c->bind_valid |= 1ull << b[i].binding; }
            c->binds_dirty = 1; break; }
        case UDR_OP_PUSH_CONSTANTS: { NEED(UdrPushConstants) const UdrPushConstants *a = (const UdrPushConstants *)pl;
            if (a->offset + a->bytes > 128 || a->bytes > plen - sizeof *a) { fail(c, "PUSH_CONSTANTS: out of range", 0, a->offset); break; }
            m_copy(c->push + a->offset, pl + sizeof *a, a->bytes); c->push_dirty = 1; break; }
        case UDR_OP_SET_VIEWPORT: { NEED(UdrViewport) if (!c->in_pass) { fail(c, "SET_VIEWPORT outside a pass", 0, 0); break; }
            const UdrViewport *v = (const UdrViewport *)pl; VkViewport vv = {v->x, v->y, v->w, v->h, v->min_depth, v->max_depth}; vkCmdSetViewport(c->cb, 0, 1, &vv); c->have_viewport = 1; break; }
        case UDR_OP_SET_SCISSOR: { NEED(UdrScissor) if (!c->in_pass) { fail(c, "SET_SCISSOR outside a pass", 0, 0); break; }
            const UdrScissor *s = (const UdrScissor *)pl; VkRect2D r = {{s->x, s->y}, {s->w, s->h}}; vkCmdSetScissor(c->cb, 0, 1, &r); c->have_scissor = 1; break; }
        case UDR_OP_CLEAR_ATTACHMENTS: { NEED(UdrClearAttachments) if (!c->in_pass) { fail(c, "CLEAR_ATTACHMENTS outside a pass", 0, 0); break; }
            const UdrClearAttachments *a = (const UdrClearAttachments *)pl; VkClearAttachment ca[UDR_MAX_TARGETS + 1]; u32 n = 0;
            for (u32 i = 0; i < c->pass.ncolor && i < UDR_MAX_TARGETS; i++) if (a->color_mask & (1u << i)) {
                VkClearAttachment x = {VK_IMAGE_ASPECT_COLOR_BIT, i, {{{0}}}}; m_copy(x.clearValue.color.float32, a->color, 16); ca[n++] = x; }
            if (a->aspects && c->pass.depth.image) {
                Res *dr = res_get(c, c->pass.depth.image, R_IMAGE);
                VkImageAspectFlags asp = ((a->aspects & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((a->aspects & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                if (dr) asp &= dr->aspect;
                if (asp) { VkClearAttachment x = {asp, 0, {{{0}}}}; x.clearValue.depthStencil.depth = a->depth; x.clearValue.depthStencil.stencil = a->stencil; ca[n++] = x; }
            }
            VkClearRect cr = {{{a->x, a->y}, {a->w, a->h}}, 0, 1};
            if (n && a->w && a->h) vkCmdClearAttachments(c->cb, n, ca, 1, &cr);
            break; }
        case UDR_OP_SET_BLEND_CONSTANTS: NEED(UdrBlendConstants) if (begin(c)) vkCmdSetBlendConstants(c->cb, ((const UdrBlendConstants *)pl)->c); break;
        case UDR_OP_SET_STENCIL_REF: NEED(UdrStencilRef) if (begin(c)) vkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, ((const UdrStencilRef *)pl)->ref); break;
        case UDR_OP_DRAW: NEED(UdrDraw) op_draw(c, (const UdrDraw *)pl); break;
        case UDR_OP_DRAW_INDEXED: NEED(UdrDrawIndexed) op_draw_indexed(c, (const UdrDrawIndexed *)pl); break;
        case UDR_OP_DISPATCH: NEED(UdrDispatch) op_dispatch(c, (const UdrDispatch *)pl); break;
        case UDR_OP_BARRIER: NEED(UdrBarrier) op_barrier(c, (const UdrBarrier *)pl, (const UdrId *)(pl + sizeof(UdrBarrier)), plen - sizeof(UdrBarrier)); break;
        case UDR_OP_READBACK: NEED(UdrReadback) op_readback(c, (const UdrReadback *)pl); break;
        case UDR_OP_READBACK_BUFFER: NEED(UdrReadbackBuffer) op_readback_buffer(c, (const UdrReadbackBuffer *)pl); break;
        case UDR_OP_FLUSH: submit_wait(c); break;
        default: fail(c, "unknown record", 0, rec->op); break;
        }
        p += len;
    }
    return c->st.errors;
}

/* ---------------------------------------------------------------- device */
UdrCtx *udr_create(const UdrHost *host, const UdrConfig *cfg) {
    if (!host || !host->alloc || !host->free) return 0;
    UdrCtx *c = (UdrCtx *)host->alloc(sizeof(UdrCtx)); if (!c) return 0;
    m_zero(c, sizeof *c); c->h = *host; if (cfg) c->cfg = *cfg; else c->cfg.flags = UDR_CFG_HAZARD_REPORT;
    c->epoch = 1;
    vkGetInstanceProcAddr = host->vk_get_instance_proc_addr ? (PFN_vkGetInstanceProcAddr)host->vk_get_instance_proc_addr() : 0;
    if (!vkGetInstanceProcAddr) { fail(c, "no Vulkan loader", 0, 0); host->free(c); return 0; }
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(0, "vkCreateInstance");
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Until Dawn", 1, "udr", UDR_IR_VERSION, VK_API_VERSION_1_3};
    const char *iext[2] = {"VK_KHR_surface", "VK_KHR_win32_surface"}; int want_present = host->present_hwnd != 0;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai, 0, 0, want_present ? 2u : 0u, want_present ? iext : 0};
    VkResult r = vkCreateInstance ? vkCreateInstance(&ici, 0, &c->inst) : VK_ERROR_INITIALIZATION_FAILED;
    if (r) { fail(c, "vkCreateInstance (Vulkan 1.3 needed)", r, 0); host->free(c); return 0; }
#define UDR_LI(f) f = (PFN_##f)vkGetInstanceProcAddr(c->inst, #f);
    UDR_VK_INST(UDR_LI)
    vkDestroyInstance_ = vkDestroyInstance;
    if (want_present) {
        UDR_VK_INST_PRESENT(UDR_LI)
        vkCreateWin32SurfaceKHR_ = (UdrPfnCreateWin32Surface)vkGetInstanceProcAddr(c->inst, "vkCreateWin32SurfaceKHR");
        UdrWin32SurfaceCreateInfo sci = {(VkStructureType)1000009000 /* VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR */, 0, 0, host->present_hinstance, host->present_hwnd};
        VkResult sr = vkCreateWin32SurfaceKHR_ ? vkCreateWin32SurfaceKHR_(c->inst, &sci, 0, &c->surf) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (sr) { c->surf = 0; logs(c, "udr: no window surface; presentation off\n"); }
    }
    u32 n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(c->inst, &n, pds);
    for (u32 i = 0; i < n && !c->pd; i++) {
        if (cfg && cfg->prefer_device && i + 1 != cfg->prefer_device) continue;
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, 0};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13, {0}};
        vkGetPhysicalDeviceFeatures2(pds[i], &f2);
        if (!f13.dynamicRendering) continue;
        u32 nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (u32 q = 0; q < nq; q++) if ((qp[q].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { c->pd = pds[i]; c->qfam = q; break; }
        if (c->pd) { Str s = {c->dev_name, 0, sizeof c->dev_name}; s_put(&s, pp.deviceName); c->max_aniso = pp.limits.maxSamplerAnisotropy; c->max_storage_range = pp.limits.maxStorageBufferRange; }
    }
    if (!c->pd) { fail(c, "no Vulkan 1.3 device with dynamic rendering", 0, n); vkDestroyInstance_(c->inst, 0); host->free(c); return 0; }
    vkGetPhysicalDeviceMemoryProperties(c->pd, &c->mp);
    VkPhysicalDeviceFeatures2 want = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, 0, {0}};
    VkPhysicalDeviceVulkan13Features w13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, 0};
    VkPhysicalDeviceFeatures2 have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, 0, {0}}; vkGetPhysicalDeviceFeatures2(c->pd, &have);
    want.pNext = &w13; w13.dynamicRendering = VK_TRUE;
    want.features.fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics; want.features.vertexPipelineStoresAndAtomics = have.features.vertexPipelineStoresAndAtomics;
    want.features.independentBlend = have.features.independentBlend; want.features.depthClamp = have.features.depthClamp;
    want.features.depthBiasClamp = have.features.depthBiasClamp; c->bias_clamp_ok = have.features.depthBiasClamp != 0;   /* the PS4's polygon offset clamp */
    want.features.samplerAnisotropy = have.features.samplerAnisotropy; c->aniso = have.features.samplerAnisotropy != 0; want.features.textureCompressionBC = have.features.textureCompressionBC;
    want.features.imageCubeArray = have.features.imageCubeArray; want.features.shaderImageGatherExtended = have.features.shaderImageGatherExtended;   /* texel offsets of *_O samples (STATUS 41) */ want.features.shaderStorageImageWriteWithoutFormat = have.features.shaderStorageImageWriteWithoutFormat;
    want.features.shaderStorageImageReadWithoutFormat = have.features.shaderStorageImageReadWithoutFormat; want.features.shaderInt64 = have.features.shaderInt64;   /* translated game programs */
    float prio = 1.0f; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, c->qfam, 1, &prio};
    if (c->surf) { VkBool32 ps = 0; vkGetPhysicalDeviceSurfaceSupportKHR(c->pd, c->qfam, c->surf, &ps); c->present_ok = ps != 0; if (!ps) logs(c, "udr: the queue cannot present to the window; presentation off\n"); }
    const char *dext[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want, 0, 1, &qci, 0, 0, c->present_ok ? 1u : 0u, c->present_ok ? dext : 0, 0};
    if ((r = vkCreateDevice(c->pd, &dci, 0, &c->dev))) { fail(c, "vkCreateDevice", r, 0); vkDestroyInstance_(c->inst, 0); host->free(c); return 0; }
#define UDR_LD(f) f = (PFN_##f)vkGetDeviceProcAddr(c->dev, #f);
    UDR_VK_DEV(UDR_LD)
    if (c->present_ok) { UDR_VK_DEV_PRESENT(UDR_LD) VkFenceCreateInfo afi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0}; vkCreateFence(c->dev, &afi, 0, &c->acq_fence); }
    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->q);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c->qfam};
    vkCreateCommandPool(c->dev, &cpi, 0, &c->pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(c->dev, &cai, &c->cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0}; vkCreateFence(c->dev, &fci, 0, &c->fence);
    VkDescriptorPoolSize ps[6] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 65536}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8192}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32768}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4096, 6, ps};
    if ((r = vkCreateDescriptorPool(c->dev, &dpi, 0, &c->dpool))) { fail(c, "vkCreateDescriptorPool", r, 0); udr_destroy(c); return 0; }
    if (!stage_ensure(c, 64ull << 20)) { udr_destroy(c); return 0; }
    return c;
}
void udr_finish(UdrCtx *c) { if (c) { end_pass_if_open(c); submit_wait(c); } }
const UdrStats *udr_stats(const UdrCtx *c) {
    UdrStats *st = (UdrStats *)&c->st; st->pool_bytes = st->pool_used = 0; st->stage_bytes = c->stage_size;
    for (u32 i = 0; i < c->nmpool; i++) { u64 f = 0; for (u32 j = 0; j < c->mpool[i].nfr; j++) f += c->mpool[i].fr[j].size; st->pool_bytes += c->mpool[i].size; st->pool_used += c->mpool[i].used - f; }
    st->live_images = st->live_image_bytes = st->live_buffers = st->live_buffer_bytes = 0;
    for (u32 i = 0; i < c->nres; i++) {
        if (c->res[i].kind == R_IMAGE) { st->live_images++; st->live_image_bytes += c->res[i].psize; }
        else if (c->res[i].kind == R_BUFFER) { st->live_buffers++; st->live_buffer_bytes += c->res[i].psize; }
    }
    return &c->st;
}
udr_u32 udr_hazards(const UdrCtx *c, const UdrHazard **list) { *list = c->haz; return c->nhaz; }
const char *udr_device_name(const UdrCtx *c) { return c->dev_name; }
udr_u64 udr_max_storage_range(const UdrCtx *c) { return c->max_storage_range; }
const char *udr_last_error(const UdrCtx *c) { return c->err; }
void udr_destroy(UdrCtx *c) {
    if (!c) return;
    if (c->dev) {
        if (c->recording) { vkEndCommandBuffer(c->cb); c->recording = 0; }
        vkDeviceWaitIdle(c->dev);
        free_dead(c);
        for (u32 i = 0; i < c->nres; i++) if (c->res[i].kind) res_free(c, &c->res[i]);
        if (c->stage) { vkUnmapMemory(c->dev, c->stage_mem); vkDestroyBuffer(c->dev, c->stage, 0); vkFreeMemory(c->dev, c->stage_mem, 0); }
        if (c->dpool) vkDestroyDescriptorPool(c->dev, c->dpool, 0);
        if (c->fence) vkDestroyFence(c->dev, c->fence, 0);
        if (c->acq_fence) vkDestroyFence(c->dev, c->acq_fence, 0);
        if (c->sw) vkDestroySwapchainKHR(c->dev, c->sw, 0);
        if (c->pool) vkDestroyCommandPool(c->dev, c->pool, 0);
        for (u32 i = 0; i < c->nmpool; i++) { vkFreeMemory(c->dev, c->mpool[i].mem, 0); if (c->mpool[i].fr) c->h.free(c->mpool[i].fr); }
        vkDestroyDevice(c->dev, 0);
    }
    if (c->surf) vkDestroySurfaceKHR(c->inst, c->surf, 0);
    if (c->inst) vkDestroyInstance_(c->inst, 0);
    if (c->res) c->h.free(c->res);
    if (c->dead) c->h.free(c->dead);
    c->h.free(c);
}

/* ---------------------------------------------------------------- presentation */
static int fmt_is_srgb(u32 f) { return f == UDR_FMT_R8G8B8A8_SRGB || f == UDR_FMT_B8G8R8A8_SRGB; }
/* the swapchain: B8G8R8A8 with the encoding of the image shown (an sRGB image into an sRGB swapchain, a UNORM one into a
   UNORM swapchain, so the blit copies the stored values); FIFO, as the game paces its flips itself */
static int sw_make(UdrCtx *c, VkFormat want) {
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->pd, c->surf, &caps);
    VkSurfaceFormatKHR fmts[32]; u32 nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surf, &nf, fmts);
    if (!nf) { fail(c, "present: the window offers no surface format", 0, 0); return 0; }
    VkSurfaceFormatKHR sf = fmts[0]; int found = 0;
    for (u32 i = 0; i < nf; i++) if (fmts[i].format == want) { sf = fmts[i]; found = 1; }
    if (!found) for (u32 i = 0; i < nf; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) sf = fmts[i];
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 1920; ext.height = 1080; }
    if (!ext.width || !ext.height) return 2;   /* minimised */
    u32 n = caps.minImageCount + 1; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount; if (n > 8) n = 8;
    VkSwapchainKHR old = c->sw;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, c->surf, n, sf.format, sf.colorSpace, ext, 1,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, caps.currentTransform,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, old};
    VkResult r = vkCreateSwapchainKHR(c->dev, &ci, 0, &c->sw);
    if (old) vkDestroySwapchainKHR(c->dev, old, 0);
    if (r) { c->sw = 0; fail(c, "vkCreateSwapchainKHR", r, 0); return 0; }
    c->sw_fmt = sf.format; c->sw_ext = ext; c->sw_n = 8; vkGetSwapchainImagesKHR(c->dev, c->sw, &c->sw_n, c->sw_img);
    char t[160]; Str q = {t, 0, sizeof t}; s_put(&q, "udr: swapchain "); s_dec(&q, ext.width); s_put(&q, "x"); s_dec(&q, ext.height); s_put(&q, " format "); s_dec(&q, (u64)sf.format);
    s_put(&q, ", "); s_dec(&q, c->sw_n); s_put(&q, " images\n"); logs(c, t);
    return 1;
}
int udr_present(UdrCtx *c, UdrId image, udr_u32 readback_tag) { return udr_present_region(c, image, 0, 0, readback_tag); }
int udr_present_region(UdrCtx *c, UdrId image, udr_u32 src_w, udr_u32 src_h, udr_u32 readback_tag) {
    if (!c || !c->present_ok) return 0;
    Res *r = res_get(c, image, R_IMAGE);
    if (!r) { fail(c, "present: no such image", 0, image); return 0; }
    end_pass_if_open(c);
    if (!submit_wait(c)) return 0;
    VkFormat want = fmt_is_srgb(r->fmt) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    if (!c->sw || c->sw_fmt != want) { vkDeviceWaitIdle(c->dev); int k = sw_make(c, want); if (k != 1) return k == 2 ? 2 : 0; }
    u32 idx = 0;
    vkResetFences(c->dev, 1, &c->acq_fence);
    VkResult ar = vkAcquireNextImageKHR(c->dev, c->sw, 1000000000ull, 0, c->acq_fence, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(c->dev); sw_make(c, want); return 2; }
    if (ar < 0) { fail(c, "vkAcquireNextImageKHR", ar, 0); return 0; }
    vkWaitForFences(c->dev, 1, &c->acq_fence, VK_TRUE, ~0ull);
    VkExtent2D e = c->sw_ext; u64 rb_bytes = (u64)e.width * e.height * 4;
    if (readback_tag && !stage_ensure(c, rb_bytes)) return 0;
    if (!begin(c)) return 0;
    full_barrier(c);
    VkImage si = c->sw_img[idx];
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, si, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    /* letterbox: the image scaled to the window with its aspect ratio kept (1:1 when the sizes are equal) */
    u32 sw = src_w && src_w <= r->w ? src_w : r->w, sh = src_h && src_h <= r->h ? src_h : r->h, dw = e.width, dh = (u32)((u64)e.width * sh / sw);
    if (dh > e.height) { dh = e.height; dw = (u32)((u64)e.height * sw / sh); }
    i32 ox = (i32)(e.width - dw) / 2, oy = (i32)(e.height - dh) / 2;
    VkClearColorValue black = {{0, 0, 0, 1}};
    vkCmdClearColorImage(c->cb, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &b.subresourceRange);
    VkMemoryBarrier tb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tb, 0, 0, 0, 0);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(i32)sw, (i32)sh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{ox, oy, 0}, {ox + (i32)dw, oy + (i32)dh, 1}}};
    vkCmdBlitImage(c->cb, r->img, VK_IMAGE_LAYOUT_GENERAL, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, (dw == sw && dh == sh) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (readback_tag) {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.oldLayout = last; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
        VkBufferImageCopy bic = {c->stage_used, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {e.width, e.height, 1}};
        vkCmdCopyImageToBuffer(c->cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->stage, 1, &bic);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = 0; b.oldLayout = last; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b);
    u64 rb_off = c->stage_used;
    if (!submit_wait(c)) return 0;   /* the copy has finished: the present needs no semaphore */
    if (readback_tag && c->h.readback) c->h.readback(c->h.user, readback_tag, e.width, e.height, c->sw_fmt == VK_FORMAT_B8G8R8A8_SRGB ? UDR_FMT_B8G8R8A8_SRGB : UDR_FMT_B8G8R8A8_UNORM, c->stage_map + rb_off, rb_bytes);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 0, 0, 1, &c->sw, &idx, 0};
    VkResult pr = vkQueuePresentKHR(c->q, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(c->dev); sw_make(c, want); return pr == VK_SUBOPTIMAL_KHR ? 1 : 2; }
    if (pr < 0) { fail(c, "vkQueuePresentKHR", pr, 0); return 0; }
    c->st.submits += 0;
    return 1;
}
