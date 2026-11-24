/* host test (w33_lang): cancel during install/repair through the shared ud_langscan functions the GUI uses.
   usage: langscan_cancel_test <Languages test dir> <CUSA00194 update dir> <CUSA00194 game dir> */
#define UD_LANGSCAN_NO_MAIN
#include "ud_langscan.c"
static u64 g_cancel_at;
static void prog(u64 d, u64 t) { (void)t; if (g_cancel_at && d >= g_cancel_at) g_lp_cancel = 1; }
static int state(const char *base, const char *code) { char d[1100], why[160]; int nb; path_join(d, base, code); const LpPack *P = lp_pack_by_code(code);
    if (!plat_is_dir(d)) return 0; return lp_pack_check(d, P, 1, why, sizeof why, &nb) ? 2 : 1; }
static int leftover(const char *base, const char *code) { char d[1100], t[32]; Sb s = {t, 0, sizeof t}; sb_s(&s, ".new_"); sb_s(&s, code); path_join(d, base, t); return plat_is_dir(d); }
int e3_main(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *base = argv[1]; int fails = 0;
    for (int i = 2; i < 4; i++) { mem_copy(g_src[g_nsrc].dir, argv[i], (u64)str_len(argv[i]) + 1); lp_read_src(&g_src[g_nsrc]); g_nsrc++; }
    lp_resolve(0, 0); plat_mkdir(base); g_lp_progress = prog;
    const LpPack *nl = lp_pack_by_code("nl"), *tr = lp_pack_by_code("tr");
#define CHECK(c, msg) do { int ok_ = (c); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", msg); fails += !ok_; } while (0)
    g_cancel_at = 0; g_lp_cancel = 0; g_lp_done = 0; g_lp_total = lp_pack_bytes(nl);
    CHECK(lp_install_one(base, nl, 1) == 1 && state(base, "nl") == 2, "install nl (baseline) complete");
    g_cancel_at = 200000; g_lp_cancel = 0; g_lp_done = 0;
    { char d[1100], p[1100]; path_join(d, base, "nl"); path_join(p, d, "localcachepink/lumps"); }   /* force a real copy: repair copies every file anew */
    CHECK(lp_install_one(base, nl, 1) == 0, "repair nl cancelled after 200 KB returns failure");
    CHECK(state(base, "nl") == 2, "previous nl pack still complete after the cancel");
    CHECK(!leftover(base, "nl"), "no .new_nl left behind");
    g_cancel_at = 100000; g_lp_cancel = 0; g_lp_done = 0;
    CHECK(lp_install_one(base, tr, 1) == 0, "fresh tr install cancelled returns failure");
    CHECK(state(base, "tr") == 0 && !leftover(base, "tr"), "no tr pack and no .new_tr after the cancel");
    g_cancel_at = 0; g_lp_cancel = 0; g_lp_done = 0;
    CHECK(lp_install_one(base, tr, 1) == 1 && state(base, "tr") == 2, "tr install without cancel complete");
    CHECK(state(base, "nl") == 2, "nl untouched by the tr operations");
    printf("%s (%d failed)\n", fails ? "SOME TESTS FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
