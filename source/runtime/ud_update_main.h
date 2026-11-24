/*
 * ud_update_main.h - w26_update: command line and flow of ud_update.exe, shared by the Windows program (ud_update.c)
 * and the Linux test build (ud_update_posix.c). Besides ud_update_core.h's host functions the host defines:
 *   int up_ask(const UpRel *r, const char *current)   0 install, 1 not now, 2 skip this version (a dialog)
 *   void up_message(const char *title, const char *text, int error)   (a message box; nothing when quiet)
 *   int up_check_deadline(UpCtx *c, UpRel *r, int ms)  up_check() with a hard time limit (2 = timed out / error)
 *   void up_exe_dir(char *out, int n); int up_log_open(const char *path); void up_progress_close(void);
 *
 *   ud_update --check   [--dir BASE] [--current VER] [--skip VER]   the start of UntilDawn.exe (update_check = on)
 *   ud_update --recover [--dir BASE]                                an interrupted install is rolled back
 *   ud_update --restore-previous [--dir BASE]                       the last update undone (files from Cache\update\backup)
 * Test options: --update-source DIR (a local fake release: DIR\latest.json + the assets, in place of GitHub),
 *   --answer install|later|skip (no dialog), --quiet / --no-dialogs or env UD_UPDATE_NO_DIALOGS=1 (nothing on screen), --deadline-ms N,
 *   --sim-cut BYTES (the download breaks off), --sim-fail-replace N (the N-th file cannot be replaced),
 *   --sim-crash-after N (the process ends abruptly after N files: power loss), --allow-dev-test (w28_int: the check may run for a
 *   development build; installing / restoring over a development build is refused regardless).
 * Exit codes: 0 go on with the game (up to date, offline, declined, or a failed update that was rolled back);
 *   2 "skip this version" chosen (Cache\update\result.txt: "skip VER"); 3 files changed: start UntilDawn.exe again;
 *   4 an install failed and could not be fully rolled back (the player was told; do not start the game);
 *   1 bad command line.
 */
#pragma once

static int up_argeq(const char *a, const char *b) { return up_eq(a, b); }
static i64 up_atoi(const char *s) { i64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static UpCtx g_upc;

static void up_write_result(UpCtx *c, const char *line) {
    char p[1200]; up_sub(c, "result.txt", p, sizeof p); up_mkdirs(c->dir);
    char t[128]; UpSb s = {t, 0, sizeof t}; t[0] = 0; sb_put(&s, line); sb_put(&s, "\n"); up_write_file(p, t, (u64)s.n);
}
static void up_fail_msg(UpCtx *c, const char *head, const char *tail) {
    static char t[1400]; UpSb s = {t, 0, sizeof t}; t[0] = 0;
    sb_put(&s, head); sb_put(&s, "\n\n"); sb_put(&s, c->err); sb_put(&s, "\n\n"); sb_put(&s, tail);
    sb_put(&s, "\n\nDetails: Logs\\update.log"); up_message("Until Dawn - update", t, 1);
}

/* w28_int: development builds are never updated. A version that is not a plain release number (e.g. "0.5.0-dev.w28") or a
   program file that carries the development mark counts as a development build: no update check, no install, no
   restore-previous - whatever update_check says. --allow-dev-test (TEST ONLY) lets the check itself run (it is logged), but an
   install or a restore over a development program is still refused. */
static int up_has_dev_word(const char *v) { for (; *v; v++) if (up_lc(v[0]) == 'd' && up_lc(v[1]) == 'e' && up_lc(v[2]) == 'v') return 1; return 0; }
static int up_is_dev_version(const char *v) { UpVer x; return up_has_dev_word(v) || !up_ver_parse(v, &x); }
static int up_exe_is_dev(const UpCtx *c) {   /* 1: UntilDawn.exe in the program folder is a development build */
    /* w38_rel: the mark is kept XOR-encoded here, so that it does not appear as text inside this updater - which is now also
       stored inside the release UntilDawn.exe, where a plain copy would make the release look like a development build. */
    static const unsigned char enc[] = {0x0f,0x1e,0x77,0x18,0x0f,0x13,0x16,0x1e,0x77,0x11,0x13,0x14,0x1e,0x67,0x3e,0x3f,0x2c,0x3f,0x36,0x35,0x2a,0x37,0x3f,0x34,0x2e}; char mark[sizeof enc + 1]; int ml = (int)sizeof enc;
    for (int i = 0; i < ml; i++) mark[i] = (char)(enc[i] ^ 0x5a); mark[ml] = 0;
    char p[1200]; up_join(p, sizeof p, c->base, "UntilDawn.exe"); u64 n = 0; char *t = up_read_file(p, 256ull << 20, &n);
    if (!t) return 0;
    int hit = 0; for (u64 i = 0; i + (u64)ml <= n && !hit; i++) if (t[i] == 'U' && t[i + 1] == 'D') { int k = 0; while (k < ml && t[i + (u64)k] == mark[k]) k++; if (k == ml) hit = 1; }
    up_free(t); return hit;
}
static int up_main(int argc, char **argv) {
    UpCtx *c = &g_upc; up_mset(c, 0, sizeof *c);
    int mode = 0, answer = -1, deadline = 6000, allow_dev = 0;   /* mode 0 check, 1 recover, 2 restore-previous */
    up_cpy(c->current, UD_VERSION, sizeof c->current);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (up_argeq(a, "--check")) mode = 0;
        else if (up_argeq(a, "--recover")) mode = 1;
        else if (up_argeq(a, "--restore-previous")) mode = 2;
        else if (up_argeq(a, "--quiet") || up_argeq(a, "--no-dialogs")) g_up_quiet = 1;   /* tests: no dialog, message box or progress window (also env UD_UPDATE_NO_DIALOGS=1) */
        else if (v && up_argeq(a, "--dir")) { up_cpy(c->base, v, sizeof c->base); i++; }
        else if (v && up_argeq(a, "--current")) { up_cpy(c->current, v, sizeof c->current); i++; }
        else if (v && up_argeq(a, "--skip")) { up_cpy(c->skip, v, sizeof c->skip); i++; }
        else if (v && up_argeq(a, "--update-source")) { up_cpy(c->source, v, sizeof c->source); i++; }
        else if (v && up_argeq(a, "--answer")) { answer = up_eq(v, "install") ? 0 : up_eq(v, "later") ? 1 : up_eq(v, "skip") ? 2 : -2; i++; if (answer == -2) return 1; }
        else if (v && up_argeq(a, "--deadline-ms")) { deadline = (int)up_atoi(v); i++; }
        else if (up_argeq(a, "--allow-dev-test")) allow_dev = 1;   /* w28_int, TEST ONLY: the check may run for a development build (never an install) */
        else if (v && up_argeq(a, "--sim-cut")) { c->sim_cut = up_atoi(v); i++; }
        else if (v && up_argeq(a, "--sim-fail-replace")) { c->sim_fail_replace = up_atoi(v); i++; }
        else if (v && up_argeq(a, "--sim-crash-after")) { c->sim_crash_after = up_atoi(v); i++; }
        else return 1;
    }
    if (!c->base[0]) up_exe_dir(c->base, sizeof c->base);
    up_ctx_dirs(c);
    { char lg[1200]; up_join(lg, sizeof lg, c->base, "Logs"); up_mkdirs(lg); up_join(lg, sizeof lg, c->base, "Logs/update.log"); up_log_open(lg); }
    up_log(mode == 0 ? "--- update check, installed version " : mode == 1 ? "--- recovery check, installed version " : "--- restore previous version, installed version ", c->current, c->source[0] ? " (TEST: local update source)" : 0);

    /* an install that was interrupted (power loss, crash) is always rolled back first */
    int rec = up_recover(c);
    if (rec < 0) { up_fail_msg(c, "An earlier update was interrupted and could not be undone completely.", "Please download the current release from GitHub and copy its files over the program folder (your Game, savedata and Config folders are not part of it)."); return 4; }
    if (rec > 0) { up_message("Until Dawn - update", "An earlier update was interrupted. The previous version of the program has been restored; your game files, saves and settings were not touched.", 0); return 3; }
    if (mode == 1) return 0;
    /* w28_int: development builds are never checked, replaced or restored */
    int dev_ver = up_is_dev_version(c->current), dev_exe = up_exe_is_dev(c), dev = dev_ver || dev_exe;
    if (dev) up_log("development build (", dev_ver ? "version " : "program file mark", dev_ver ? c->current : ")");
    if (dev && mode == 2) { up_log("development build: --restore-previous refused (a development program is never replaced)", 0, 0); return 0; }
    if (dev && !allow_dev) { up_log("development build: no update check (update_check is ignored; development builds are never updated)", 0, 0); return 0; }
    if (dev) { up_log("TEST --allow-dev-test: the check runs, but nothing will be installed", 0, 0);
               int k = 0; while (c->current[k] == '.' || (c->current[k] >= '0' && c->current[k] <= '9')) k++; c->current[k] = 0;   /* (compared by its number part) */
               if (k && c->current[k - 1] == '.') c->current[k - 1] = 0; }
    if (mode == 2) {
        int r = up_restore_previous(c);
        if (r) { up_fail_msg(c, "The previous version could not be restored.", ""); return r == 2 ? 4 : 0; }
        up_message("Until Dawn - update", "The previous version of the program has been restored.", 0); return 3;
    }

    UpRel r; int ck = up_check_deadline(c, &r, deadline);
    if (ck) return 0;   /* up to date, skipped, offline, GitHub not reachable in time: the game starts, nothing said */
    if (dev) { up_log("development build: version ", r.ver, " is NOT offered or installed (development builds are never replaced)"); return 0; }   /* w28_int */
    int a = answer >= 0 ? answer : up_ask(&r, c->current);
    if (a == 1) { up_log("the player chose: not now", 0, 0); return 0; }
    if (a == 2) { char t[64]; UpSb s = {t, 0, sizeof t}; t[0] = 0; sb_put(&s, "skip "); sb_put(&s, r.ver); up_write_result(c, t); up_log("the player chose: skip version ", r.ver, 0); return 2; }
    up_log("the player chose: install ", r.ver, 0);

    u8 *zip = 0; u64 zn = 0;
    if (up_download(c, &r, &zip, &zn)) { up_progress_close(); up_fail_msg(c, "The update could not be downloaded or did not pass the checksum test. Nothing was changed.", "Until Dawn starts with the current version; the update is offered again on the next start."); return 0; }
    int st = up_stage(c, zip, zn); up_free(zip);
    if (st) { up_progress_close(); up_fail_msg(c, "The update package could not be unpacked. Nothing was changed.", "Until Dawn starts with the current version."); return 0; }
    int ap = up_apply(c, r.ver);
    up_progress_close();
    if (ap == 1) { up_fail_msg(c, "The update could not be installed. The previous version has been restored; nothing else was changed.", "Until Dawn starts with the current version."); return 0; }
    if (ap == 2) { up_fail_msg(c, "The update could not be installed and the previous version could not be restored completely.", "Please download the current release from GitHub and copy its files over the program folder (your Game, savedata and Config folders are not part of it)."); return 4; }
    { char dl[1200]; up_sub(c, "download", dl, sizeof dl); up_rmtree(dl); }   /* the verified zip is not needed any more */
    return 3;
}
