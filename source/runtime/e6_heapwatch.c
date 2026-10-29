/* e6_heapwatch.c - stray-write detector for the streaming crash (--watch-heap, STATUS 40). Ours.
 *
 * The four crashes of 18-20 October have one signature: a streamed object at the very start of a 4 KB page has its
 * vtable pointer reduced by exactly 0x10 (it then points at the vtable's two header slots, which are 0), while its
 * neighbours are intact; later the lump code calls through it and jumps to address 0. Who writes there is unknown:
 * our layer (CPU-side GPU writes, file reads, zero fills, the GPU itself through imported memory) or a game thread
 * with a stale pointer.
 *
 * A background thread reads the first 8 bytes of every page of game memory over and over. When one changes from a
 * vtable pointer V to V - 0x10 (or, more loosely, from an eboot pointer to a slightly smaller one), it reports the page,
 * the flips between which it changed, the page's mapping history, and every write our own layer logged into that page:
 *   - CPU side (this file's journal, hw_note): labels and other command-processor writes done on the CPU, file reads,
 *     zero fills of fresh mappings;
 *   - GPU side (E6's GPU-write log, gw_note in e6_vk.c): shader stores to game memory, copies, fills, write-backs.
 * If none of ours touched the page in that window, the writer is (almost certainly) game code.
 *
 * It only reads game memory; it changes nothing. Off unless --watch-heap is given. */

/* ---- the journal of writes our layer makes into game memory (CPU side) ---- */
enum { HWK_CP = 1, HWK_FILE, HWK_ZERO, HWK_CE, HWK_DMA, HWK_N };
static const char *const g_hwk_name[HWK_N] = {"?", "command-processor write on the CPU (label / WRITE_DATA / occlusion)", "file read", "zero fill of a fresh mapping",
                                              "constant-RAM dump on the CPU", "CP DMA copy / fill on the CPU"};
typedef struct { u64 lo, hi, flip; const char *tn; u32 kind; } HwEv;
enum { HW_EV = 1 << 16 };
static HwEv g_hw_ev[HW_EV]; static u64 g_hw_nev;
static int g_hw_on;
#ifndef HAVE_SENT_DECL
static u64 g_sent_lo; enum { SENT_PAGES = 8192 };   /* t120 sentinel (below); the scanner skips it */
#endif
static void (*g_hw_note_hook)(u64 a, u64 n);   /* t107g: the late-write check's journal of our own writes (e6_latewrite.c) */
static void hw_note(u64 a, u64 n, int kind) {
    g2s_memo_wrote(a, a + n);   /* our writes into game memory: the translation memo checks shader code they touch again */
    if (g_hw_note_hook && n) g_hw_note_hook(a, n);
#ifdef HAVE_CPU_WROTE_NOTE
    if (kind == 2 && n) cpu_wrote_note(a, n);   /* STATUS 80: file reads are kernel writes the OS write tracking may miss */
#endif
    if (!g_hw_on || !n) return;
    u64 k = __atomic_fetch_add(&g_hw_nev, 1, __ATOMIC_RELAXED);
    HwEv *e = &g_hw_ev[k & (HW_EV - 1)];
    e->lo = a; e->hi = a + n; e->flip = g_plat_frame; e->tn = cur_thread_name(); e->kind = (u32)kind;
}

/* ---- the check (pure: testable on its own) ----
   1 = the signature: old was a vtable pointer (it points at a non-zero slot inside the eboot image) and new = old - 0x10,
       where the two qwords at new (offset-to-top and type info) are 0;
   2 = looser: old was a vtable pointer and new is a slightly smaller pointer (same high bits, less than 0x100 lower) that
       points at a zero qword, so it cannot be a vtable pointer (a valid one points at a function: a destructor setting
       the base class's vtable, the ordinary case seen in the first runs, is not reported);
   0 = anything else (ordinary changes: new objects, data). */
#ifdef _WIN32
static int hw_readable(u64 a, u64 n) { return plat_readable(a, n); }   /* the image may have uncommitted gaps between segments */
static int hw_writable8(u64 a) { return plat_writable8(a); }
#else
static int hw_readable(u64 a, u64 n) { (void)n; return a >= 0x10000; }
static int hw_writable8(u64 a) { return a >= 0x10000; }
#endif
static int hw_classify(u64 old, u64 nw, u64 eb_lo, u64 eb_hi) {
    if (old == nw || old < eb_lo + 0x18 || old > eb_hi - 8 || nw < eb_lo || nw > eb_hi - 0x18) return 0;   /* (no wrap-around: values near 2^64) */
    if ((old & 7) || (nw & 7)) return 0;
    if (!hw_readable(old, 8) || !hw_readable(nw, 0x10)) return 0;
    if (old - nw == 0x10 && *(const u64 *)old != 0 && *(const u64 *)nw == 0 && *(const u64 *)(nw + 8) == 0) return 1;
    if (nw < old && old - nw < 0x100 && (old >> 8) == (nw >> 8) && *(const u64 *)old != 0 && *(const u64 *)nw == 0) return 2;
    return 0;
}
/* 3 = the bad value itself, whatever was there before (STATUS 46: the fifth crash, at flip ~450, was not seen as a
   change from V: the page went straight to V - 0x10). new points at two zero qwords (a vtable's offset-to-top and type
   info) followed by a non-zero qword: a vtable's start, which no object should hold. eb_lo..eb_hi: the eboot image. */
static int hw_is_vtable_start(u64 nw, u64 eb_lo, u64 eb_hi) {
    if (nw < eb_lo || nw > eb_hi - 0x18 || (nw & 7)) return 0;   /* nw + 0x18 would wrap for values near 2^64 (run t46d: 0xfffffffffffffff0 crashed the scanner) */
    if (!hw_readable(nw, 0x18)) return 0;
    const u64 *q = (const u64 *)nw;
    return q[0] == 0 && q[1] == 0 && q[2] >= eb_lo && q[2] < eb_hi;
}

#ifndef HW_TEST
/* ---- the scanner ---- */
typedef struct { u64 lo, hi; u64 *last; int skip; } HwReg;
static HwReg g_hwr[4096]; static int g_nhwr;
static u64 g_hw_scans, g_hw_scan_ticks, g_hw_pages, g_hw_found1, g_hw_found2, g_hw_reported1, g_hw_reported2;
static u64 g_hw_pass_flip[2];   /* flip at the start of the previous and the current pass */
static void hw_line(const char *s) { say(s); }
static u64 g_hw_found3, g_hw_reported3;
static int g_lump_repair; static u64 g_lump_repaired_scan, g_lump_repaired_call;
static u64 g_hw_found4, g_hw_reported4, g_hw_found5, g_hw_reported5;
static void hw_report(u64 page, u64 old, u64 nw, int cls) {
    if (cls == 1 ? g_hw_reported1 >= 40 : cls == 3 ? g_hw_reported3 >= 40 : cls == 4 ? g_hw_reported4 >= 60 : cls == 5 ? g_hw_reported5 >= 40 : g_hw_reported2 >= 10) return;   /* separate limits: looser reports never hide a match */
    if (cls == 1) g_hw_reported1++; else if (cls == 3) g_hw_reported3++; else if (cls == 4) g_hw_reported4++; else if (cls == 5) g_hw_reported5++; else g_hw_reported2++;
    u64 eb = (u64)g_eboot.base;
    char b[400]; Sb t = {b, 0, sizeof b};
    sb_s(&t, cls == 1 ? "\n*** watch-heap: MATCH - the first qword of page " : cls == 3 ? "\n*** watch-heap: VTABLE START - the first qword of page " : cls == 4 ? "\n*** watch-heap: LOW BYTE CLEARED - the first qword of page " : cls == 5 ? "\n*** watch-heap: EBOOT POINTER LOW BYTE CLEARED - the first qword of page " : "\n*** watch-heap: possible - the first qword of page "); sb_x(&t, page);
    sb_s(&t, " changed from "); if (old >= eb && old < eb + g_eboot.size) { sb_s(&t, "eboot+"); sb_x(&t, old - eb); } else sb_x(&t, old);
    sb_s(&t, " to "); if (nw >= eb && nw < eb + g_eboot.size) { sb_s(&t, "eboot+"); sb_x(&t, nw - eb); } else sb_x(&t, nw);
    sb_s(&t, cls == 1 ? " (a vtable pointer reduced by 0x10: the streaming-crash signature)" : cls == 3 ? " (the start of a vtable, where an object's pointer would be +0x10: the crash value)" : cls == 4 ? " (only the lowest byte changed, to 0: the pattern of all reports so far, STATUS 69)" : cls == 5 ? " (a pointer into the game's code / data image whose lowest byte went to 0, nothing else: the damage pattern, any target; t107)" : " (an eboot pointer made slightly smaller)");
    sb_s(&t, "\n    between flip "); sb_d(&t, (i64)g_hw_pass_flip[0]); sb_s(&t, " and flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, " (scan "); sb_d(&t, (i64)g_hw_scans); sb_s(&t, ")\n");
    hw_line(b);
    /* the page now */
    for (int r = 0; r < 4; r++) {
        const u32 *w = (const u32 *)(page + 16 * (u64)r); Sb u = {b, 0, sizeof b}; sb_s(&u, "    "); sb_x(&u, page + 16 * (u64)r); sb_s(&u, ":");
        for (int k = 0; k < 4; k++) { sb_s(&u, " "); sb_x(&u, w[k]); } sb_s(&u, "\n"); hw_line(b);
    }
    /* its mapping and the mappings of it that went away */
    pm_lock(&g_dmap_lock);
    for (int i = 0; i < g_ndmap; i++) { const DMap *d = &g_dmap[i]; if (page < d->va || page >= d->va + d->len) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, "    mapping "); sb_x(&u, d->va); sb_s(&u, "+"); sb_x(&u, d->len); sb_s(&u, " phys "); sb_x(&u, (u64)d->phys);
        sb_s(&u, ", mapped at flip "); sb_d(&u, (i64)d->flip); sb_s(&u, " by "); eb_off(&u, d->caller); sb_s(&u, "\n"); hw_line(b); break; }
    { int shown = 0;
      for (u64 k = g_ndrel; k-- > (g_ndrel > 1024 ? g_ndrel - 1024 : 0) && shown < 6; ) { const DRel *r = &g_drel[k % 1024];
        if (page < r->va || page >= r->va + r->len) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, r->why == 1 ? "    earlier mapping released at flip " : "    earlier mapping replaced at flip "); sb_d(&u, (i64)r->flip);
        sb_s(&u, " by "); eb_off(&u, r->caller); sb_s(&u, " (mapped at flip "); sb_d(&u, (i64)r->map_flip); sb_s(&u, ")\n"); hw_line(b); shown++; } }
    pm_unlock(&g_dmap_lock);
    /* our CPU-side writes into the page, newest first */
    { u64 n = __atomic_load_n(&g_hw_nev, __ATOMIC_ACQUIRE), from = n > HW_EV ? n - HW_EV : 0; int shown = 0;
      for (u64 k = n; k-- > from && shown < 16; ) { const HwEv *e = &g_hw_ev[k & (HW_EV - 1)];
        if (e->hi <= page || e->lo >= page + 4096) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, "    our CPU write at flip "); sb_d(&u, (i64)e->flip); sb_s(&u, ": "); sb_s(&u, e->kind < HWK_N ? g_hwk_name[e->kind] : "?");
        sb_s(&u, " ["); sb_s(&u, e->tn ? e->tn : "?"); sb_s(&u, "] "); sb_x(&u, e->lo); sb_s(&u, "-"); sb_x(&u, e->hi); sb_s(&u, "\n"); hw_line(b); shown++; }
      Sb u = {b, 0, sizeof b}; sb_s(&u, "    our CPU writes into this page in the journal (last "); sb_d(&u, (i64)(n - from)); sb_s(&u, " writes, from flip ");
      sb_d(&u, (i64)(n > from ? g_hw_ev[from & (HW_EV - 1)].flip : 0)); sb_s(&u, "): "); sb_d(&u, shown); sb_s(&u, shown >= 16 ? " or more\n" : "\n"); hw_line(b); }
#ifdef _WIN32
    /* E6's GPU-write log: what the GPU wrote (or may have written) into game memory */
    { u64 seq = g_gw_seq, done = g_gw_done, from = seq > GW_N ? seq - GW_N : 0; int shown = 0;
      for (u64 k = seq; k-- > from && shown < 16; ) { u64 i = k % GW_N; if (g_gw[i].hi <= page || g_gw[i].lo >= page + 4096) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, "    GPU write logged (position "); sb_d(&u, (i64)k); sb_s(&u, k < done ? ", finished" : ", in flight"); sb_s(&u, "): ");
        sb_x(&u, g_gw[i].lo); sb_s(&u, "-"); sb_x(&u, g_gw[i].hi); sb_s(&u, "\n"); hw_line(b); shown++; }
      Sb u = {b, 0, sizeof b}; sb_s(&u, "    GPU writes into this page in E6's log (last "); sb_d(&u, (i64)(seq - from)); sb_s(&u, "): "); sb_d(&u, shown); sb_s(&u, "\n"); hw_line(b); }
    /* t122: Vulkan imports over this page (any time) and any import in this window */
    { u64 n = g_imp_n, from = n > IMP_EV ? n - IMP_EV : 0; int over = 0, inwin = 0;
      for (u64 k = n; k-- > from; ) { const ImpEv *e = &g_imp_ev[k & (IMP_EV - 1)]; int ov = !(e->hi <= page || e->lo >= page + 4096);
          int win = e->f1 >= g_hw_pass_flip[0] || e->f1 == ~0ull;
          if (ov) over++; if (win) inwin++;
          if ((ov || win) && over + inwin <= 12) { Sb u = {b, 0, sizeof b}; sb_s(&u, ov ? "    Vulkan import OVER this page: " : "    Vulkan import elsewhere in this window: ");
              sb_x(&u, e->lo); sb_s(&u, "-"); sb_x(&u, e->hi); sb_s(&u, " flips "); sb_d(&u, (i64)e->f0); sb_s(&u, "-"); if (e->f1 == ~0ull) sb_s(&u, "(running)"); else sb_d(&u, (i64)e->f1);
              sb_s(&u, " ["); sb_s(&u, e->tn ? e->tn : "?"); sb_s(&u, "]\n"); hw_line(b); } }
      Sb u = {b, 0, sizeof b}; sb_s(&u, "    Vulkan imports (t122, last "); sb_d(&u, (i64)(n - from)); sb_s(&u, "): over this page "); sb_d(&u, over); sb_s(&u, ", in this window "); sb_d(&u, inwin); sb_s(&u, "\n"); hw_line(b); }
#endif
}
/* at a crash: the scanner's view of the page holding v and our journalled writes into it (STATUS 46) */
static void hw_crash_page(u64 v, void *f) {
    if (!g_hw_on || !f) return;
    u64 page = v & ~0xFFFull; char b[300];
    for (int i = 0; i < g_nhwr; i++) { const HwReg *R = &g_hwr[i]; if (R->skip || page < R->lo || page >= R->hi) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, "      watch-heap: last value seen at the page start "); sb_x(&u, R->last[(page - R->lo) >> 12]);
        sb_s(&u, " (scan "); sb_d(&u, (i64)g_hw_scans); sb_s(&u, ", flip "); sb_d(&u, (i64)g_plat_frame); sb_s(&u, ")\n"); plat_write(f, b); break; }
    u64 n = __atomic_load_n(&g_hw_nev, __ATOMIC_ACQUIRE), from = n > HW_EV ? n - HW_EV : 0; int shown = 0;
    for (u64 k = n; k-- > from && shown < 12; ) { const HwEv *e = &g_hw_ev[k & (HW_EV - 1)];
        if (e->hi <= page || e->lo >= page + 4096) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, "      our CPU write at flip "); sb_d(&u, (i64)e->flip); sb_s(&u, ": "); sb_s(&u, e->kind < HWK_N ? g_hwk_name[e->kind] : "?");
        sb_s(&u, " ["); sb_s(&u, e->tn ? e->tn : "?"); sb_s(&u, "] "); sb_x(&u, e->lo); sb_s(&u, "-"); sb_x(&u, e->hi); sb_s(&u, "\n"); plat_write(f, b); shown++; }
    Sb u = {b, 0, sizeof b}; sb_s(&u, "      our CPU writes into this page in the journal: "); sb_d(&u, shown); sb_s(&u, "\n"); plat_write(f, b);
#ifdef _WIN32
    { u64 seq = g_gw_seq, done = g_gw_done, fr = seq > GW_N ? seq - GW_N : 0; int sh = 0;
      for (u64 k = seq; k-- > fr && sh < 8; ) { u64 i = k % GW_N; if (g_gw[i].hi <= page || g_gw[i].lo >= page + 4096) continue;
        Sb w = {b, 0, sizeof b}; sb_s(&w, "      GPU write logged (position "); sb_d(&w, (i64)k); sb_s(&w, k < done ? ", finished" : ", in flight"); sb_s(&w, "): ");
        sb_x(&w, g_gw[i].lo); sb_s(&w, "-"); sb_x(&w, g_gw[i].hi); sb_s(&w, "\n"); plat_write(f, b); sh++; }
      Sb w = {b, 0, sizeof b}; sb_s(&w, "      GPU writes into this page in E6's log (last "); sb_d(&w, (i64)(seq - fr)); sb_s(&w, "): "); sb_d(&w, sh); sb_s(&w, "\n"); plat_write(f, b); }
#endif
}
/* --lump-repair, at the crash itself (STATUS 47): a call through an object whose vtable pointer is a vtable's start (the
   streaming crash: the call reads a header slot, 0, and jumps to 0). Recognised only when all of this holds:
   rip = 0; the return address on the stack is in the eboot right after `call [reg]` / `call [reg+disp8]` (FF /2, with or
   without REX.B); that register holds V with V, V+8 = 0 and V+0x10 an eboot pointer (a vtable's start); the slot it read
   is 0. Then every register-held object whose first qword is V gets V + 0x10, the register gets V + 0x10, the return
   address is popped and the call runs again. Anything else is left to the crash report. */
static int lump_repair_call(u8 *ctx) {
    u64 *g = (u64 *)(ctx + 0x78), *rip = (u64 *)(ctx + 0xF8);   /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    u64 eb_lo = (u64)g_eboot.base, eb_hi = eb_lo + g_eboot.size, sp = g[4];
    if (*rip != 0 || !hw_readable(sp, 8)) return 0;
    u64 ret = *(const u64 *)sp;
    if (ret < eb_lo + 4 || ret >= eb_hi || !hw_readable(ret - 4, 4)) return 0;
    const u8 *c = (const u8 *)ret; int reg = -1, len = 0; i64 disp = 0;
    if (c[-2] == 0xFF && (c[-1] & 0xF8) == 0x10 && (c[-1] & 7) != 4 && (c[-1] & 7) != 5) { reg = c[-1] & 7; len = 2; if (c[-3] == 0x41) { reg += 8; len = 3; } }
    else if (c[-3] == 0xFF && (c[-2] & 0xF8) == 0x50 && (c[-2] & 7) != 4) { reg = c[-2] & 7; disp = (signed char)c[-1]; len = 3; if (c[-4] == 0x41) { reg += 8; len = 4; } }
    if (reg < 0 || reg == 4) return 0;
    u64 V = g[reg];
    if (!hw_is_vtable_start(V, eb_lo, eb_hi) || disp < 0 || disp > 0x400 || !hw_readable(V + (u64)disp, 8) || *(const u64 *)(V + (u64)disp) != 0) return 0;
    int fixed_obj = 0;
    for (int r = 0; r < 16; r++) { u64 o = g[r]; if (r == 4 || (o & 7) || o < 0x10000 || o == V || !hw_readable(o, 8) || *(const u64 *)o != V || !hw_writable8(o)) continue;
        u64 want = V; if (__atomic_compare_exchange_n((u64 *)o, &want, V + 0x10, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) fixed_obj++; }
    g[reg] = V + 0x10; g[4] = sp + 8; *rip = ret - (u64)len;
    u64 n = ++g_lump_repaired_call;
    if (n <= 40) { char b[260]; Sb t = {b, 0, sizeof b};
        sb_s(&t, "\n*** lump-repair: call through a vtable's start (eboot+"); sb_x(&t, V - eb_lo); sb_s(&t, ") at eboot+"); sb_x(&t, ret - (u64)len - eb_lo);
        sb_s(&t, " on thread "); sb_s(&t, cur_thread_name()); sb_s(&t, " (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, "): pointer set to +0x10 in "); sb_d(&t, fixed_obj);
        sb_s(&t, " object(s), call repeated. This is the streaming crash, contained (STATUS 47)\n"); say(b); }
    return n < 100000;   /* a loop of repairs would mean something else is wrong: then crash and report */
}
/* only memory the Lump Loader mapped (256 KB direct-memory chunks, caller eboot+0xace6ab) is repaired by the scanner:
   run t47a showed page starts elsewhere that legitimately point at a structure beginning with two zero qwords (resource
   records such as "CloseUpShotResource_Yaw" in the main heap); the scanner changed two of them (STATUS 47b) */
static int hw_in_lump_chunk(u64 p) {
    int r = 0; u64 want = (u64)g_eboot.base + 0xace6ab;
    pm_lock(&g_dmap_lock);
    for (int i = 0; i < g_ndmap; i++) { const DMap *d = &g_dmap[i]; if (p >= d->va && p < d->va + d->len) { r = d->caller == want; break; } }
    pm_unlock(&g_dmap_lock);
    return r;
}
/* ---- hardware write watchpoints on reported page starts (STATUS 69) ----
   Every report so far had the same shape: only the lowest byte of the page's first qword changed, to 0. The same page
   was sometimes hit again (0x214e7f000 three times in run t47a). So each reported page start gets a 1-byte write
   watchpoint in the CPU's debug registers (4, oldest replaced), in every thread. A CPU write there traps: the trap
   records the thread, the instruction after the write, code bytes before it and return addresses on the stack. A
   watched page that changes again without any trap was written by something that is not a CPU thread (GPU, device). */
/* --watch-writes only (off by default): a watchpoint trap is an exception, and Windows delivers exceptions on the
   thread's own stack right below rsp - over the 128-byte red zone the game's System V code keeps locals in. Test 69
   (1,577 traps) crashed that way: a local at rsp-0x28 read back as 0 at eboot+0x1197e1a. */
static int g_hw_dr_on;
static u64 g_hw_watch[4], g_hw_wval[4]; static volatile u64 g_hw_wtraps[4]; static u64 g_hw_wseen[4]; static int g_hw_nwatch, g_hw_rearm;
static u64 g_hw_arm_scan; static int g_hw_armed_threads;
typedef struct { u64 addr, before, after, rip, rsp, flip, stk[8], gpr[16]; u8 code[16]; u32 tid; const char *tn; int slot; } HwTrap;
enum { HW_TRAPS = 2048 };   /* t126: room for the whole 1000-trap budget */
static HwTrap g_hw_trap[HW_TRAPS]; static volatile u64 g_hw_ntrap, g_hw_ntrap_ord, g_hw_nrec; static u64 g_hw_ptrap, g_hw_suspect;   /* t129: g_hw_ntrap counts damage-shaped traps (the budget), g_hw_ntrap_ord the ordinary ones, g_hw_nrec every record */
static int hw_is_lowbyte_clear(u64 old, u64 nw) { return old != nw && (old >> 8) == (nw >> 8) && (old & 0xFF) && !(nw & 0xFF); }
/* t129: the damage pattern proper: the old value was a pointer (8-aligned, into the eboot image or game / host memory) and only its
   low byte went to 0; counters and characters at a page start (4 -> 0, 'a' -> 0, 0x...00000001 -> 0) are ordinary writes */
static int hw_is_pointer_like(u64 v) { return !(v & 7) && v >= 0x10000 && (v >> 48) == 0; }
static int hw_is_damage_write(u64 old, u64 nw) { return hw_is_lowbyte_clear(old, nw) && hw_is_pointer_like(old); }
static volatile u32 g_hw_wordinary[4];   /* ordinary (not damage-shaped) writes seen per watch slot: a hot page gives its slot up */
static int g_hw_wcls[4];
/* a slot for page start p (report class cls): vtable damage (classes 1-3) is never pushed out by a cleared low byte
   (class 4, which may also be ordinary data); within a kind the oldest goes */
static void hw_watch_add(u64 p, int cls) {
    for (int i = 0; i < g_hw_nwatch; i++) if (g_hw_watch[i] == p) return;
    int k = -1;
    if (g_hw_nwatch < 4) k = g_hw_nwatch++;
    else {
        for (int i = 0; i < 4 && k < 0; i++) if (g_hw_wcls[i] == 4) k = i;   /* oldest data slot first */
        if (k < 0 && cls != 4) k = 0;                                       /* all vtable damage: the oldest */
        if (k < 0) return;                                                  /* a data case does not replace vtable damage */
        for (int i = k; i < 3; i++) { g_hw_watch[i] = g_hw_watch[i + 1]; g_hw_wval[i] = g_hw_wval[i + 1]; g_hw_wtraps[i] = g_hw_wtraps[i + 1]; g_hw_wseen[i] = g_hw_wseen[i + 1]; g_hw_wcls[i] = g_hw_wcls[i + 1]; g_hw_wordinary[i] = g_hw_wordinary[i + 1]; }
        k = 3;
    }
    g_hw_watch[k] = p; g_hw_wval[k] = *(volatile const u64 *)p; g_hw_wtraps[k] = 0; g_hw_wseen[k] = 0; g_hw_wcls[k] = cls; g_hw_wordinary[k] = 0;
    g_hw_rearm = 1;
}
/* runs on the writing thread, inside the exception handler: record only (printed by the scanner) */
static void hw_dr_hook(u8 *ctx, u64 dr6) {
    for (int i = 0; i < 4; i++) {
        if (!((dr6 >> i) & 1)) continue;
        u64 a = *(u64 *)(ctx + 0x48 + 8 * i); int slot = -1;
        for (int j = 0; j < g_hw_nwatch; j++) if (g_hw_watch[j] == a) slot = j;
        u64 nw = *(volatile const u64 *)(a & ~7ull), old = slot >= 0 ? g_hw_wval[slot] : 0;
        if (slot >= 0) { g_hw_wval[slot] = nw; __atomic_add_fetch(&g_hw_wtraps[slot], 1, __ATOMIC_RELAXED); }
        int dmg = hw_is_damage_write(old, nw);
        if (!dmg && slot >= 0) { u32 o = __atomic_add_fetch(&g_hw_wordinary[slot], 1, __ATOMIC_RELAXED); if (o > 24) continue; }   /* t129: a few ordinary writes per page are recorded, the rest only counted */
        u64 k = __atomic_fetch_add(dmg ? &g_hw_ntrap : &g_hw_ntrap_ord, 1, __ATOMIC_RELAXED);
        k = __atomic_fetch_add(&g_hw_nrec, 1, __ATOMIC_RELAXED);
        HwTrap *T = &g_hw_trap[k % HW_TRAPS];   /* a ring: old records are overwritten once printed (the scanner prints every pass) */
        T->addr = a; T->before = old; T->after = nw; T->rip = *(u64 *)(ctx + 0xF8); T->rsp = *(u64 *)(ctx + 0x98); T->flip = g_plat_frame;
#ifdef _WIN32
        T->tid = GetCurrentThreadId();
#else
        T->tid = 0;
#endif
        T->tn = cur_thread_name(); T->slot = slot;
        for (int r = 0; r < 16; r++) T->gpr[r] = *(u64 *)(ctx + 0x78 + 8 * r);   /* t127: rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
        for (int b = 0; b < 16; b++) T->code[b] = hw_readable(T->rip - 16, 16) ? ((const u8 *)(T->rip - 16))[b] : 0;
        int ns = 0; u64 eb = (u64)g_eboot.base;
        for (int q = 0; q < 96 && ns < 8 && hw_readable(T->rsp + 8ull * q, 8); q++) { u64 v = ((const u64 *)T->rsp)[q];
            if ((v >= eb && v < eb + g_eboot.size) || (v >> 40) == 0x7f) T->stk[ns++] = v; }
        for (; ns < 8; ns++) T->stk[ns] = 0;
        __atomic_store_n(&T->slot, slot, __ATOMIC_RELEASE);
    }
}
static void hw_addr_out(Sb *t, u64 v) {   /* eboot+off, module+off, or the address */
    u64 eb = (u64)g_eboot.base;
    if (v >= eb && v < eb + g_eboot.size) { sb_s(t, "eboot+"); sb_x(t, v - eb); return; }
#ifdef _WIN32
    char mn[260]; u64 mb = plat_module_of(v, mn, sizeof mn);
    if (mb) { const char *q = mn; for (const char *c = mn; *c; c++) if (*c == '\\' || *c == '/') q = c + 1; sb_s(t, q); sb_s(t, "+"); sb_x(t, v - mb); return; }
#endif
    sb_x(t, v);
}
static void (*g_hw_explain_fn)(u64 lo, u64 hi, void (*out)(const char *));   /* t128: the arena allocations around a trapped write (e3_loader.c) */
static void hw_print_traps(void) {
    u64 n = __atomic_load_n(&g_hw_nrec, __ATOMIC_ACQUIRE);
    if (n > g_hw_ptrap + HW_TRAPS) g_hw_ptrap = n - HW_TRAPS;   /* (records overwritten before they were printed) */
    for (; g_hw_ptrap < n; g_hw_ptrap++) {
        HwTrap *T = &g_hw_trap[g_hw_ptrap % HW_TRAPS]; char b[1200]; Sb t = {b, 0, sizeof b};
        int sus = hw_is_damage_write(T->before, T->after); if (sus) g_hw_suspect++;
        if (!sus && g_hw_ptrap >= 200) continue;   /* ordinary writes: the first 200 records only */
        sb_s(&t, sus ? "\n*** watch-heap: CAUGHT - a CPU write cleared the low byte of watched page start " : "  watch-heap: CPU write to watched page start "); sb_x(&t, T->addr);
        sb_s(&t, " at flip "); sb_d(&t, (i64)T->flip); sb_s(&t, ": "); sb_x(&t, T->before); sb_s(&t, " -> "); sb_x(&t, T->after);
        sb_s(&t, "\n    thread "); sb_s(&t, T->tn ? T->tn : "?"); sb_s(&t, " (id "); sb_d(&t, T->tid); sb_s(&t, "), next instruction "); hw_addr_out(&t, T->rip);
        sb_s(&t, "\n    code before it:"); for (int i = 0; i < 16; i++) { sb_s(&t, " "); sb_x(&t, T->code[i]); }
        sb_s(&t, "\n    return addresses on the stack:"); for (int i = 0; i < 8 && T->stk[i]; i++) { sb_s(&t, " "); hw_addr_out(&t, T->stk[i]); }
        if (sus) { static const char *const nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
            sb_s(&t, "\n    registers:"); for (int r = 0; r < 16; r++) { sb_s(&t, " "); sb_s(&t, nm[r]); sb_s(&t, "="); sb_x(&t, T->gpr[r]); } }
        sb_s(&t, "\n"); say(b);
        if (sus && g_hw_explain_fn) g_hw_explain_fn(T->addr - 0x400, T->addr + 0x40, say);
    }
}
static void hw_scan_region(HwReg *R) {
    u64 n = (R->hi - R->lo) >> 12;
    for (u64 k = 0; k < n; k++) {
        u64 p = R->lo + (k << 12), v = *(volatile const u64 *)p, o = R->last[k];
        if (v == o) continue;
        R->last[k] = v;
        int c = hw_classify(o, v, (u64)g_eboot.base, (u64)g_eboot.base + g_eboot.size);
        if (!c && hw_is_vtable_start(v, (u64)g_eboot.base, (u64)g_eboot.base + g_eboot.size) && !((o & 7) && (o & ~7ull) == v)) c = 3;   /* t126: not a counter in the low bits counting down to a vtable-start-like value (t125: eboot+0x1ff0002 -> 0x1ff0000, ordinary) */
        if (!c && g_hw_scans && hw_is_lowbyte_clear(o, v)) c = 4;
        if (c == 4 && o >= (u64)g_eboot.base && o < (u64)g_eboot.base + g_eboot.size && !(o & 7)) c = 5;   /* t122: a pointer (8-aligned); a counter in the low bits (t120 B: eboot+0x1fe0002 -> 0x1fe0000, ordinary code) is not */   /* t107: the same change on an eboot pointer (a player's 0.1.3 crash report: a vtable pointer 0x...10 -> 0x...00 that the old classes missed) */
        if (c == 1) g_hw_found1++; else if (c == 2) g_hw_found2++; else if (c == 3) g_hw_found3++; else if (c == 4) g_hw_found4++; else if (c == 5) g_hw_found5++;
        for (int w = 0; w < g_hw_nwatch; w++) if (g_hw_watch[w] == p) {   /* a watched page start changed: was a CPU write trapped? */
            u64 tr = __atomic_load_n(&g_hw_wtraps[w], __ATOMIC_ACQUIRE);
            if (tr == g_hw_wseen[w] && g_hw_arm_scan && g_hw_scans > g_hw_arm_scan) { char b[300]; Sb t = {b, 0, sizeof b};
                sb_s(&t, "\n*** watch-heap: NOT A CPU WRITE - watched page start "); sb_x(&t, p); sb_s(&t, " changed from "); sb_x(&t, o); sb_s(&t, " to "); sb_x(&t, v);
                sb_s(&t, " (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, ") and no thread's write trapped: the GPU or a device wrote it (or a thread started after the last re-arm; ");
                sb_d(&t, g_hw_armed_threads); sb_s(&t, " threads armed)\n"); say(b); }
            g_hw_wseen[w] = tr; }
        if (c == 4) c = 0;   /* counted only: test 69 showed 691 of these in 1,600 flips of ordinary heap activity */
        if (c) { hw_report(p, o, v, c); if (g_hw_dr_on && c != 2) hw_watch_add(p, c); }   /* t120: watch true damage only (not the looser class) */
        if ((c == 1 || c == 3) && g_lump_repair && hw_in_lump_chunk(p) && hw_writable8(p)) {   /* --lump-repair: no object holds a vtable's start; the pointer is +0x10 */
            if (__atomic_compare_exchange_n((u64 *)p, &v, v + 0x10, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { R->last[k] = v + 0x10; g_lump_repaired_scan++;
                char b[160]; Sb t = {b, 0, sizeof b}; sb_s(&t, "    lump-repair: page start "); sb_x(&t, p); sb_s(&t, " set from eboot+"); sb_x(&t, v - (u64)g_eboot.base);
                sb_s(&t, " to +0x10 by the scanner (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, ")\n"); if (g_lump_repaired_scan <= 40) say(b); } }
    }
}
static void hw_thread(void *arg) {
    (void)arg;
    u64 eb_lo = (u64)g_eboot.base, eb_hi = eb_lo + g_eboot.size;
    for (;;) {
        u64 t0 = plat_ticks();
        g_hw_pass_flip[0] = g_hw_pass_flip[1]; g_hw_pass_flip[1] = g_plat_frame;
        int nm = __atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE);
        for (int i = g_nhwr; i < nm && g_nhwr < 4096; i++) {   /* regions registered since the last pass: start watching them */
            HwReg *R = &g_hwr[g_nhwr]; R->lo = (g_mem[i].lo + 0xFFF) & ~0xFFFull; R->hi = g_mem[i].hi & ~0xFFFull;
            R->skip = R->hi <= R->lo || (R->lo < eb_hi && R->hi > eb_lo);   /* not the eboot image itself */
            if (g_sent_lo && R->lo < g_sent_lo + ((u64)SENT_PAGES << 12) && R->hi > g_sent_lo) R->skip = 1;   /* t120: the sentinel checks its own pages */
            if (!R->skip) { R->last = (u64 *)plat_alloc_rw(((R->hi - R->lo) >> 12) * 8 + 8); if (!R->last) R->skip = 1; else mem_name(R->last, "heap watch: page sums"); }
            if (!R->skip) { u64 n = (R->hi - R->lo) >> 12;
                /* regions registered while the game runs (a new 64 MB lump arena, say) start from 0, as fresh memory does:
                   the first pass then classifies what was written before it saw the region (the two crashes of STATUS 46
                   were in arenas created a few flips earlier, and their bad values were taken in as the baseline) */
                for (u64 k = 0; k < n; k++) R->last[k] = g_hw_scans ? 0 : *(volatile const u64 *)(R->lo + (k << 12));
                g_hw_pages += n; }
            g_nhwr++;
        }
        for (int i = 0; i < g_nhwr; i++) if (!g_hwr[i].skip) hw_scan_region(&g_hwr[i]);
        g_hw_scans++; g_hw_scan_ticks += plat_ticks() - t0;
        hw_print_traps();
        if (g_hw_dr_on && g_hw_nwatch) {   /* t129: a page with many ordinary writes is an ordinary hot page now: its slot goes to the next damage report */
            for (int w = 0; w < g_hw_nwatch; w++) if (__atomic_load_n(&g_hw_wordinary[w], __ATOMIC_RELAXED) > 64) {
                char b[160]; Sb t = {b, 0, sizeof b}; sb_s(&t, "  watch-heap: page start "); sb_x(&t, g_hw_watch[w]); sb_s(&t, " is written ordinarily now (64+ writes) - no longer watched (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, ")\n"); say(b);
                for (int i = w; i < 3; i++) { g_hw_watch[i] = g_hw_watch[i + 1]; g_hw_wval[i] = g_hw_wval[i + 1]; g_hw_wtraps[i] = g_hw_wtraps[i + 1]; g_hw_wseen[i] = g_hw_wseen[i + 1]; g_hw_wcls[i] = g_hw_wcls[i + 1]; g_hw_wordinary[i] = g_hw_wordinary[i + 1]; }
                g_hw_nwatch--; g_hw_wordinary[g_hw_nwatch] = 0; g_hw_rearm = 1; w--; } }
        if (g_hw_dr_on && __atomic_load_n(&g_hw_ntrap, __ATOMIC_ACQUIRE) >= 1000) {   /* t120: a bounded number of traps (each one can clobber the red zone of the thread it stops) */
            g_hw_dr_on = 0; g_hw_nwatch = 0; g_hw_rearm = 0; int nt = plat_dr_arm_all(g_hw_watch, 0);
            char b[160]; Sb t = {b, 0, sizeof b}; sb_s(&t, "  watch-heap: 1000 damage-shaped watchpoint traps reached - all watchpoints cleared in "); sb_d(&t, nt); sb_s(&t, " threads (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, ")\n"); say(b);
        }
        if (g_hw_nwatch && (g_hw_rearm || g_hw_scans % 40 == 0)) {   /* new watch, or every ~40 passes for threads started since */
            int first = g_hw_rearm; g_hw_rearm = 0; g_dr_hook = hw_dr_hook;
            int nt = plat_dr_arm_all(g_hw_watch, g_hw_nwatch); g_hw_armed_threads = nt; if (first) g_hw_arm_scan = g_hw_scans;
            if (first) { char b[300]; Sb t = {b, 0, sizeof b}; sb_s(&t, "  watch-heap: hardware write watch on");
                for (int w = 0; w < g_hw_nwatch; w++) { sb_s(&t, " "); sb_x(&t, g_hw_watch[w]); }
                sb_s(&t, " in "); sb_d(&t, nt); sb_s(&t, " threads (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, ")\n"); say(b); }
        }
        if (g_hw_scans == 50 || g_hw_scans % 20000 == 0) {   /* the detector's time resolution, for the log */
            char b[200]; Sb t = {b, 0, sizeof b}; u64 f = plat_ticks_per_sec() / 1000;
            sb_s(&t, "  watch-heap: "); sb_d(&t, (i64)g_hw_pages); sb_s(&t, " pages in "); sb_d(&t, g_nhwr); sb_s(&t, " regions, a pass takes ");
            sb_d(&t, (i64)(f ? (plat_ticks() - t0) / f : 0)); sb_s(&t, " ms (flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, ")\n"); say(b);
        }
        plat_sleep_ms(5);
    }
}

/* ---- t120: the sentinel - is the page-start damage done by something other than game code? ----
   32 MB of memory that only our two test threads use, allocated and registered exactly like game memory (write
   tracking, imported into Vulkan by the renderer, scanned by this watcher). A writer thread keeps rewriting the first
   qword of every page with real vtable pointers (low byte 0x10, as the damaged objects had) and checks, before each
   rewrite, that the page still holds what it wrote last. A second thread meanwhile runs the operating-system and
   driver work our layer runs over game memory: write-tracking queries with reset (the renderer's memory checks),
   VirtualQuery, working-set queries (the memory report) and Vulkan imports of pieces of it (the t107 page-probe
   theory). No game code ever touches the sentinel, so any change the writer did not make is ours, Windows' or the
   driver's. --sentinel; off by default. */
static int g_sent_on; static u32 g_sent_mask = 0x3F;   /* t124: --sentinel-ops MASK (bit per phase; 1 = nothing at all, only the GPU using it) */
#if defined(_WIN32) && !defined(HW_TEST)
/* t122: the operations run in phases of 30 s, one kind at a time (t120 found damage but could not say during which
   operation: it was not one started after the page was written, i.e. a long one already running). A change is
   counted for a phase only when the page was written and found changed in the same phase. Phase 0 runs nothing (the
   renderer keeps the sentinel imported, as it keeps all game memory). */
enum { SENT_PH = 6 };
static const char *const g_sent_phname[SENT_PH] = {"nothing", "write-tracking query with reset", "VirtualQuery", "working-set query",
                                                   "Vulkan import + free of 4 MB pieces", "Vulkan host-pointer query only"};
static u64 *g_sent_exp; static u8 *g_sent_wph, *g_sent_wst; static volatile int g_sent_step; static volatile u64 g_sent_bad_step[6]; static volatile int g_sent_ph; static u64 g_sent_t0;
static volatile u64 g_sent_passes, g_sent_writes, g_sent_bad, g_sent_bad_lb, g_sent_bad_ph[SENT_PH], g_sent_bad_edge, g_sent_opn[SENT_PH], g_sent_phus[SENT_PH], g_sent_hb, g_sent_hbcall, g_sent_imp_ok, g_sent_imp_fail;
typedef struct { u64 page, want, got, flip, pass; int ph, wph; i64 sweep; } SentBad;
static SentBad g_sent_badl[48]; static volatile u32 g_sent_hist[128];   /* t124: changes per 300 flips */
static void sent_writer(void *arg) {
    (void)arg;
    /* per page a cycle of three values: 0 (fresh memory), then two real vtable pointers of damaged objects (STATUS 40c,
       69) whose low bytes differ (0x10, 0x30). A late write-back of the first byte turns the value into one ending in
       the previous value's low byte: different from what was written. */
    u64 eb = (u64)g_eboot.base, val[3] = {0, eb + 0x2007d10, eb + 0x1f90830};
    for (u64 k = 0; k < SENT_PAGES; k++) { *(volatile u64 *)(g_sent_lo + (k << 12)) = 0; g_sent_exp[k] = 0; g_sent_wph[k] = 0; }
    for (u64 pass = 1;; pass++) {
        for (u64 k = 0; k < SENT_PAGES; k++) {
            volatile u64 *q = (volatile u64 *)(g_sent_lo + (k << 12));
            u64 v = *q; int ph = g_sent_ph;
            if (v != g_sent_exp[k]) {
                u64 n = __atomic_fetch_add(&g_sent_bad, 1, __ATOMIC_RELAXED);
                { u64 hb = g_plat_frame / 300; if (hb > 127) hb = 127; __atomic_add_fetch(&g_sent_hist[hb], 1, __ATOMIC_RELAXED); }
                if (hw_is_lowbyte_clear(g_sent_exp[k], v)) __atomic_add_fetch(&g_sent_bad_lb, 1, __ATOMIC_RELAXED);
                if (ph == g_sent_wph[k]) __atomic_add_fetch(&g_sent_bad_ph[ph], 1, __ATOMIC_RELAXED); else __atomic_add_fetch(&g_sent_bad_edge, 1, __ATOMIC_RELAXED);
                { int st = g_sent_step; __atomic_add_fetch(&g_sent_bad_step[st == g_sent_wst[k] ? st : 5], 1, __ATOMIC_RELAXED); }
                if (n < 48) { SentBad *b = &g_sent_badl[n]; b->page = (u64)q; b->want = g_sent_exp[k]; b->got = v; b->flip = g_plat_frame; b->pass = pass; b->ph = ph; b->wph = g_sent_wph[k]; b->sweep = (i64)(g_sent_sweep_pos % SENT_PAGES) - (i64)k; }
            }
            u64 w = val[(pass + k) % 3];
            g_sent_wph[k] = (u8)ph; g_sent_wst[k] = (u8)g_sent_step; *q = w; g_sent_exp[k] = w;
            ((volatile u64 *)q)[8 + (pass & 7)] = pass;   /* some ordinary object data further into the page */
        }
        g_sent_passes = pass; g_sent_writes += SENT_PAGES;
        plat_sleep_ms(1);
    }
}
static void sent_ops(void *arg) {
    (void)arg;
    static void *dirty[SENT_PAGES];
    u64 tps = plat_ticks_per_sec(), last = plat_ticks(); int lastph = 0;
    for (u64 c = 0;; c++) {
        u64 now = plat_ticks();
        int en[SENT_PH], nen = 0; for (int q = 0; q < SENT_PH; q++) if ((g_sent_mask >> q) & 1) en[nen++] = q; if (!nen) en[nen++] = 0;
        int ph = en[((now - g_sent_t0) / (tps * 30)) % (u64)nen];
        g_sent_phus[lastph] += (now - last) * 1000000 / tps; last = now; lastph = ph;
        g_sent_ph = ph; g_sent_hb = now; g_sent_hbcall = ph;
        u64 piece = g_sent_lo + (c % 8) * (4ull << 20);
        if (ph == 1) (void)plat_dirty_pages((void *)g_sent_lo, (u64)SENT_PAGES << 12, dirty, SENT_PAGES);
        else if (ph == 2) { for (u64 k = c % 64; k < SENT_PAGES; k += 64) (void)plat_readable(g_sent_lo + (k << 12), 8); }
        else if (ph == 3) (void)plat_resident(g_sent_lo, g_sent_lo + ((u64)SENT_PAGES << 12));
        else if (ph == 4 && g_vk_on && V.dev) {   /* t125: the import and the free apart, 10 ms between them; the writer tells which one a change fell in */
            VkBuffer b; VkDeviceMemory m; u64 d; const char *why = 0;
            g_sent_step = 1; int ok = vk_import_one(piece, piece + (4ull << 20), &b, &m, &d, &why); g_sent_step = 3;
            if (ok) { g_sent_imp_ok++; plat_sleep_ms(10); g_sent_step = 2; vkDestroyBuffer(V.dev, b, 0); vkFreeMemory(V.dev, m, 0); g_sent_step = 4; } else g_sent_imp_fail++;
            plat_sleep_ms(10); g_sent_step = 0;
        }
        else if (ph == 5 && g_vk_on && V.dev) {
            VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
            (void)vkGetMemoryHostPointerPropertiesEXT(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)piece, &hpp);
        }
        g_sent_hbcall = 100 + ph;   /* the call returned */
        if (ph) g_sent_opn[ph]++;
        plat_sleep_ms(ph == 4 ? 20 : 2);
    }
}
static void sent_start(void) {
    if (!g_sent_on) return;
    u64 len = (u64)SENT_PAGES << 12;
    void *p = plat_alloc_at(0, len);
    g_sent_exp = (u64 *)plat_alloc_rw(SENT_PAGES * 8); g_sent_wph = (u8 *)plat_alloc_rw(SENT_PAGES); g_sent_wst = (u8 *)plat_alloc_rw(SENT_PAGES);
    if (!p || !g_sent_exp || !g_sent_wph || !g_sent_wst) { say("  sentinel: no memory - off\n"); g_sent_on = 0; return; }
    mem_name(p, "sentinel (t120)"); mem_name(g_sent_exp, "sentinel: expected values");
    g_sent_lo = (u64)p; g_sent_t0 = plat_ticks();
    mem_register(p, len);   /* like game memory: imported into Vulkan at the next submit (the scanner skips it) */
    pt_create(sent_writer, 0, 0x40000);
    pt_create(sent_ops, 0, 0x40000);
    char b[260]; Sb t = {b, 0, sizeof b}; sb_s(&t, "  sentinel (t123): "); sb_x(&t, g_sent_lo); sb_s(&t, " + 32 MB, rewritten page by page by a test thread; another runs one kind of Windows / driver operation over it at a time, 30 s each; the GPU writes into each 4 MB piece every frame\n"); say(b);
}
static void sent_summary(void) {
    if (!g_sent_on) return;
    char b[3000]; Sb t = {b, 0, sizeof b};
    sb_s(&t, "sentinel (t125): passes "); sb_d(&t, (i64)g_sent_passes); sb_s(&t, ", page-start writes "); sb_d(&t, (i64)g_sent_writes);
    sb_s(&t, "; changes the writer did not make: "); sb_d(&t, (i64)g_sent_bad); sb_s(&t, " (low byte cleared only: "); sb_d(&t, (i64)g_sent_bad_lb); sb_s(&t, "; at a phase change, not counted below: "); sb_d(&t, (i64)g_sent_bad_edge); sb_s(&t, ")\n");
    for (int o = 0; o < SENT_PH; o++) { sb_s(&t, "    phase \""); sb_s(&t, g_sent_phname[o]); sb_s(&t, "\": "); sb_d(&t, (i64)g_sent_bad_ph[o]); sb_s(&t, " changes in "); sb_d(&t, (i64)(g_sent_phus[o] / 1000000)); sb_s(&t, " s");
        if (o) { sb_s(&t, " ("); sb_d(&t, (i64)g_sent_opn[o]); sb_s(&t, " calls)"); } sb_s(&t, "\n"); }
    { u64 tps = plat_ticks_per_sec(), now = plat_ticks(); sb_s(&t, "    operations thread last seen "); sb_d(&t, (i64)((now - g_sent_hb) / tps)); sb_s(&t, " s ago (");
      sb_s(&t, g_sent_hbcall >= 100 ? "after " : "inside "); sb_s(&t, g_sent_phname[g_sent_hbcall % 100]); sb_s(&t, "); run "); sb_d(&t, (i64)((now - g_sent_t0) / tps)); sb_s(&t, " s; mask "); sb_x(&t, g_sent_mask); sb_s(&t, "\n"); }
    sb_s(&t, "    t125 import phase, changes written and found during: the import call "); sb_d(&t, (i64)g_sent_bad_step[1]); sb_s(&t, ", holding the import "); sb_d(&t, (i64)g_sent_bad_step[3]);
    sb_s(&t, ", the free call "); sb_d(&t, (i64)g_sent_bad_step[2]); sb_s(&t, ", after the free "); sb_d(&t, (i64)g_sent_bad_step[4]); sb_s(&t, ", nothing "); sb_d(&t, (i64)g_sent_bad_step[0]); sb_s(&t, ", across steps "); sb_d(&t, (i64)g_sent_bad_step[5]); sb_s(&t, "\n");
    sb_s(&t, "    changes by flip (per 300 flips, from flip):"); for (int h = 0; h < 128; h++) if (g_sent_hist[h]) { sb_s(&t, " "); sb_d(&t, (i64)h * 300); sb_s(&t, ":"); sb_d(&t, (i64)g_sent_hist[h]); } sb_s(&t, "\n");
    sb_s(&t, "    imports ok "); sb_d(&t, (i64)g_sent_imp_ok); sb_s(&t, ", failed "); sb_d(&t, (i64)g_sent_imp_fail); sb_s(&t, "\n"); say(b);
    u64 n = g_sent_bad < 48 ? g_sent_bad : 48;
    for (u64 i = 0; i < n; i++) { const SentBad *x = &g_sent_badl[i]; Sb u = {b, 0, sizeof b};
        sb_s(&u, "    sentinel page "); sb_x(&u, x->page); sb_s(&u, ": wrote "); sb_x(&u, x->want); sb_s(&u, ", found "); sb_x(&u, x->got);
        sb_s(&u, " (flip "); sb_d(&u, (i64)x->flip); sb_s(&u, ", pass "); sb_d(&u, (i64)x->pass); sb_s(&u, ", phase \""); sb_s(&u, g_sent_phname[x->ph]); sb_s(&u, "\"");
        if (x->wph != x->ph) { sb_s(&u, ", written in phase \""); sb_s(&u, g_sent_phname[x->wph]); sb_s(&u, "\""); }
        if (g_sent_sweep) { sb_s(&u, ", GPU sweep position minus this page "); sb_d(&u, x->sweep); sb_s(&u, " (sweep "); sb_d(&u, (i64)(g_sent_sweep_pos / SENT_PAGES)); sb_s(&u, " rounds at exit)"); }
        sb_s(&u, ")\n"); say(b); }
}
#else
static void sent_start(void) {}
static void sent_summary(void) {}
#endif

static void hw_start(void) {
    sent_start();
    if (!g_hw_on) return;
    pt_create(hw_thread, 0, 0x40000);
    say("  watch-heap: watching the first 8 bytes of every page of game memory for the streaming-crash signature (STATUS 40)\n");
}
static void hw_summary(void) {
    sent_summary();
    if (!g_hw_on) {
        if (g_lump_repair) { char b[200]; Sb t = {b, 0, sizeof b}; sb_s(&t, "lump-repair: calls repaired "); sb_d(&t, (i64)g_lump_repaired_call); sb_s(&t, " (the scanner is off without --watch-heap)\n"); say(b); }
        return; }
    char b[700]; Sb t = {b, 0, sizeof b};
    u64 f = plat_ticks_per_sec() / 1000;
    sb_s(&t, "watch-heap: "); sb_d(&t, (i64)g_hw_scans); sb_s(&t, " scans of "); sb_d(&t, (i64)g_hw_pages); sb_s(&t, " pages (");
    sb_d(&t, (i64)(g_hw_scans && f ? g_hw_scan_ticks / f / g_hw_scans : 0)); sb_s(&t, " ms each); signature matches "); sb_d(&t, (i64)g_hw_found1);
    sb_s(&t, ", looser matches "); sb_d(&t, (i64)g_hw_found2); sb_s(&t, ", vtable starts "); sb_d(&t, (i64)g_hw_found3); sb_s(&t, ", low bytes cleared "); sb_d(&t, (i64)g_hw_found4); sb_s(&t, " (of eboot pointers "); sb_d(&t, (i64)g_hw_found5); sb_s(&t, ")");
    sb_s(&t, "; watchpoint traps: damage-shaped "); sb_d(&t, (i64)g_hw_ntrap); sb_s(&t, ", ordinary "); sb_d(&t, (i64)g_hw_ntrap_ord); sb_s(&t, " (damage writes printed: "); sb_d(&t, (i64)g_hw_suspect); sb_s(&t, ")"); sb_s(&t, "; lump repairs by the scanner "); sb_d(&t, (i64)g_lump_repaired_scan); sb_s(&t, ", at calls "); sb_d(&t, (i64)g_lump_repaired_call); sb_s(&t, "; our CPU writes journalled "); sb_d(&t, (i64)g_hw_nev); sb_s(&t, "\n");
    say(b);
}
#endif
