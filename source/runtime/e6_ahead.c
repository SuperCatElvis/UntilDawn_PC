/* w11_load: read-ahead of the game's file reads (--read-ahead, default off).
   The game streams its data one read after another on its own I/O threads (SceFiosIO, Lump Loader); a scene shows as
   soon as its first files are in, the rest (textures, the level's lighting set) follows in the order of that queue.
   When the disk is slow or busy (sessions 60_06 / fast_04: 0.2-3.3 s a read while test jobs and a capture file used
   the disk) the picture is black or unlit for seconds.
   The order of the reads is the same from run to run. Here every read of a game file (path, offset, length) is noted;
   at exit the list is written to <cache dir>\read_order.bin. In the next run, when the game makes a read that is in
   the list, a helper thread reads the next g_ra_n entries of the list into a scratch buffer of its own: Windows keeps
   them in its file cache and the game's own read of them is a memory copy. Nothing changes for the game: its reads
   are made exactly as before; no game memory is touched; the scratch buffer is 1 MB; the tables are ~6 MB. */
enum { RA_MAXPATH = 16384, RA_ARENA = 3 << 20, RA_MAXRD = 131072, RA_HASH = 262144, RA_CAP = 16 << 20 };
typedef struct { u32 pid, len; u64 off; } RaRd;
static int g_ra_on, g_ra_n = 24, g_ra_ready, g_opt_read_delay_ms;
static const char *g_ra_file;
static PMutex g_ra_m; static PCond g_ra_c;
static char *g_ra_arena; static u32 g_ra_arena_n; static u32 *g_ra_poff; static u64 *g_ra_phash; static u32 g_ra_np;
static u32 *g_ra_pht;                                  /* path hash table: index + 1 */
static RaRd *g_ra_prev, *g_ra_cur; static u32 g_ra_nprev, g_ra_ncur;
static u32 *g_ra_rht, *g_ra_cht;                       /* (pid, off) -> first index + 1 in prev / cur */
static u32 *g_ra_stamp;                                /* prev entry: second it was last read ahead (0: never) */
static u32 g_ra_last, g_ra_req_lo, g_ra_req_hi;        /* last matched prev index; wanted range [lo, hi) */
static volatile u64 g_ra_n_match, g_ra_n_miss, g_ra_n_pf, g_ra_pf_bytes, g_ra_pf_ticks, g_ra_n_late, g_ra_n_fail;
static volatile u32 g_ra_doing = 0xffffffffu;
static u64 ra_hash_s(const char *s) { u64 h = 1469598103934665603ull; for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; } return h ? h : 1; }
static u64 ra_key(u32 pid, u64 off) { u64 h = (u64)pid * 0x9E3779B97F4A7C15ull ^ (off + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full; return h ^ (h >> 29); }
static int ra_path_id(const char *p) {   /* (g_ra_m held) -1: table full */
    u64 h = ra_hash_s(p); u32 s = (u32)h & (RA_MAXPATH * 2 - 1);
    for (;;) { u32 v = g_ra_pht[s]; if (!v) break; if (g_ra_phash[v - 1] == h && str_eq(g_ra_arena + g_ra_poff[v - 1], p)) return (int)(v - 1); s = (s + 1) & (RA_MAXPATH * 2 - 1); }
    int n = str_len(p);
    if (g_ra_np >= RA_MAXPATH || g_ra_arena_n + (u32)n + 1 > RA_ARENA) return -1;
    g_ra_poff[g_ra_np] = g_ra_arena_n; g_ra_phash[g_ra_np] = h; for (int i = 0; i <= n; i++) g_ra_arena[g_ra_arena_n + (u32)i] = p[i];
    g_ra_arena_n += (u32)n + 1; g_ra_pht[s] = ++g_ra_np; return (int)(g_ra_np - 1);
}
static u32 ra_find(const u32 *ht, const RaRd *a, u32 pid, u64 off) {   /* index + 1, 0: not there */
    u32 s = (u32)ra_key(pid, off) & (RA_HASH - 1);
    for (;;) { u32 v = ht[s]; if (!v) return 0; if (a[v - 1].pid == pid && a[v - 1].off == off) return v; s = (s + 1) & (RA_HASH - 1); }
}
static void ra_put(u32 *ht, const RaRd *a, u32 i) {
    u32 s = (u32)ra_key(a[i].pid, a[i].off) & (RA_HASH - 1);
    for (;;) { u32 v = ht[s]; if (!v) { ht[s] = i + 1; return; } if (a[v - 1].pid == a[i].pid && a[v - 1].off == a[i].off) return; s = (s + 1) & (RA_HASH - 1); }
}
static int map_path(char *out, const char *p);
static void ra_thread(void *arg) {
    (void)arg; u8 *buf = (u8 *)plat_alloc_rw(1 << 20); if (!buf) return;
    for (;;) {
        pm_lock(&g_ra_m);
        while (g_ra_req_lo >= g_ra_req_hi) pc_wait(&g_ra_c, &g_ra_m, 500000);
        u32 j = g_ra_req_lo++; RaRd e = g_ra_prev[j]; char p[1100]; const char *sp = g_ra_arena + g_ra_poff[e.pid]; int k = 0; while (sp[k] && k < 1099) { p[k] = sp[k]; k++; } p[k] = 0;
        u32 now = (u32)(plat_ticks() / plat_ticks_per_sec()) + 1; int skip = g_ra_stamp[j] && now - g_ra_stamp[j] < 60;
        if (!skip) { g_ra_stamp[j] = now; g_ra_doing = j; }
        pm_unlock(&g_ra_m);
        if (skip) continue;
        char hp[1100]; if (map_path(hp, p) != 1) { g_ra_doing = 0xffffffffu; continue; }   /* the game folder only (read-only) */
        u64 t0 = plat_ticks(); void *f = lp_CreateFile(hp, 0x80000000u, 3, 3, 0x80 | 0x08000000 /* FILE_FLAG_SEQUENTIAL_SCAN */);
        if (f == (void *)-1) { __atomic_add_fetch(&g_ra_n_fail, 1, __ATOMIC_RELAXED); g_ra_doing = 0xffffffffu; continue; }
        u64 left = e.len > RA_CAP ? RA_CAP : e.len, tot = 0; u32 err = 0;
        if (g_opt_read_delay_ms) plat_sleep_ms((u32)g_opt_read_delay_ms);   /* (test: the helper reads from the same slow disk) */
        if (hf_seek(f, (i64)e.off, 0) >= 0) while (left) { u32 c = left > (1u << 20) ? (1u << 20) : (u32)left; u64 got = hf_read_raw(f, buf, c, &err); tot += got; left -= got; if (got < c) break; }
        CloseHandle(f);
        __atomic_add_fetch(&g_ra_n_pf, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_ra_pf_bytes, tot, __ATOMIC_RELAXED); __atomic_add_fetch(&g_ra_pf_ticks, plat_ticks() - t0, __ATOMIC_RELAXED);
        g_ra_doing = 0xffffffffu;
    }
}
static void ra_init(void) {
    if (!g_ra_on || g_ra_ready || !g_ra_file) return;
    pm_init(&g_ra_m); pc_init(&g_ra_c);
    g_ra_arena = (char *)plat_alloc_rw(RA_ARENA); g_ra_poff = (u32 *)plat_alloc_rw(RA_MAXPATH * 4); g_ra_phash = (u64 *)plat_alloc_rw(RA_MAXPATH * 8); g_ra_pht = (u32 *)plat_alloc_rw(RA_MAXPATH * 2 * 4);
    g_ra_prev = (RaRd *)plat_alloc_rw(RA_MAXRD * sizeof(RaRd)); g_ra_cur = (RaRd *)plat_alloc_rw(RA_MAXRD * sizeof(RaRd));
    g_ra_rht = (u32 *)plat_alloc_rw(RA_HASH * 4); g_ra_cht = (u32 *)plat_alloc_rw(RA_HASH * 4); g_ra_stamp = (u32 *)plat_alloc_rw(RA_MAXRD * 4);
    if (!g_ra_arena || !g_ra_poff || !g_ra_phash || !g_ra_pht || !g_ra_prev || !g_ra_cur || !g_ra_rht || !g_ra_cht || !g_ra_stamp) { say("  --read-ahead: no memory for its tables - off\n"); g_ra_on = 0; return; }
    u64 sz = 0; u8 *d = (u8 *)plat_read_file(g_ra_file, &sz);
    if (d && sz >= 16 && d[0] == 'U' && d[1] == 'D' && d[2] == 'R' && d[3] == 'A' && *(u32 *)(d + 4) == 1) {
        u32 np = *(u32 *)(d + 8), nr = *(u32 *)(d + 12); u64 o = 16; int ok = np <= RA_MAXPATH && nr <= RA_MAXRD;
        static u32 map_[RA_MAXPATH];
        for (u32 i = 0; ok && i < np; i++) {
            if (o + 2 > sz) { ok = 0; break; } u32 n = *(u16 *)(d + o); o += 2; if (n >= 1090 || o + n > sz) { ok = 0; break; }
            char t[1100]; for (u32 k = 0; k < n; k++) t[k] = (char)d[o + k]; t[n] = 0; o += n; int id = ra_path_id(t); if (id < 0) { ok = 0; break; } map_[i] = (u32)id;
        }
        if (ok && o + (u64)nr * 16 <= sz) { for (u32 i = 0; i < nr; i++) { const RaRd *e = (const RaRd *)(d + o + (u64)i * 16); if (e->pid >= np) continue; g_ra_prev[g_ra_nprev].pid = map_[e->pid]; g_ra_prev[g_ra_nprev].len = e->len; g_ra_prev[g_ra_nprev].off = e->off; ra_put(g_ra_rht, g_ra_prev, g_ra_nprev); g_ra_nprev++; } }
        else if (!ok) say("  --read-ahead: read_order.bin not as expected - started anew\n");
    }
    if (d) plat_free_pages(d);
    g_ra_last = 0xffffffffu; g_ra_ready = 1;
    say("  read-ahead of game files on (w11_load, --read-ahead): "); say_dec((i64)g_ra_nprev); say(" reads of "); say_dec((i64)g_ra_np); say(" files known from earlier runs; "); say_dec(g_ra_n);
    say(" reads ahead of the game's on a helper thread (into Windows' file cache; the game's own reads are unchanged)\n");
    if (g_ra_nprev) pt_create(ra_thread, 0, 1u << 20);
}
static int ra_open(const char *p) {   /* a game file was opened: its path id (or -1) */
    if (!g_ra_ready) return -1;
    pm_lock(&g_ra_m); int id = ra_path_id(p); pm_unlock(&g_ra_m); return id;
}
static int ra_read(int pid, i64 off, u64 n) {   /* before a read of the game's; 1: this read was read ahead already */
    if (!g_ra_ready || pid < 0 || off < 0 || !n) return 0;
    int hit = 0;
    pm_lock(&g_ra_m);
    RaRd e = {(u32)pid, n > 0xffffffffull ? 0xffffffffu : (u32)n, (u64)off};
    if (g_ra_ncur < RA_MAXRD && !ra_find(g_ra_cht, g_ra_cur, e.pid, e.off)) { g_ra_cur[g_ra_ncur] = e; ra_put(g_ra_cht, g_ra_cur, g_ra_ncur); g_ra_ncur++; }
    if (g_ra_nprev) {
        u32 i = 0xffffffffu;
        for (u32 k = 1; k <= 8 && g_ra_last != 0xffffffffu && g_ra_last + k < g_ra_nprev; k++) if (g_ra_prev[g_ra_last + k].pid == e.pid && g_ra_prev[g_ra_last + k].off == e.off) { i = g_ra_last + k; break; }
        if (i == 0xffffffffu) { u32 v = ra_find(g_ra_rht, g_ra_prev, e.pid, e.off); if (v) i = v - 1; }
        if (i != 0xffffffffu) {
            g_ra_n_match++; g_ra_last = i;
            if (!g_ra_stamp[i] || g_ra_doing == i) g_ra_n_late++;   /* (the game got there first) */
            else hit = 1;
            u32 lo = i + 1, hi = i + 1 + (u32)g_ra_n; if (hi > g_ra_nprev) hi = g_ra_nprev;
            if (lo < hi) { if (g_ra_req_lo < lo || g_ra_req_lo > hi) g_ra_req_lo = lo; g_ra_req_hi = hi; if (g_ra_req_lo < g_ra_req_hi) pc_broadcast(&g_ra_c); }
        } else g_ra_n_miss++;
    }
    pm_unlock(&g_ra_m);
    return hit;
}
static void ra_save(void) {   /* at exit: this run's reads first, then the known ones this run did not make */
    if (!g_ra_ready) return;
    pm_lock(&g_ra_m);
    u64 f_ = plat_ticks_per_sec() / 1000; if (!f_) f_ = 1;
    say("read-ahead (w11_load): the game's reads found in the list "); say_dec((i64)g_ra_n_match); say(", not in it "); say_dec((i64)g_ra_n_miss); say("; read ahead "); say_dec((i64)g_ra_n_pf);
    say(" ("); say_dec((i64)(g_ra_pf_bytes >> 20)); say(" MB, "); say_dec((i64)(g_ra_pf_ticks / f_)); say(" ms on the helper thread), the game was there first "); say_dec((i64)g_ra_n_late);
    say(", files not opened "); say_dec((i64)g_ra_n_fail); say("; this run's reads "); say_dec((i64)g_ra_ncur); say("\n");
    if (g_ra_ncur >= 16) {
        u32 nr = g_ra_ncur; for (u32 i = 0; i < g_ra_nprev && nr < RA_MAXRD; i++) if (!ra_find(g_ra_cht, g_ra_cur, g_ra_prev[i].pid, g_ra_prev[i].off)) g_ra_cur[nr++] = g_ra_prev[i];
        char tmp[1200]; { int k = 0; while (g_ra_file[k] && k < 1190) { tmp[k] = g_ra_file[k]; k++; } tmp[k++] = '.'; tmp[k++] = 't'; tmp[k++] = 'm'; tmp[k++] = 'p'; tmp[k] = 0; }
        void *f = plat_open_write(tmp);
        if (f) {
            u32 h[4] = {0x41524455u /* UDRA */, 1, g_ra_np, nr}; plat_write_bin(f, h, 16);
            for (u32 i = 0; i < g_ra_np; i++) { const char *s = g_ra_arena + g_ra_poff[i]; u16 n = (u16)str_len(s); plat_write_bin(f, &n, 2); plat_write_bin(f, s, n); }
            plat_write_bin(f, g_ra_cur, (u64)nr * 16); plat_close(f); plat_replace_file(tmp, g_ra_file);
        }
    }
    g_ra_ready = 0;
    pm_unlock(&g_ra_m);
}
