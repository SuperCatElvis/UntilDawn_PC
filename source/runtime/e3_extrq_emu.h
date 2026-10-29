/* e3_extrq_emu.h - EXTRQ trap emulation (SSE4a, AMD-only; Intel CPUs raise #UD): shared by e3_loader.c and the tests (STATUS 88) */
static int g_extrq_emulated;
/* EXTRQ xmm1, xmm2 (66 [REX] 0F 79 /r, mod=11): xmm1[63:0] = (xmm1 >> idx) & mask(len) */
static int try_emulate_extrq(u8 *rip, u8 *xmm_base /* 16 bytes per reg */, u64 *rip_out) {
    int i = 0; u8 rex = 0;
    if (rip[i] != 0x66) return 0;
    i++;
    if ((rip[i] & 0xF0) == 0x40) rex = rip[i++];
    if (rip[i] != 0x0F || rip[i + 1] != 0x79) return 0;
    u8 modrm = rip[i + 2];
    if ((modrm >> 6) != 3) return 0;
    int dst = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    int src = (modrm & 7) | ((rex & 1) ? 8 : 0);
    u64 ctl = *(u64 *)(xmm_base + 16 * src);
    u32 len = ctl & 63, idx = (ctl >> 8) & 63;
    if (!len) len = 64;
    u64 v = *(u64 *)(xmm_base + 16 * dst);
    v >>= idx;
    if (len < 64) v &= (1ull << len) - 1;
    *(u64 *)(xmm_base + 16 * dst) = v;
    *rip_out = (u64)(rip + i + 3);
    g_extrq_emulated++;
    return 1;
}

