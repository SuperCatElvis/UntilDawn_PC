/* e6_compat.h - small hardware-compatibility helpers of the renderer (STATUS 88), portable so the host tests use the same code.
   Included by e6_vk.c (Windows build) and by compat_host_test.c (Linux). */
/* STATUS 88: a pipeline cache file is only given to the driver when its header names this GPU and driver build
   (vendor, device, pipelineCacheUUID): drivers are meant to ignore foreign data, but a check here is explicit and logged */
static int pcache_header_ok_for(const void *data, u64 n, u32 vendor_id, u32 device_id, const u8 *uuid, int *same_out) {
    const u8 *d = (const u8 *)data; if (n < 32) return 0;
    u32 hlen = *(const u32 *)d, hver = *(const u32 *)(d + 4), vend = *(const u32 *)(d + 8), devid = *(const u32 *)(d + 12);
    if (hlen < 32 || hver != 1 /* VK_PIPELINE_CACHE_HEADER_VERSION_ONE */) { *same_out = 0; return 0; }
    int same = vend == vendor_id && devid == device_id; for (int i = 0; i < 16; i++) if (d[16 + i] != uuid[i]) same = 0; *same_out = same;
    return 1;
}
/* does the module declare a cross-lane capability (GroupNonUniformShuffle 65 / Ballot 64)? OpCapability (17) instructions
   come first in a module (STATUS 88: such shaders are asked to run in 32-lane subgroups where the GPU could use fewer) */
static int spv_xlane(const u32 *spv, u32 n) {
    for (u32 i = 5; i < n; ) { u32 op = spv[i] & 0xFFFF, len = spv[i] >> 16; if (!len) break;
        if (op == 17) { if (spv[i + 1] == 65 || spv[i + 1] == 64) return 1; } else if (op != 11 && op != 10) break;   /* past OpCapability / OpExtension / OpExtInstImport */
        i += len; }
    return 0;
}
