/* e5_fiber.c - libSceFiber.
 * Linux: user-mode fibers on the game-provided stacks with a hand-written context switch.
 * Windows: native Windows fibers (CreateFiberEx/SwitchToFiber), because Windows tracks each thread's stack
 * (TEB bounds, and on CET-capable CPUs a hardware shadow stack) and kills a process that switches stacks
 * by hand. The game's own stack memory is then simply unused; nothing in the game depends on it.
 * Either way a fiber keeps the thread's PS4 thread pointer (TLS), exactly like on the console.
 * Included by e3_hle.c. */

#define FIBER_MAGIC 0x5245424946454455ull   /* "UDEFIBER" */
#define SCE_FIBER_ERROR_NULL 0x80590001
#define SCE_FIBER_ERROR_INVALID 0x80590004
#define SCE_FIBER_ERROR_STATE 0x80590006
#define SCE_FIBER_ERROR_PERMISSION 0x80590005

typedef struct FiberHost {
    u64 rsp;                    /* saved stack pointer while not running */
    void (SYSV *entry)(u64, u64);
    u64 arg_init;
    u8 *stack; u64 stack_size;
    char name[32];
    u64 in_arg;                 /* value handed over by whoever resumes this fiber */
    u64 *ret_to;                /* where sceFiberRun wants the fiber's return value */
    u64 thread_rsp;             /* the thread context to go back to */
    int running, started;
    u64 *game_obj;
#ifdef _WIN32
    void *winfiber, *thread_fiber;
#endif
} FiberHost;

/* switch: save callee-saved registers + MXCSR/x87 control word on the current stack, store rsp in *save,
   load 'next' and restore the same layout from it. Declared System V so callers (MS ABI on Windows)
   save rdi/rsi/xmm6-15 themselves. */
__attribute__((naked, noinline)) static SYSV void fctx_switch(u64 *save, u64 next) {
    __asm__ volatile(
        "push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "sub $8, %rsp\n stmxcsr (%rsp)\n fnstcw 4(%rsp)\n"
        "mov %rsp, (%rdi)\n mov %rsi, %rsp\n"
        "ldmxcsr (%rsp)\n fldcw 4(%rsp)\n add $8, %rsp\n"
        "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n");
}
static SYSV void fiber_boot(FiberHost *f);
/* first 'return' of a new fiber lands here with r12 = FiberHost* */
__attribute__((naked, noinline)) static void fiber_tramp(void) {
    __asm__ volatile("mov %r12, %rdi\n and $-16, %rsp\n call *%r13\n ud2\n");
}

#ifdef _WIN32
__declspec(dllimport) void *WINAPI ConvertThreadToFiber(void *);
__declspec(dllimport) void *WINAPI CreateFiberEx(u64, u64, u32, void *, void *);
__declspec(dllimport) void WINAPI SwitchToFiber(void *);
__declspec(dllimport) void WINAPI DeleteFiber(void *);
__declspec(dllimport) int WINAPI IsThreadAFiber(void);
static void *win_current_fiber(void) { void *f; __asm__ volatile("mov %%gs:0x20, %0" : "=r"(f)); return f; }
static WINAPI void win_fiber_proc(void *p);
#endif

static FiberHost *fiber_of(u64 *obj) {
    if (!obj || obj[0] != FIBER_MAGIC) return 0;
    return (FiberHost *)obj[1];
}
/* per-thread: fiber currently running on this thread */
static FiberHost *cur_fiber(void) { PThread *t = cur_thread(); return t ? (FiberHost *)t->fiber : 0; }
static void set_cur_fiber(FiberHost *f) { PThread *t = cur_thread(); if (t) t->fiber = f; }

static SYSV void fiber_boot(FiberHost *f) {
    f->entry(f->arg_init, f->in_arg);
    fatal_from_game("a fiber entry function returned (not allowed on PS4)");
}
#ifdef _WIN32
static void fb_fp_rearm(void) { if (g_fp_trap) { u32 m; __asm__ volatile("stmxcsr %0" : "=m"(m)); m &= ~0x2BFu; __asm__ volatile("ldmxcsr %0" : : "m"(m)); } }   /* t147: --fp-trap also inside fibers (each keeps its own MXCSR) */
static WINAPI void win_fiber_proc(void *p) { fb_fp_rearm(); fiber_boot((FiberHost *)p); }
#endif
/* enter the fiber from a thread / go back from the fiber to that thread */
static void fiber_enter(FiberHost *f) {
#ifdef _WIN32
    if (!IsThreadAFiber()) ConvertThreadToFiber(0);
    f->thread_fiber = win_current_fiber();
    SwitchToFiber(f->winfiber); fb_fp_rearm();
#else
    fctx_switch(&f->thread_rsp, f->rsp);
#endif
}
static void fiber_leave(FiberHost *f) {
#ifdef _WIN32
    SwitchToFiber(f->thread_fiber); fb_fp_rearm();
#else
    fctx_switch(&f->rsp, f->thread_rsp);
#endif
}


static SYSV int fb_init(u64 *obj, const char *name, void *entry, u64 arg_init, void *stack, u64 stack_size, const void *opt, u32 build) {
    TRACE("_sceFiberInitializeImpl", obj, name, stack_size); (void)opt; (void)build;
    if (!obj || !entry || !stack) return (int)SCE_FIBER_ERROR_NULL;
    if (stack_size < 512 || ((u64)stack & 15)) return (int)SCE_FIBER_ERROR_INVALID;
    FiberHost *f = (FiberHost *)calloc(1, sizeof(FiberHost));
    f->entry = (void (SYSV *)(u64, u64))entry; f->arg_init = arg_init; f->stack = (u8 *)stack; f->stack_size = stack_size;
    f->game_obj = obj;
    copy_name(f->name, name ? name : "fiber", sizeof f->name);
    /* initial frame (low -> high): [mxcsr|fcw][r15][r14][r13=fiber_boot][r12=f][rbx][rbp][ret=fiber_tramp] */
    u64 top = ((u64)stack + stack_size) & ~15ull;
    u64 *sp = (u64 *)(top - 16 - 8 * 8);
    u32 mx; __asm__ volatile("stmxcsr %0" : "=m"(mx)); u16 cw; __asm__ volatile("fnstcw %0" : "=m"(cw));
    sp[0] = (u64)mx | ((u64)cw << 32);
    sp[1] = 0; sp[2] = 0; sp[3] = (u64)fiber_boot; sp[4] = (u64)f; sp[5] = 0; sp[6] = 0; sp[7] = (u64)fiber_tramp;
    f->rsp = (u64)sp;
#ifdef _WIN32
    f->winfiber = CreateFiberEx(0x100000, 0x100000, 1 /*FIBER_FLAG_FLOAT_SWITCH; whole 1 MB committed: PS4 code never probes its stack*/, (void *)win_fiber_proc, f);
    if (!f->winfiber) { free(f); return (int)0x80590002; }
#endif
    obj[0] = FIBER_MAGIC; obj[1] = (u64)f;
    g_fibers_created++;
    if (g_fibers_created <= 16) { tr("    fiber \""); tr(f->name); tr("\" stack "); tr_hex(stack_size); tr("\n"); }
    return 0;
}
static SYSV int fb_finalize(u64 *obj) {
    FiberHost *f = fiber_of(obj);
    if (!f) return (int)SCE_FIBER_ERROR_INVALID;
    if (f->running) return (int)SCE_FIBER_ERROR_STATE;
    obj[0] = 0;
#ifdef _WIN32
    if (f->winfiber) DeleteFiber(f->winfiber);
#endif
    free(f);
    return 0;
}
static SYSV int fb_run(u64 *obj, u64 arg, u64 *ret) {
    TRACE("sceFiberRun", obj, arg, ret);
    FiberHost *f = fiber_of(obj);
    if (!f) return (int)SCE_FIBER_ERROR_INVALID;
    if (cur_fiber()) return (int)SCE_FIBER_ERROR_PERMISSION;   /* must be called from a thread, not a fiber */
    if (f->running) return (int)SCE_FIBER_ERROR_STATE;
    f->running = 1; f->in_arg = arg; f->ret_to = ret;
    g_fiber_runs++;
    set_cur_fiber(f);
    fiber_enter(f);
    /* back on the thread: the fiber called sceFiberReturnToThread */
    set_cur_fiber(0);
    f->running = 0;
    return 0;
}
static SYSV int fb_return_to_thread(u64 arg_ret, u64 *arg_run) {
    TRACE("sceFiberReturnToThread", arg_ret, arg_run, 0);
    FiberHost *f = cur_fiber();
    if (!f) return (int)SCE_FIBER_ERROR_PERMISSION;
    if (f->ret_to) *f->ret_to = arg_ret;
    fiber_leave(f);
    /* resumed by a later sceFiberRun (possibly on another thread) */
    if (arg_run) *arg_run = f->in_arg;
    return 0;
}
typedef struct { u64 size; void *entry; u64 arg_init; void *stack; u64 stack_size; char name[32]; u64 margin; } FiberInfo;
static SYSV int fb_get_info(u64 *obj, FiberInfo *info) {
    FiberHost *f = fiber_of(obj);
    if (!f || !info) return (int)SCE_FIBER_ERROR_INVALID;
    if (info->size < sizeof(FiberInfo)) return (int)SCE_FIBER_ERROR_INVALID;
    info->entry = (void *)f->entry; info->arg_init = f->arg_init; info->stack = f->stack; info->stack_size = f->stack_size;
    copy_name(info->name, f->name, 32); info->margin = (u64)-1;
    return 0;
}
static SYSV int fb_get_self(u64 **out) { FiberHost *f = cur_fiber(); if (!f) return (int)SCE_FIBER_ERROR_PERMISSION; if (out) *out = f->game_obj; return 0; }

static const HleEntry g_hle_fiber[] = {
    {"_sceFiberInitializeImpl", (void *)fb_init}, {"sceFiberFinalize", (void *)fb_finalize}, {"sceFiberRun", (void *)fb_run},
    {"sceFiberReturnToThread", (void *)fb_return_to_thread}, {"sceFiberGetInfo", (void *)fb_get_info},
    {"sceFiberGetSelf", (void *)fb_get_self},
    {0, 0}};
