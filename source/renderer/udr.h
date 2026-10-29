/* udr.h - UD renderer: our Vulkan backend that executes the IR in udr_ir.h.
 * Independent of E6 (no PM4, no GPU addresses, no shader translation at run time). Ours. */
#ifndef UDR_H
#define UDR_H
#include "udr_ir.h"

/* What the backend needs from the program around it (the game executable, or a test runner). No C library is used. */
typedef struct {
    void *(*alloc)(udr_u64 bytes);             /* zeroed memory */
    void (*free)(void *p);
    void (*log)(const char *text);
    void *(*vk_get_instance_proc_addr)(void);  /* returns the loader's vkGetInstanceProcAddr, or 0 */
    /* UDR_OP_READBACK: pixels of one mip / layer, rows tightly packed (row bytes = width * texel bytes for
       uncompressed formats) */
    void (*readback)(void *user, udr_u32 tag, udr_u32 width, udr_u32 height, udr_u32 format, const void *pixels, udr_u64 bytes);
    void *user;
    /* optional, Windows: a window to present into (udr_present). Both 0: no presentation (tests, tools) */
    void *present_hinstance, *present_hwnd;
} UdrHost;

typedef struct UdrCtx UdrCtx;

enum {
    UDR_CFG_HAZARD_REPORT = 1,    /* report reads of data written since the last barrier (default on) */
    UDR_CFG_HAZARD_BARRIER = 2,   /* also insert a barrier there (safety net; off in validation runs) */
    UDR_CFG_NO_ANISO = 4,         /* diagnostic: samplers without anisotropic filtering (the behaviour before STATUS 28a) */
};
typedef struct { udr_u32 flags; udr_u32 prefer_device; /* 0: first suitable */ udr_u32 log_image_mb; /* log images of at least this many MB (0: 64) */ } UdrConfig;

typedef struct {
    udr_u64 records, draws, dispatches, passes, pipelines, descriptor_sets, barriers_ir, barriers_auto, uploads_bytes;
    udr_u64 hazards, submits, errors;
    udr_u64 destroyed, pool_reused, dedicated;   /* DESTROY records; allocations placed in freed pool ranges; resources with their own allocation */
    udr_u64 pool_bytes, pool_used, stage_bytes;   /* device memory blocks allocated, bytes in use in them now, staging buffer size */
    udr_u64 live_images, live_image_bytes, live_buffers, live_buffer_bytes;   /* resources alive now and their memory */
} UdrStats;

typedef struct { udr_u32 cmd, op; UdrId resource; udr_u32 writer_cmd, writer_op; } UdrHazard;

UdrCtx *udr_create(const UdrHost *host, const UdrConfig *cfg);   /* 0 on failure (reason logged) */
/* executes a record stream (with or without the "UDRIR001" header); returns the number of errors so far */
udr_u64 udr_execute(UdrCtx *c, const void *ir, udr_u64 bytes);
void udr_finish(UdrCtx *c);                                       /* submits outstanding work and waits */
/* shows an image in the host's window (UdrHost.present_hwnd): everything recorded so far is run, then the image's level 0
   is copied to the window's swapchain image (scaled with the aspect ratio kept, black borders; an exact copy when the
   window has the image's size), and presented. With readback_tag != 0 the swapchain image's pixels (B8G8R8A8, the
   window's size) also go to the host's readback callback with that tag, before presenting - the test of what reaches
   the window. Returns 1 presented, 2 skipped (window minimised or resized: try again next frame), 0 failed (logged). */
int udr_present(UdrCtx *c, UdrId image, udr_u32 readback_tag);
/* the same for the image's top-left src_w x src_h texels (the display's size: a 1920x1080 display buffer is a
   1920x1088 image, whose last 8 rows are not part of the picture); 0 = the whole image */
int udr_present_region(UdrCtx *c, UdrId image, udr_u32 src_w, udr_u32 src_h, udr_u32 readback_tag);
const UdrStats *udr_stats(const UdrCtx *c);
udr_u32 udr_hazards(const UdrCtx *c, const UdrHazard **list);    /* the first ones recorded */
const char *udr_device_name(const UdrCtx *c);
udr_u64 udr_max_storage_range(const UdrCtx *c);   /* the largest storage-buffer binding (maxStorageBufferRange) */
const char *udr_last_error(const UdrCtx *c);
void udr_destroy(UdrCtx *c);
udr_u32 udr_format_bytes(udr_u32 fmt);                            /* bytes per texel (per 4x4 block for BC) */
udr_u32 udr_format_vk(udr_u32 fmt);                               /* its VkFormat (test tools compare with E6 snaps) */
#endif
