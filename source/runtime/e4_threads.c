/* e4_threads.c - real threads and synchronisation for the PS4 thread APIs (libkernel scePthread*,
 * sceKernel{Sema,EventFlag,Equeue}, libScePosix pthread_* / sem_*), built on the platform layer.
 * Included by e3_hle.c. Every function called by game code uses the System V ABI. */

#define SCE_EBUSY 0x80020010
#define SCE_EINVAL 0x80020016
#define SCE_ETIMEDOUT 0x8002003C
#define SCE_ESRCH 0x80020003
#define PX_EBUSY 16
#define PX_ETIMEDOUT 60
#define SCE_EDEADLK 0x8002000B
#define SCE_EPERM 0x80020001
#define PX_EDEADLK 11
#define PX_EPERM 1

/* ---------------- thread objects ---------------- */
#define MAX_THREADS 512
typedef struct PThread {
    u64 magic;
    void *host;
    void *(SYSV *entry)(void *);
    void *arg, *ret;
    char name[64];
    void *spec[256];
    u8 *tp;
    volatile int done;
    PMutex jm; PCond jc;
    u64 stack;
    int id;
    void *fiber;          /* E5: fiber currently running on this thread */
    volatile int last_ix; /* last OS function called on this thread (sampling profiler) */
    volatile u64 last_ret, last_a0;   /* where in the game it was called from, and its first argument */
    void *volatile wait_obj; volatile u64 wait_ret;   /* the mutex a "mutex lock" wait is for, and the game code that asked */
    const char *volatile wait;   /* what this thread is blocked in right now, or 0 (sampling profiler) */
    u32 h_n; u16 h_ix[32]; u64 h_ret[32], h_a[32], h_b[32], h_c[32];   /* this thread's last 32 OS calls (printed at a crash) */
    int err;              /* this thread's errno (__error), STATUS 41 */
    int ps4_prio; u64 ps4_aff;   /* t133: what the game asked for (--ps4-sched honours it) */
} PThread;
/* t133 (--ps4-sched, experiment for the memory damage): the game's threads run on 6 logical processors of 6 different
   physical cores, as on the PS4, with the PS4 priorities mapped onto Windows' (256..300 highest, ..650 above normal,
   ..699 above normal, 700 normal, above that below normal). A PS4 affinity mask names cores 0-5 of those six. */
static int g_opt_ps4_sched; static u64 g_ps4_cores; static u64 g_ps4_lp[6]; static int g_ps4_n_applied;
static int ps4_winprio(int p) { return p <= 300 ? 2 : p <= 699 ? 1 : p == 700 ? 0 : -1; }
static u64 ps4_mask_to_win(u64 m) { if (!g_ps4_cores) return 0; u64 r = 0; for (int i = 0; i < 6; i++) if (m & (1ull << i)) r |= g_ps4_lp[i]; return r ? r : g_ps4_cores; }
static void ps4_sched_init(void) {
    if (!g_opt_ps4_sched) return;
    g_ps4_cores = plat_six_core_mask(); if (!g_ps4_cores) { say("  --ps4-sched: fewer than 6 cores or no CPU-set information - not applied\n"); g_opt_ps4_sched = 0; return; }
    { int n = 0; for (int b = 0; b < 64 && n < 6; b++) if (g_ps4_cores & (1ull << b)) g_ps4_lp[n++] = 1ull << b; }
    say("  PS4 scheduling (t133, --ps4-sched): the game's threads on 6 physical cores (processor mask "); say_hex(g_ps4_cores); say(") with the PS4 priorities mapped onto Windows'\n");
}
static void ps4_sched_apply(void *h, int prio, u64 aff) {
    if (!g_opt_ps4_sched || !h) return;
    plat_thread_affinity(h, aff ? ps4_mask_to_win(aff) : g_ps4_cores);
    plat_thread_priority(h, ps4_winprio(prio));
    g_ps4_n_applied++;
}
static PThread *g_threads[MAX_THREADS];
static int g_nthreads;
static PMutex g_reg_lock;
static int g_threads_started;

static void copy_name(char *d, const char *s, int n) { int i = 0; if (s) for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }

static PThread *thread_new(const char *name) {
    PThread *t = (PThread *)calloc(1, sizeof(PThread));
    t->magic = 0x5448524541445053ull;
    copy_name(t->name, name ? name : "?", sizeof t->name);
    pm_init(&t->jm); pc_init(&t->jc);
    pm_lock(&g_reg_lock);
    t->id = g_nthreads;
    if (g_nthreads < MAX_THREADS) g_threads[g_nthreads++] = t;
    pm_unlock(&g_reg_lock);
    return t;
}
static PThread *cur_thread(void) { return (PThread *)pt_get_current(); }
/* errno is per thread, as on the PS4 (STATUS 41): it was one variable shared by every thread, so a thread could read
   another thread's value between its own call and its errno check - the Bink sound thread's timed wait saw neither
   ETIMEDOUT nor EINTR and the game stopped itself (ud2 at eboot+0x1609d70). Threads we did not create share g_errno. */
static int *thread_errno(void) { PThread *t = cur_thread(); return t ? &t->err : &g_errno; }
static const char *cur_thread_name(void) { PThread *t = cur_thread(); return t ? t->name : "(host)"; }
/* fatal error in the player build: stop every other game thread before the message box, so the game does not go on
   running half-broken behind it (it used to keep rendering until a loading screen waited forever for the dead thread) */
static void pt_suspend_others(void) {
    PThread *me = cur_thread();
    for (int i = 0; i < g_nthreads; i++) { PThread *t = g_threads[i]; if (t && t != me && t->host && !t->done) plat_suspend_thread(t->host); }
}
static void note_thread_call(int idx, u64 ret, u64 a, u64 b, u64 c) {
    PThread *t = cur_thread(); if (!t) return;
    t->last_ix = idx + 1; t->last_ret = ret; t->last_a0 = a;
    u32 k = t->h_n++ & 31; t->h_ix[k] = (u16)idx; t->h_ret[k] = ret; t->h_a[k] = a; t->h_b[k] = b; t->h_c[k] = c;
}
/* at a crash: the OS calls this thread made last, oldest first (what the game was doing just before) */
static void where(u64 a);
static void pt_report_history(void) {
    PThread *t = cur_thread(); if (!t || !t->h_n) return;
    say("    last OS calls on this thread (oldest first):\n");
    for (u32 i = t->h_n > 32 ? t->h_n - 32 : 0; i < t->h_n; i++) { u32 k = i & 31;
        say("      "); say(g_imports[t->h_ix[k]].name); say(" from "); where(t->h_ret[k]);
        say(" args "); say_hex(t->h_a[k]); say(" "); say_hex(t->h_b[k]); say(" "); say_hex(t->h_c[k]); say("\n"); }
}
static const char *wait_begin(const char *what) { PThread *t = cur_thread(); const char *o = 0; if (t) { o = t->wait; t->wait = what; } return o; }
static void wait_end(const char *o) { PThread *t = cur_thread(); if (t) t->wait = o; }
#define WAITING(what, stmt) do { const char *wo_ = wait_begin(what); stmt; wait_end(wo_); } while (0)

static u8 *make_thread_block(void);
static void thread_main(void *p) {
    PThread *t = (PThread *)p;
    pt_set_current(t);
    t->tp = make_thread_block();
    plat_set_thread_pointer(t->tp);
    plat_fp_thread_init();
    ps4_sched_apply(plat_this_thread(), t->ps4_prio, t->ps4_aff);   /* t133 */
    t->ret = t->entry(t->arg);
    pm_lock(&t->jm); t->done = 1; pc_broadcast(&t->jc); pm_unlock(&t->jm);
}

/* attributes: ScePthreadAttr / pthread_attr_t are pointers to these */
typedef struct { u64 stack; int detach, prio, policy, inherit; u64 affinity; } PAttr;
static PAttr *attr_of(u64 *a) {
    if (!a) return 0;
    if (*a < 0x10000) { PAttr *x = (PAttr *)calloc(1, sizeof(PAttr)); x->stack = 0x100000; x->prio = 700; *a = (u64)x; }
    return (PAttr *)*a;
}
static SYSV int t_attr_init(u64 *a) { if (a) { *a = 0; attr_of(a); } return 0; }
static SYSV int t_attr_destroy(u64 *a) { (void)a; return 0; }
static SYSV int t_attr_setstacksize(u64 *a, u64 sz) { PAttr *x = attr_of(a); if (x) x->stack = sz; return 0; }
static SYSV int t_attr_getstacksize(u64 *a, u64 *sz) { PAttr *x = attr_of(a); if (x && sz) *sz = x->stack; return 0; }
static SYSV int t_attr_setdetach(u64 *a, int d) { PAttr *x = attr_of(a); if (x) x->detach = d; return 0; }
static SYSV int t_attr_getdetach(u64 *a, int *d) { PAttr *x = attr_of(a); if (x && d) *d = x->detach; return 0; }
static SYSV int t_attr_ok2(u64 *a, u64 v) { (void)attr_of(a); (void)v; return 0; }
/* t129: what the game asks for (recorded, logged at creation; not honoured yet - the PS4 schedules threads FIFO per core,
   so the game may rely on two threads never running at once) */
static SYSV int t_attr_setaffinity(u64 *a, u64 m) { PAttr *x = attr_of(a); if (x) x->affinity = m; return 0; }
static SYSV int t_attr_setschedparam(u64 *a, const int *p) { PAttr *x = attr_of(a); if (x && p) x->prio = *p; return 0; }
static SYSV int t_attr_setschedpolicy(u64 *a, int pol) { PAttr *x = attr_of(a); if (x) x->policy = pol; return 0; }
static void thread_sched_log(const char *what, const char *tn, u64 mask, int prio) {
    pm_lock(&g_log_lock); tr("    thread "); tr(what); tr(": \""); tr(tn); tr("\" affinity "); tr_hex(mask); tr(" priority "); tr_dec(prio); tr("\n"); pm_unlock(&g_log_lock);
}
static void *ps4_handle_of(PThread *x) { return !x ? 0 : x == cur_thread() ? plat_this_thread() : x->host; }
static SYSV int t_setaffinity(u64 t, u64 m) { PThread *x = (PThread *)t; thread_sched_log("set affinity", x ? x->name : "?", m, -1); if (x && g_opt_ps4_sched) { x->ps4_aff = m; ps4_sched_apply(ps4_handle_of(x), x->ps4_prio, m); } return 0; }
static SYSV int t_setprio(u64 t, int p) { PThread *x = (PThread *)t; thread_sched_log("set priority", x ? x->name : "?", 0, p); if (x && g_opt_ps4_sched) { x->ps4_prio = p; ps4_sched_apply(ps4_handle_of(x), p, x->ps4_aff); } return 0; }
static SYSV int t_setschedparam(u64 t, int pol, const int *p) { PThread *x = (PThread *)t; (void)pol; thread_sched_log("set sched param", x ? x->name : "?", 0, p ? *p : -1); if (x && p && g_opt_ps4_sched) { x->ps4_prio = *p; ps4_sched_apply(ps4_handle_of(x), *p, x->ps4_aff); } return 0; }
static SYSV int t_attr_getschedparam(u64 *a, int *p) { (void)a; if (p) *p = 700; return 0; }
static SYSV int t_attr_getaffinity(u64 *a, u64 *m) { (void)a; if (m) *m = 0x7f; return 0; }

static int thread_create(u64 *out, u64 *attr, void *entry, void *arg, const char *name) {
    PThread *t = thread_new(name);
    t->entry = (void *(SYSV *)(void *))entry; t->arg = arg;
    PAttr *x = attr ? attr_of(attr) : 0;
    u64 st = x && x->stack ? x->stack : 0x100000;
    if (st < 0x100000) st = 0x100000;       /* our HLE code also runs on game thread stacks */
    t->stack = st;
    t->ps4_prio = x ? x->prio : 700; t->ps4_aff = x ? x->affinity : 0;   /* t133 */
    if (out) *out = (u64)t;
    pm_lock(&g_log_lock);
    tr("    thread started: \""); tr(t->name); tr("\" entry eboot+"); tr_hex((u64)entry - (u64)g_eboot.base);
    tr(" stack "); tr_hex(st); tr(" by \""); tr(cur_thread_name()); tr("\"");
    if (x) { tr(" affinity "); tr_hex(x->affinity); tr(" priority "); tr_dec(x->prio); tr(" policy "); tr_dec(x->policy); }
    tr("\n");
    pm_unlock(&g_log_lock);
    t->host = pt_create(thread_main, t, st);
    g_threads_started++;
    return t->host ? 0 : 1;
}
static SYSV int t_create(u64 *out, u64 *attr, void *entry, void *arg, const char *name) {
    TRACE("scePthreadCreate", entry, arg, name);
    return thread_create(out, attr, entry, arg, name) ? (int)0x8002000B : 0;
}
static SYSV int t_px_create(u64 *out, u64 *attr, void *entry, void *arg) {
    TRACE("pthread_create", entry, arg, 0);
    return thread_create(out, attr, entry, arg, "pthread");
}
static SYSV int t_px_create_named(u64 *out, u64 *attr, void *entry, void *arg, const char *name) {
    TRACE("pthread_create_name_np", entry, arg, name);
    return thread_create(out, attr, entry, arg, name);
}
static SYSV int t_join(u64 th, void **ret) {
    PThread *t = (PThread *)th;
    if (!t || t->magic != 0x5448524541445053ull) return (int)SCE_ESRCH;
    pm_lock(&t->jm); while (!t->done) WAITING("thread join", pc_wait(&t->jc, &t->jm, -1)); pm_unlock(&t->jm);
    if (ret) *ret = t->ret;
    return 0;
}
static SYSV void t_exit(void *ret) {
    PThread *t = cur_thread();
    if (!t || t->id == 0) { fatal_from_game("scePthreadExit on the main thread"); return; }
    t->ret = ret;
    pm_lock(&t->jm); t->done = 1; pc_broadcast(&t->jc); pm_unlock(&t->jm);
    pt_exit();
}
static SYSV void *t_self(void) { return cur_thread(); }
static SYSV int t_equal(u64 a, u64 b) { return a == b; }
/* t107h: when there was nothing else to run, a short pause before returning. The game's wait loops (the main thread
   waiting for the GPU's frame counter: 50% of its time with asynchronous submits) called this back to back, a system
   call each time, on a core whose other hyperthread may be the renderer's; PAUSE leaves the core to the other thread.
   The loops still see their condition within a few microseconds. */
static SYSV int t_yield(void) { WAITING("yield", { if (!pt_yield2()) for (int k_ = 0; k_ < 48; k_++) __builtin_ia32_pause(); }); return 0; }
static SYSV int t_getprio(u64 t, int *p) { (void)t; if (p) *p = 700; return 0; }
static SYSV int t_getschedparam(u64 t, int *pol, int *p) { (void)t; if (pol) *pol = 1; if (p) *p = 700; return 0; }
static SYSV int t_getaffinity(u64 t, u64 *m) { (void)t; if (m) *m = 0x7f; return 0; }
static SYSV int t_ok(void) { return 0; }
static SYSV int t_rename(u64 t, const char *n) { PThread *x = (PThread *)t; if (x && x->magic == 0x5448524541445053ull) copy_name(x->name, n, sizeof x->name); return 0; }

/* thread-specific data */
static int g_next_key = 1;
static SYSV int t_key_create(int *k, void *dtor) { (void)dtor; pm_lock(&g_reg_lock); *k = g_next_key++; pm_unlock(&g_reg_lock); return 0; }
static SYSV void *t_getspecific(int k) { PThread *t = cur_thread(); return (t && k > 0 && k < 256) ? t->spec[k] : 0; }
static SYSV int t_setspecific(int k, void *v) { PThread *t = cur_thread(); if (t && k > 0 && k < 256) t->spec[k] = v; return 0; }

/* ---------------- mutexes (recursive host mutex + owner/count for cond waits) ---------------- */
typedef struct { PMutex m; void *owner; int count, type; char name[32]; } PSMutex;
static PSMutex *mutex_of(u64 *h) {
    if (!h) return 0;
    u64 v = __atomic_load_n(h, __ATOMIC_ACQUIRE);
    if (v >= 0x10000) return (PSMutex *)v;          /* small values = static initializers */
    PSMutex *m = (PSMutex *)calloc(1, sizeof(PSMutex));
    pm_init(&m->m);
    u64 exp = v;
    if (!__atomic_compare_exchange_n(h, &exp, (u64)m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { free(m); return (PSMutex *)exp; }
    return m;
}
/* PS4 mutex types (FreeBSD lineage): 1 error-checking (the default), 2 recursive, 3 normal, 4 adaptive.
   E6 used to make every mutex recursive. That changes three answers the game can see: a trylock by the
   owner succeeded (PS4: busy), a second lock by the owner succeeded (PS4: EDEADLK for error-checking, a hang
   for normal), and an unlock by another thread was accepted. Each case is now answered like the PS4 (a normal
   mutex returns EDEADLK instead of hanging) and counted. --recursive-mutexes restores the old behaviour. */
static int g_opt_recursive_mutexes;
static i64 g_mx_try_owner, g_mx_relock, g_mx_bad_unlock, g_mx_types[5];
static int mx_attr_type(const u64 *attr) {   /* attribute objects hold 0x10000 | type (see mx_attr_init / mx_attr_settype) */
    if (attr && *attr >= 0x10000 && *attr <= 0x10004) { int t = (int)(*attr & 0xF); return t ? t : 1; }
    return 1;
}
static SYSV int mx_init(u64 *h, u64 *attr, const char *name) {
    if (!h) return (int)SCE_EINVAL;
    *h = 0; PSMutex *m = mutex_of(h);
    m->type = mx_attr_type(attr);
    __atomic_add_fetch(&g_mx_types[m->type <= 4 ? m->type : 0], 1, __ATOMIC_RELAXED);
    copy_name(m->name, name, sizeof m->name);
    return 0;
}
static SYSV int mx_px_init(u64 *h, u64 *attr) { return mx_init(h, attr, 0); }
static SYSV int mx_destroy(u64 *h) { (void)h; return 0; }
static int mx_recursive(PSMutex *m) { return g_opt_recursive_mutexes || m->type == 2; }
static int mx_owned_by_me(PSMutex *m) { return m->count && m->owner == pt_get_current(); }
static void mx_take(PSMutex *m) { if (mx_owned_by_me(m)) m->count++; else { m->owner = pt_get_current(); m->count = 1; } }
static int mx_lock_impl2(PSMutex *m, u64 ret) {
    if (mx_owned_by_me(m) && !mx_recursive(m)) { __atomic_add_fetch(&g_mx_relock, 1, __ATOMIC_RELAXED); return (int)SCE_EDEADLK; }
    if (pm_trylock(&m->m)) {
        PThread *t = cur_thread(); if (t) { t->wait_obj = m; t->wait_ret = ret; }
        WAITING("mutex lock (contended)", pm_lock(&m->m));
        if (t) t->wait_obj = 0;
    }
    mx_take(m);
    return 0;
}
static int mx_lock_impl(PSMutex *m) { return mx_lock_impl2(m, 0); }
static SYSV __attribute__((noinline)) int mx_lock(u64 *h) { PSMutex *m = mutex_of(h); if (!m) return (int)SCE_EINVAL; return mx_lock_impl2(m, (u64)__builtin_return_address(0)); }
static SYSV __attribute__((noinline)) int mx_px_lock(u64 *h) { PSMutex *m = mutex_of(h); int r = m ? mx_lock_impl2(m, (u64)__builtin_return_address(0)) : (int)SCE_EINVAL; return r == (int)SCE_EDEADLK ? PX_EDEADLK : r ? 22 : 0; }
static SYSV int mx_trylock(u64 *h) {
    PSMutex *m = mutex_of(h); if (!m) return (int)SCE_EINVAL;
    if (mx_owned_by_me(m) && !mx_recursive(m)) { __atomic_add_fetch(&g_mx_try_owner, 1, __ATOMIC_RELAXED); return (int)SCE_EBUSY; }
    if (pm_trylock(&m->m)) return (int)SCE_EBUSY;
    mx_take(m);
    return 0;
}
static SYSV int mx_px_trylock(u64 *h) { int r = mx_trylock(h); return r == (int)SCE_EBUSY ? PX_EBUSY : r; }
static SYSV int mx_timedlock(u64 *h, u32 us) {
    PSMutex *m = mutex_of(h); if (!m) return (int)SCE_EINVAL;
    if (mx_owned_by_me(m) && !mx_recursive(m)) { __atomic_add_fetch(&g_mx_relock, 1, __ATOMIC_RELAXED); return (int)SCE_EDEADLK; }
    u64 t0 = plat_ticks(), f = plat_ticks_per_sec();
    while (pm_trylock(&m->m)) {
        if ((plat_ticks() - t0) * 1000000ull / f >= us) return (int)SCE_ETIMEDOUT;
        WAITING("mutex timed lock", pt_yield());
    }
    mx_take(m);
    return 0;
}
static SYSV int mx_unlock(u64 *h) {
    PSMutex *m = mutex_of(h); if (!m) return (int)SCE_EINVAL;
    if (!mx_owned_by_me(m)) { __atomic_add_fetch(&g_mx_bad_unlock, 1, __ATOMIC_RELAXED); return (int)SCE_EPERM; }
    if (--m->count <= 0) { m->count = 0; m->owner = 0; }
    pm_unlock(&m->m);
    return 0;
}
static SYSV int mx_px_unlock(u64 *h) { int r = mx_unlock(h); return r == (int)SCE_EPERM ? PX_EPERM : r ? 22 : 0; }
static SYSV int mx_attr_settype(u64 *a, int type) { if (!a || type < 1 || type > 4) return (int)SCE_EINVAL; *a = 0x10000 | (u64)type; return 0; }
static SYSV int mx_px_attr_settype(u64 *a, int type) { return mx_attr_settype(a, type) ? 22 : 0; }
static SYSV int mx_attr_ok(void) { return 0; }
static SYSV int mx_attr_init(u64 *a) { if (a) *a = 0x10000; return 0; }

/* ---------------- condition variables ---------------- */
typedef struct { PCond c; } PSCond;
static PSCond *cond_of(u64 *h) {
    if (!h) return 0;
    u64 v = __atomic_load_n(h, __ATOMIC_ACQUIRE);
    if (v >= 0x10000) return (PSCond *)v;
    PSCond *c = (PSCond *)calloc(1, sizeof(PSCond));
    pc_init(&c->c);
    u64 exp = v;
    if (!__atomic_compare_exchange_n(h, &exp, (u64)c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { free(c); return (PSCond *)exp; }
    return c;
}
static SYSV int cv_init(u64 *h, u64 *attr, const char *n) { (void)attr; (void)n; if (!h) return (int)SCE_EINVAL; *h = 0; cond_of(h); return 0; }
static SYSV int cv_px_init(u64 *h, u64 *attr) { return cv_init(h, attr, 0); }
static int cv_wait_impl(u64 *ch, u64 *mh, i64 us) {
    PSCond *c = cond_of(ch); PSMutex *m = mutex_of(mh);
    if (!c || !m) return -1;
    int cnt = m->count; void *own = m->owner;
    for (int i = 1; i < cnt; i++) pm_unlock(&m->m);      /* hold exactly once while waiting */
    m->count = 0; m->owner = 0;
    int r; WAITING("condition variable wait", r = pc_wait(&c->c, &m->m, us));
    for (int i = 1; i < cnt; i++) pm_lock(&m->m);
    m->count = cnt; m->owner = own;
    return r;
}
static SYSV int cv_wait(u64 *c, u64 *m) { return cv_wait_impl(c, m, -1) < 0 ? (int)SCE_EINVAL : 0; }
static SYSV int cv_timedwait(u64 *c, u64 *m, u32 us) { int r = cv_wait_impl(c, m, us); return r < 0 ? (int)SCE_EINVAL : r ? (int)SCE_ETIMEDOUT : 0; }
static i64 abstime_to_us(const i64 *ts) {       /* absolute CLOCK_REALTIME deadline -> relative us (the game has no setclock import) */
    if (!ts) return -1;
    i64 now_us = (i64)(plat_wall_ns() / 1000ull);
    i64 abs_us = ts[0] * 1000000 + ts[1] / 1000;
    return abs_us > now_us ? abs_us - now_us : 0;
}
static SYSV int cv_px_timedwait(u64 *c, u64 *m, const i64 *abst) { int r = cv_wait_impl(c, m, abstime_to_us(abst)); return r < 0 ? 22 : r ? PX_ETIMEDOUT : 0; }
static SYSV int cv_signal(u64 *h) { PSCond *c = cond_of(h); if (c) pc_signal(&c->c); return 0; }
static SYSV int cv_broadcast(u64 *h) { PSCond *c = cond_of(h); if (c) pc_broadcast(&c->c); return 0; }

/* ---------------- read/write locks ---------------- */
typedef struct { PMutex m; PCond c; int readers, writer; void *wowner; int wcount; } PSRw;
static PSRw *rw_of(u64 *h) {
    if (!h) return 0;
    u64 v = __atomic_load_n(h, __ATOMIC_ACQUIRE);
    if (v >= 0x10000) return (PSRw *)v;
    PSRw *r = (PSRw *)calloc(1, sizeof(PSRw)); pm_init(&r->m); pc_init(&r->c);
    u64 exp = v;
    if (!__atomic_compare_exchange_n(h, &exp, (u64)r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { free(r); return (PSRw *)exp; }
    return r;
}
static SYSV int rw_init(u64 *h, u64 *a, const char *n) { (void)a; (void)n; *h = 0; rw_of(h); return 0; }
static SYSV int rw_rdlock(u64 *h) {
    PSRw *r = rw_of(h); void *me = pt_get_current();
    pm_lock(&r->m);
    while (r->writer && r->wowner != me) WAITING("rwlock read", pc_wait(&r->c, &r->m, -1));
    r->readers++; pm_unlock(&r->m); return 0;
}
static SYSV int rw_tryrdlock(u64 *h) {
    PSRw *r = rw_of(h); int ok;
    pm_lock(&r->m); ok = !r->writer || r->wowner == pt_get_current(); if (ok) r->readers++; pm_unlock(&r->m);
    return ok ? 0 : (int)SCE_EBUSY;
}
static SYSV int rw_wrlock(u64 *h) {
    PSRw *r = rw_of(h); void *me = pt_get_current();
    pm_lock(&r->m);
    if (r->writer && r->wowner == me) { r->wcount++; pm_unlock(&r->m); return 0; }
    while (r->writer || r->readers) WAITING("rwlock write", pc_wait(&r->c, &r->m, -1));
    r->writer = 1; r->wowner = me; r->wcount = 1; pm_unlock(&r->m); return 0;
}
static SYSV int rw_trywrlock(u64 *h) {
    PSRw *r = rw_of(h); int ok;
    pm_lock(&r->m); ok = !r->writer && !r->readers; if (ok) { r->writer = 1; r->wowner = pt_get_current(); r->wcount = 1; } pm_unlock(&r->m);
    return ok ? 0 : (int)SCE_EBUSY;
}
static SYSV int rw_unlock(u64 *h) {
    PSRw *r = rw_of(h);
    pm_lock(&r->m);
    if (r->writer && r->wowner == pt_get_current()) { if (--r->wcount == 0) { r->writer = 0; r->wowner = 0; } }
    else if (r->readers) r->readers--;
    pc_broadcast(&r->c); pm_unlock(&r->m); return 0;
}

/* ---------------- semaphores ---------------- */
typedef struct { PMutex m; PCond c; int count, max; char name[32]; } PSSema;
static PSSema *sema_new(int init, int max, const char *n) {
    PSSema *s = (PSSema *)calloc(1, sizeof(PSSema)); pm_init(&s->m); pc_init(&s->c);
    s->count = init; s->max = max; copy_name(s->name, n, sizeof s->name); return s;
}
static SYSV int sem_create(u64 *out, const char *n, u32 attr, int init, int max, void *opt) {
    TRACE("sceKernelCreateSema", init, max, n); (void)attr; (void)opt;
    *out = (u64)sema_new(init, max, n); return 0;
}
static int sema_wait(PSSema *s, int need, i64 us) {
    if (!s) return (int)SCE_EINVAL;
    u64 t0 = plat_ticks(), f = plat_ticks_per_sec();
    pm_lock(&s->m);
    while (s->count < need) {
        i64 left = -1;
        if (us >= 0) { left = us - (i64)((plat_ticks() - t0) * 1000000ull / f); if (left <= 0) { pm_unlock(&s->m); return (int)SCE_ETIMEDOUT; } }
        WAITING("semaphore wait", pc_wait(&s->c, &s->m, left));
    }
    s->count -= need; pm_unlock(&s->m); return 0;
}
static SYSV int sem_wait_sce(u64 s, int need, u32 *timeout) { return sema_wait((PSSema *)s, need, timeout ? (i64)*timeout : -1); }
static SYSV int sem_poll_sce(u64 sh, int need) {
    PSSema *s = (PSSema *)sh; int ok;
    pm_lock(&s->m); ok = s->count >= need; if (ok) s->count -= need; pm_unlock(&s->m);
    return ok ? 0 : (int)SCE_EBUSY;
}
static SYSV int sem_signal_sce(u64 sh, int n) { PSSema *s = (PSSema *)sh; if (!s) return (int)SCE_EINVAL; pm_lock(&s->m); s->count += n; pc_broadcast(&s->c); pm_unlock(&s->m); return 0; }
static SYSV int sem_delete_sce(u64 s) { (void)s; return 0; }
/* POSIX sem_t is a pointer on PS4 */
static SYSV int psem_init(u64 *s, int pshared, u32 v) { (void)pshared; *s = (u64)sema_new((int)v, 0x7fffffff, "sem_t"); return 0; }
static SYSV int psem_wait(u64 *s) { return sema_wait((PSSema *)*s, 1, -1) ? -1 : 0; }
static SYSV int psem_trywait(u64 *s) { if (sem_poll_sce(*s, 1)) { *thread_errno() = 35; return -1; } return 0; }
static SYSV int psem_timedwait(u64 *s, const i64 *abst) { if (sema_wait((PSSema *)*s, 1, abstime_to_us(abst))) { *thread_errno() = PX_ETIMEDOUT; return -1; } return 0; }
static SYSV int psem_post(u64 *s) { return sem_signal_sce(*s, 1); }
static SYSV int psem_destroy(u64 *s) { (void)s; return 0; }

/* ---------------- event flags ---------------- */
typedef struct { PMutex m; PCond c; u64 bits; char name[32]; } PSEvf;
static SYSV int evf_create(u64 *out, const char *n, u32 attr, u64 init, void *opt) {
    TRACE("sceKernelCreateEventFlag", attr, init, n); (void)opt;
    PSEvf *e = (PSEvf *)calloc(1, sizeof(PSEvf)); pm_init(&e->m); pc_init(&e->c); e->bits = init; copy_name(e->name, n, sizeof e->name);
    *out = (u64)e; return 0;
}
static int evf_ok(PSEvf *e, u64 pat, u32 mode) { return (mode & 1) ? (e->bits & pat) == pat : (e->bits & pat) != 0; }
static void evf_consume(PSEvf *e, u64 pat, u32 mode) { if (mode & 0x10) e->bits = 0; else if (mode & 0x20) e->bits &= ~pat; }
static SYSV int evf_wait(u64 h, u64 pat, u32 mode, u64 *result, u32 *timeout) {
    PSEvf *e = (PSEvf *)h; if (!e) return (int)SCE_EINVAL;
    i64 us = timeout ? (i64)*timeout : -1;
    u64 t0 = plat_ticks(), f = plat_ticks_per_sec();
    pm_lock(&e->m);
    while (!evf_ok(e, pat, mode)) {
        i64 left = -1;
        if (us >= 0) { left = us - (i64)((plat_ticks() - t0) * 1000000ull / f); if (left <= 0) { if (result) *result = e->bits; pm_unlock(&e->m); if (timeout) *timeout = 0; return (int)SCE_ETIMEDOUT; } }
        WAITING("event flag wait", pc_wait(&e->c, &e->m, left));
    }
    if (result) *result = e->bits;
    evf_consume(e, pat, mode);
    pm_unlock(&e->m); return 0;
}
static SYSV int evf_poll(u64 h, u64 pat, u32 mode, u64 *result) {
    PSEvf *e = (PSEvf *)h; if (!e) return (int)SCE_EINVAL;
    pm_lock(&e->m);
    int ok = evf_ok(e, pat, mode);
    if (result) *result = e->bits;
    if (ok) evf_consume(e, pat, mode);
    pm_unlock(&e->m);
    return ok ? 0 : (int)SCE_EBUSY;
}
static SYSV int evf_set(u64 h, u64 b) { PSEvf *e = (PSEvf *)h; if (!e) return (int)SCE_EINVAL; pm_lock(&e->m); e->bits |= b; pc_broadcast(&e->c); pm_unlock(&e->m); return 0; }
static SYSV int evf_clear(u64 h, u64 b) { PSEvf *e = (PSEvf *)h; if (!e) return (int)SCE_EINVAL; pm_lock(&e->m); e->bits &= b; pm_unlock(&e->m); return 0; }
static SYSV int evf_delete(u64 h) { (void)h; return 0; }

/* ---------------- event queues (kqueue-like) ---------------- */
typedef struct { u64 ident; short filter; u16 flags; u32 fflags; i64 data; void *udata; } SceKEvent;   /* 32 bytes */
typedef struct { u64 ident; short filter; int used, triggered, level; i64 data; void *udata; } EqReg;
typedef struct { PMutex m; PCond c; EqReg reg[64]; char name[32]; } PSEq;
static SYSV int eq_create(u64 *out, const char *n) {
    TRACE("sceKernelCreateEqueue", n, 0, 0);
    PSEq *q = (PSEq *)calloc(1, sizeof(PSEq)); pm_init(&q->m); pc_init(&q->c); copy_name(q->name, n, sizeof q->name);
    *out = (u64)q; return 0;
}
static SYSV int eq_delete(u64 q) { (void)q; return 0; }
static EqReg *eq_find(PSEq *q, u64 ident, short filter, int create) {
    for (int i = 0; i < 64; i++) if (q->reg[i].used && q->reg[i].ident == ident && q->reg[i].filter == filter) return &q->reg[i];
    if (!create) return 0;
    for (int i = 0; i < 64; i++) if (!q->reg[i].used) { h_memset(&q->reg[i], 0, sizeof(EqReg)); q->reg[i].used = 1; q->reg[i].ident = ident; q->reg[i].filter = filter; return &q->reg[i]; }
    return 0;
}
static int eq_add(u64 qh, u64 ident, short filter, void *udata, int level, int triggered) {
    PSEq *q = (PSEq *)qh; if (!q) return (int)SCE_EINVAL;
    pm_lock(&q->m);
    EqReg *r = eq_find(q, ident, filter, 1);
    if (r) { r->udata = udata; r->level = level; r->triggered = triggered; if (triggered) pc_broadcast(&q->c); }
    pm_unlock(&q->m);
    return r ? 0 : (int)0x8002000C;
}
static SYSV int eq_add_user(u64 q, int id) { return eq_add(q, (u64)id, -11, 0, 0, 0); }
static SYSV int eq_add_read(u64 q, int fd, u64 size, void *udata) { (void)size; return eq_add(q, (u64)fd, -1, udata, 1, 1); }
static SYSV int eq_del(u64 qh, int id, short filter) { PSEq *q = (PSEq *)qh; pm_lock(&q->m); EqReg *r = eq_find(q, (u64)id, filter, 0); if (r) r->used = 0; pm_unlock(&q->m); return 0; }
static SYSV int eq_del_user(u64 q, int id) { return eq_del(q, id, -11); }
static SYSV int eq_del_read(u64 q, int fd) { return eq_del(q, fd, -1); }
static SYSV int eq_trigger_user(u64 qh, int id, void *udata) {
    PSEq *q = (PSEq *)qh; if (!q) return (int)SCE_EINVAL;
    pm_lock(&q->m);
    EqReg *r = eq_find(q, (u64)id, -11, 0);
    if (r) { r->triggered = 1; r->udata = udata; pc_broadcast(&q->c); }
    pm_unlock(&q->m);
    return r ? 0 : (int)0x80020002;
}
static SYSV int eq_wait(u64 qh, SceKEvent *ev, int num, int *out, u32 *timeout) {
    PSEq *q = (PSEq *)qh; if (!q) return (int)SCE_EINVAL;
    i64 us = timeout ? (i64)*timeout : -1;
    u64 t0 = plat_ticks(), f = plat_ticks_per_sec();
    pm_lock(&q->m);
    for (;;) {
        int n = 0;
        for (int i = 0; i < 64 && n < num; i++) {
            EqReg *r = &q->reg[i];
            if (!r->used || !r->triggered) continue;
            ev[n].ident = r->ident; ev[n].filter = r->filter; ev[n].flags = 0; ev[n].fflags = 0; ev[n].data = r->data; ev[n].udata = r->udata;
            n++;
            if (!r->level) r->triggered = 0;
        }
        if (n) { if (out) *out = n; pm_unlock(&q->m); return 0; }
        i64 left = -1;
        if (us >= 0) { left = us - (i64)((plat_ticks() - t0) * 1000000ull / f); if (left <= 0) { if (out) *out = 0; pm_unlock(&q->m); return (int)SCE_ETIMEDOUT; } }
        WAITING("event queue wait", pc_wait(&q->c, &q->m, left));
    }
}
static SYSV void *eq_ev_udata(SceKEvent *e) { return e ? e->udata : 0; }
static SYSV int eq_ev_filter(SceKEvent *e) { return e ? e->filter : 0; }
static SYSV i64 eq_ev_data(SceKEvent *e) { return e ? e->data : 0; }
static SYSV u64 eq_ev_id(SceKEvent *e) { return e ? e->ident : 0; }

/* ---------------- sleeping ---------------- */
static SYSV int t_usleep(u32 us) {
    if (us >= 1000) WAITING("usleep", plat_sleep_ms(us / 1000)); else WAITING("usleep (short: yield)", pt_yield());
    return 0;
}
static SYSV u32 t_sleep(u32 s) { WAITING("sleep", plat_sleep_ms(s * 1000)); return 0; }

/* timeout: where is every other game thread? */
static void where(u64 a);
static void sample_other_threads(void) {
    for (int i = 0; i < g_nthreads; i++) {
        PThread *t = g_threads[i];
        if (t == cur_thread()) continue;
        say("\n  ["); say(t->name); say("]");
        if (t->done) { say(" exited\n"); continue; }
        u64 rip, rbp, rsp;
        if (t->last_ix > 0) { say(" last OS call "); say(g_imports[t->last_ix - 1].name); say("(0x"); say_hex(t->last_a0); say(") from "); where(t->last_ret); }
        { const char *w = *(const char *volatile *)&t->wait;   /* read once: the thread clears it when its wait ends */
          if (w) { say(", blocked in: "); say(w); } }
        { PSMutex *wm = (PSMutex *)t->wait_obj;
          if (wm) { PThread *o = (PThread *)wm->owner;
              say(" on mutex "); say_hex((u64)wm); if (wm->name[0]) { say(" \""); say(wm->name); say("\""); }
              if (t->wait_ret) { say(" (locked from "); where(t->wait_ret); say(")"); }
              say(", held by "); say(o && wm->count ? o->name : "(nobody)"); } }
        if (t->host && pt_sample(t->host, &rip, &rbp, &rsp)) {
            report_where(rip, rbp, rsp);
            if (!t->wait) {   /* running: is it making progress? 4 more samples, 50 ms apart */
                say("    more samples:");
                for (int k = 0; k < 4; k++) { plat_sleep_ms(50); u64 r2, b2, s2; if (pt_sample(t->host, &r2, &b2, &s2)) { say(" "); where(r2); } }
                say("\n");
            }
        } else say(" (could not sample)\n");
    }
}

static const HleEntry g_hle_thr[] = {
    {"scePthreadCreate", (void *)t_create}, {"pthread_create", (void *)t_px_create}, {"pthread_create_name_np", (void *)t_px_create_named},
    {"scePthreadJoin", (void *)t_join}, {"pthread_join", (void *)t_join}, {"scePthreadDetach", (void *)t_ok},
    {"scePthreadExit", (void *)t_exit}, {"pthread_exit", (void *)t_exit},
    {"scePthreadSelf", (void *)t_self}, {"pthread_self", (void *)t_self}, {"scePthreadEqual", (void *)t_equal}, {"pthread_equal", (void *)t_equal},
    {"scePthreadYield", (void *)t_yield}, {"scePthreadGetprio", (void *)t_getprio}, {"scePthreadSetprio", (void *)t_setprio},
    {"scePthreadGetschedparam", (void *)t_getschedparam}, {"scePthreadSetschedparam", (void *)t_setschedparam}, {"pthread_setschedparam", (void *)t_setschedparam},
    {"scePthreadGetaffinity", (void *)t_getaffinity}, {"scePthreadSetaffinity", (void *)t_setaffinity}, {"scePthreadRename", (void *)t_rename},
    {"scePthreadAttrInit", (void *)t_attr_init}, {"pthread_attr_init", (void *)t_attr_init},
    {"scePthreadAttrDestroy", (void *)t_attr_destroy}, {"pthread_attr_destroy", (void *)t_attr_destroy},
    {"scePthreadAttrSetstacksize", (void *)t_attr_setstacksize}, {"pthread_attr_setstacksize", (void *)t_attr_setstacksize},
    {"scePthreadAttrSetdetachstate", (void *)t_attr_setdetach}, {"pthread_attr_setdetachstate", (void *)t_attr_setdetach},
    {"scePthreadAttrSetinheritsched", (void *)t_attr_ok2}, {"scePthreadAttrSetschedparam", (void *)t_attr_setschedparam},
    {"pthread_attr_setschedparam", (void *)t_attr_setschedparam}, {"scePthreadAttrSetschedpolicy", (void *)t_attr_setschedpolicy},
    {"scePthreadAttrSetaffinity", (void *)t_attr_setaffinity}, {"scePthreadAttrGetschedparam", (void *)t_attr_getschedparam},
    {"scePthreadKeyCreate", (void *)t_key_create}, {"scePthreadGetspecific", (void *)t_getspecific}, {"scePthreadSetspecific", (void *)t_setspecific},
    {"scePthreadMutexInit", (void *)mx_init}, {"pthread_mutex_init", (void *)mx_px_init},
    {"scePthreadMutexDestroy", (void *)mx_destroy}, {"pthread_mutex_destroy", (void *)mx_destroy},
    {"scePthreadMutexLock", (void *)mx_lock}, {"pthread_mutex_lock", (void *)mx_px_lock},
    {"scePthreadMutexTrylock", (void *)mx_trylock}, {"pthread_mutex_trylock", (void *)mx_px_trylock},
    {"scePthreadMutexTimedlock", (void *)mx_timedlock},
    {"scePthreadMutexUnlock", (void *)mx_unlock}, {"pthread_mutex_unlock", (void *)mx_px_unlock},
    {"scePthreadMutexattrInit", (void *)mx_attr_init}, {"pthread_mutexattr_init", (void *)mx_attr_init},
    {"scePthreadMutexattrSettype", (void *)mx_attr_settype}, {"pthread_mutexattr_settype", (void *)mx_px_attr_settype},
    {"scePthreadMutexattrDestroy", (void *)mx_attr_ok}, {"pthread_mutexattr_destroy", (void *)mx_attr_ok},
    {"scePthreadMutexattrSetprotocol", (void *)mx_attr_ok},
    {"scePthreadCondInit", (void *)cv_init}, {"pthread_cond_init", (void *)cv_px_init},
    {"scePthreadCondDestroy", (void *)mx_destroy}, {"pthread_cond_destroy", (void *)mx_destroy},
    {"scePthreadCondWait", (void *)cv_wait}, {"pthread_cond_wait", (void *)cv_wait},
    {"scePthreadCondTimedwait", (void *)cv_timedwait}, {"pthread_cond_timedwait", (void *)cv_px_timedwait},
    {"scePthreadCondSignal", (void *)cv_signal}, {"pthread_cond_signal", (void *)cv_signal},
    {"scePthreadCondBroadcast", (void *)cv_broadcast}, {"pthread_cond_broadcast", (void *)cv_broadcast},
    {"scePthreadCondattrInit", (void *)mx_attr_init},
    {"scePthreadRwlockInit", (void *)rw_init}, {"scePthreadRwlockDestroy", (void *)mx_destroy},
    {"scePthreadRwlockRdlock", (void *)rw_rdlock}, {"scePthreadRwlockTryrdlock", (void *)rw_tryrdlock},
    {"scePthreadRwlockWrlock", (void *)rw_wrlock}, {"scePthreadRwlockTrywrlock", (void *)rw_trywrlock},
    {"scePthreadRwlockUnlock", (void *)rw_unlock},
    {"sceKernelCreateSema", (void *)sem_create}, {"sceKernelWaitSema", (void *)sem_wait_sce}, {"sceKernelPollSema", (void *)sem_poll_sce},
    {"sceKernelSignalSema", (void *)sem_signal_sce}, {"sceKernelDeleteSema", (void *)sem_delete_sce},
    {"sem_init", (void *)psem_init}, {"sem_wait", (void *)psem_wait}, {"sem_trywait", (void *)psem_trywait},
    {"sem_timedwait", (void *)psem_timedwait}, {"sem_post", (void *)psem_post}, {"sem_destroy", (void *)psem_destroy},
    {"sceKernelCreateEventFlag", (void *)evf_create}, {"sceKernelWaitEventFlag", (void *)evf_wait}, {"sceKernelPollEventFlag", (void *)evf_poll},
    {"sceKernelSetEventFlag", (void *)evf_set}, {"sceKernelClearEventFlag", (void *)evf_clear}, {"sceKernelDeleteEventFlag", (void *)evf_delete},
    {"sceKernelCreateEqueue", (void *)eq_create}, {"sceKernelDeleteEqueue", (void *)eq_delete},
    {"sceKernelAddUserEvent", (void *)eq_add_user}, {"sceKernelAddUserEventEdge", (void *)eq_add_user},
    {"sceKernelDeleteUserEvent", (void *)eq_del_user}, {"sceKernelTriggerUserEvent", (void *)eq_trigger_user},
    {"sceKernelAddReadEvent", (void *)eq_add_read}, {"sceKernelDeleteReadEvent", (void *)eq_del_read},
    {"sceKernelWaitEqueue", (void *)eq_wait}, {"sceKernelGetEventUserData", (void *)eq_ev_udata},
    {"sceKernelGetEventFilter", (void *)eq_ev_filter}, {"sceKernelGetEventData", (void *)eq_ev_data}, {"sceKernelGetEventId", (void *)eq_ev_id},
    {"sceKernelUsleep", (void *)t_usleep}, {"usleep", (void *)t_usleep}, {"sleep", (void *)t_sleep},
    {0, 0}};
