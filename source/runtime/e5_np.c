/* e5_np.c - trophies (persisted to savedata/trophies.bin) and PSN state ("signed out").
 * Included by e3_hle.c after e5_savedata.c. */

#define NP_ERROR_SIGNED_OUT 0x80550006
static u8 g_trophy_unlocked[128];
static int g_trophy_loaded;

static void trophy_path(char *out) { path_join(out, g_save_root, "trophies.bin"); }
static void trophy_load(void) {
    if (g_trophy_loaded) return;
    g_trophy_loaded = 1;
    char p[1100]; trophy_path(p);
    void *f = hf_open_rb(p);
    if (f) { hf_read(f, g_trophy_unlocked, sizeof g_trophy_unlocked); hf_close(f); }
}
static void trophy_save(void) {
    char p[1100]; trophy_path(p);
    plat_mkdir(g_save_root);
    void *f = hf_open_rw(p, 1, 1);
    if (f) { hf_write(f, g_trophy_unlocked, sizeof g_trophy_unlocked); hf_close(f); }
}

static SYSV int np_trophy_create_ctx(int *ctx, int user, u32 label, u64 opt) {
    TRACE("sceNpTrophyCreateContext", ctx, user, label); (void)opt;
    if (ctx) *ctx = 1;
    return 0;
}
static SYSV int np_trophy_create_handle(int *h) { TRACE("sceNpTrophyCreateHandle", h, 0, 0); if (h) *h = 1; return 0; }
static SYSV int np_trophy_register(int ctx, int h, u64 opt) { TRACE("sceNpTrophyRegisterContext", ctx, h, opt); return 0; }
static SYSV int np_trophy_ok(void) { return 0; }
/* SceNpTrophyFlagArray is 128 bits (4 x u32) */
static SYSV int np_trophy_state(int ctx, int h, u32 *flags, u32 *count) {
    TRACE("sceNpTrophyGetTrophyUnlockState", ctx, h, flags);
    pm_lock(&g_np_lock); trophy_load();
    if (flags) { for (int i = 0; i < 4; i++) flags[i] = 0; for (int t = 0; t < 128; t++) if (g_trophy_unlocked[t]) flags[t >> 5] |= 1u << (t & 31); }
    pm_unlock(&g_np_lock);
    if (count) *count = 0;
    return 0;
}
static SYSV int np_trophy_unlock(int ctx, int h, int id, int *platinum) {
    TRACE("sceNpTrophyUnlockTrophy", ctx, h, id);
    if (platinum) *platinum = -1;
    if (id < 0 || id >= 128) return (int)0x80551604;   /* invalid trophy id */
    pm_lock(&g_np_lock); trophy_load();
    int already = g_trophy_unlocked[id];
    if (!already) { g_trophy_unlocked[id] = 1; trophy_save(); }
    pm_unlock(&g_np_lock);
    if (already) return (int)0x8055160F;                 /* already unlocked */
    say("\n*** TROPHY UNLOCKED: id "); say_dec(id); say(" ***\n");
    return 0;
}

/* PSN: report "signed out" consistently */
static SYSV int np_get_state(int user, int *state) { TRACE("sceNpGetState", user, state, 0); if (state) *state = 1; return 0; }
static SYSV int np_signed_out(void) { return (int)NP_ERROR_SIGNED_OUT; }
static int g_np_cb_id;
static SYSV int np_register_state_cb(void *cb, void *arg) { TRACE("sceNpRegisterStateCallback", cb, arg, 0); return ++g_np_cb_id; }
static SYSV int np_ok(void) { return 0; }

static const HleEntry g_hle_np[] = {
    {"sceNpTrophyCreateContext", (void *)np_trophy_create_ctx}, {"sceNpTrophyCreateHandle", (void *)np_trophy_create_handle},
    {"sceNpTrophyRegisterContext", (void *)np_trophy_register}, {"sceNpTrophyDestroyContext", (void *)np_trophy_ok},
    {"sceNpTrophyDestroyHandle", (void *)np_trophy_ok}, {"sceNpTrophyGetTrophyUnlockState", (void *)np_trophy_state},
    {"sceNpTrophyUnlockTrophy", (void *)np_trophy_unlock},
    {"sceNpGetState", (void *)np_get_state}, {"sceNpCheckNpAvailability", (void *)np_signed_out},
    {"sceNpGetNpId", (void *)np_signed_out}, {"sceNpGetAccountId", (void *)np_signed_out},
    {"sceNpGetAccountCountry", (void *)np_signed_out}, {"sceNpGetAccountLanguage", (void *)np_signed_out},
    {"sceNpGetParentalControlInfo", (void *)np_signed_out},
    {"sceNpRegisterStateCallback", (void *)np_register_state_cb}, {"sceNpUnregisterStateCallback", (void *)np_ok},
    {"sceNpCheckCallback", (void *)np_ok},
    {0, 0}};
