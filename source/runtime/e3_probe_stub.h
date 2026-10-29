/* e3_probe_stub.h - a diagnostic probe stub for the game's code (used by e3_loader.c, tested by probe_stub_test.c). Ours.
   The stub saves the red zone, rflags, every general register and xmm0-15, calls handler(P, id) with the Windows x64
   calling convention (P = the save area: xmm0..15 at P[0..31], then r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx
   rcx rax rflags at P[32..47]; the probe point's rsp = (u8 *)P + 0x200), restores everything, runs the displaced
   position-independent instruction bytes and jumps back. Needs u8/u32/u64/i32 and mem_copy. */
static u8 *probe_emit_stub(u8 *w, u32 id, const u8 *displaced, int nd, u8 *back, void *handler) {
    static const u8 save[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff,   /* lea rsp,[rsp-0x80] */
                              0x9c, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,   /* pushfq; push rax rcx rdx rbx rbp rsi rdi */
                              0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                              0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00};        /* sub rsp,0x100 */
    mem_copy(w, save, sizeof save); w += sizeof save;
    for (int i = 0; i < 16; i++) {   /* movups [rsp+16*i], xmm_i */
        if (i >= 8) *w++ = 0x44;
        *w++ = 0x0f; *w++ = 0x11; *w++ = (u8)(0x84 | ((i & 7) << 3)); *w++ = 0x24; *(u32 *)w = (u32)(16 * i); w += 4;
    }
    static const u8 call1[] = {0x48, 0x89, 0xe3, 0x48, 0x89, 0xd9, 0xba};       /* mov rbx,rsp; mov rcx,rbx; mov edx,imm32 */
    mem_copy(w, call1, sizeof call1); w += sizeof call1; *(u32 *)w = id; w += 4;
    static const u8 call2[] = {0x48, 0x83, 0xe4, 0xf0, 0x48, 0x83, 0xec, 0x20, 0x48, 0xb8};   /* and rsp,-16; sub rsp,32; movabs rax,imm64 */
    mem_copy(w, call2, sizeof call2); w += sizeof call2; *(u64 *)w = (u64)handler; w += 8;
    *w++ = 0xff; *w++ = 0xd0;                                                    /* call rax */
    *w++ = 0x48; *w++ = 0x89; *w++ = 0xdc;                                       /* mov rsp,rbx */
    for (int i = 0; i < 16; i++) {   /* movups xmm_i, [rsp+16*i] */
        if (i >= 8) *w++ = 0x44;
        *w++ = 0x0f; *w++ = 0x10; *w++ = (u8)(0x84 | ((i & 7) << 3)); *w++ = 0x24; *(u32 *)w = (u32)(16 * i); w += 4;
    }
    static const u8 rest[] = {0x48, 0x81, 0xc4, 0x00, 0x01, 0x00, 0x00,          /* add rsp,0x100 */
                              0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
                              0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d,    /* pop ...; popfq */
                              0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};   /* lea rsp,[rsp+0x80] */
    mem_copy(w, rest, sizeof rest); w += sizeof rest;
    mem_copy(w, displaced, (u64)nd); w += nd;
    *w++ = 0xe9; *(i32 *)w = (i32)(back - (w + 4)); w += 4;
    return (u8 *)(((u64)w + 15) & ~15ull);
}
