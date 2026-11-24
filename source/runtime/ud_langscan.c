/* ud_langscan.c - language packs for the Until Dawn native port (w33_lang; optional, not in a release yet).
 *
 * Players may add the languages of ANOTHER regional edition they own, once: the verified language files are copied
 * into Languages\<code>\ next to UntilDawn.exe, after which the second edition is no longer needed. The tool holds no
 * game data - only our metadata (ud_langpack_data.h / ud_langpack.h: names, sizes, fingerprints, language slots measured
 * on CUSA00194 01.03 and CUSA02636 01.03). It only reads edition folders and never writes into them or the game folder.
 *
 *   ud_langscan <edition folder> [<update folder>]                 report (as before)
 *   ud_langscan --install fr,de,it,nl,tr|all --from <update folder> <game folder> [--languages-dir D]
 *   ud_langscan --repair <codes|all> [--from <update folder> <game folder>]   (re-uses intact files; the rest from --from)
 *   ud_langscan --remove <codes|all>                                      ud_langscan --list [--verify size|quick|full]
 *   ud_langscan --inventory <folder> [<folder> ...] [--out FILE] [--json] [--hashes-only] [--quick]
 *   ud_langscan --install-experimental <update folder> <game folder> --i-accept-unverified-files
 *   ud_langscan --remove-experimental <EDITION_VERSION>
 * Status words: VERIFIED (measured files + in-game test), EXPERIMENTAL (an edition the game code knows, files not
 * verified by us), UNSUPPORTED (unknown title). Exit code 0 = done, 1 = something incomplete / failed, 2 = refused.
 */
#ifdef UD_LANGSCAN_HOST
#include "tests/langscan_host.h"   /* Linux host build for tests (tests/build_langscan_host.sh) */
#else
#define E3_NO_MAIN
#include "e3_loader.c"
__declspec(dllimport) int __attribute__((ms_abi)) GetDiskFreeSpaceExA(const char *, u64 *, u64 *, u64 *);
static u64 plat_free_bytes(const char *dir) { u64 a = 0, t = 0, f = 0; return GetDiskFreeSpaceExA(dir, &a, &t, &f) ? a : (u64)-1; }
#endif
#include "ud_langpack.h"

#define LP_NSLOT ((int)(sizeof k_lp_slot / sizeof k_lp_slot[0]))
#define LP_NFILE ((int)(sizeof k_lp_files / sizeof k_lp_files[0]))
static const char *const k_lp_sku[2] = {"CUSA00194", "CUSA02636"};
static const char *lp_slot_desc(int s) {
    static const char *const t[] = {"French text", "French speech", "German text", "German speech", "Italian text", "Italian speech",
        "Turkish text (slot X)", "Dutch text (slot Z)", "Spanish text", "Spanish speech", "Portuguese text", "Portuguese speech",
        "Russian text", "Russian speech", "Polish text", "Polish speech", "Nordic text slot Y1", "Nordic text slot Y2", "Nordic text slot Y3", "Nordic text slot Y4"};
    return t[s];
}
static const struct { const char *sha, *what; int sku, v103; } k_lp_eboot[] = {
    {"ca291314cdd5308694eb9243f1ba098e97817087e5356d78d94cf8854abebd26", "CUSA02636 01.03 (primary edition)", 1, 1},
    {"fdd1e3247e1f931e277336099062aa240c362b48ae0a7d68e08390dfbd743789", "CUSA02636 01.00", 1, 0},
    {"8e833bb6b1f782c6949b9a87000b12b460b774f73db885692d44a92a8b63be60", "CUSA00194 01.03", 0, 1},
    {"5f0a12dc4bdba2700a5c22e047488382778827f227f390340c918aacffa93da2", "CUSA00194 01.00", 0, 0},
};
/* the game code's own region table (measured in eboot 01.03) */
static const char *lp_region_of(const char *id) {
    if (str_eq(id, "CUSA00194")) return "Europe1 (en fr it de tr nl)";
    if (str_eq(id, "CUSA02636")) return "Europe2 (en es pl pt ru sv no fi da)";
    if (str_eq(id, "CUSA01913")) return "UK (en)";
    if (str_eq(id, "CUSA00359")) return "Americas: US, Canada, Latin America incl. Brazil (en fr es-LA pt-BR)";
    if (str_eq(id, "CUSA00902")) return "Japan (ja)";
    if (str_eq(id, "CUSA01423")) return "Asia (en ko zh-TW zh-CN)";
    if (str_eq(id, "CUSA02915")) return "region 9 (all 20 languages in the code)";
    return 0;
}
/* languages each edition's region offers in the game code (measured, eboot 01.03 region table) - "" if unknown */
static const char *lp_region_langs(const char *id) {
    if (str_eq(id, "CUSA00194")) return "en fr it de tr nl";
    if (str_eq(id, "CUSA02636")) return "en es pl pt ru sv no fi da";
    if (str_eq(id, "CUSA01913")) return "en";
    if (str_eq(id, "CUSA00359")) return "en fr es-LA pt-BR";
    if (str_eq(id, "CUSA00902")) return "ja";
    if (str_eq(id, "CUSA01423")) return "en ko zh-TW zh-CN";
    if (str_eq(id, "CUSA02915")) return "en fr es de it nl pt zh-TW ko ru pl da fi no sv ja es-LA pt-BR tr zh-CN";
    return "";
}
static const char *lp_label(const char *id) {
    if (str_eq(id, "CUSA00194") || str_eq(id, "CUSA02636")) return "VERIFIED";
    return lp_region_of(id) ? "EXPERIMENTAL" : "UNSUPPORTED";
}

static void *g_lr;
/* shared with the GUI (ud_langmgr.c -> UDLanguageImporter.exe): text sink, progress and cancel. The CLI leaves them unset. */
static void (*g_lr_hook)(const char *);
static void (*g_lp_progress)(u64 done, u64 total);
static volatile int g_lp_cancel;
static u64 g_lp_done, g_lp_total;
static void lr(const char *s) { if (g_lr_hook) g_lr_hook(s); else plat_write(0, s); if (g_lr) plat_write(g_lr, s); }
static void lr_d(i64 v) { char b[24]; fmt_dec(b, v); lr(b); }
static void lr_mb(u64 v) { lr_d((i64)((v + 500000) / 1000000)); lr(" MB"); }
static int lp_eq_ci(const char *a, const char *b) { while (*a && *b) { if ((*a | 0x20) != (*b | 0x20)) return 0; a++; b++; } return !*a && !*b; }

/* fingerprint of our metadata: SHA-256 over the decimal size + whole file (<= 1 MiB) or first/middle/last 256 KiB */
static int lp_fingerprint(const char *path, char out[65]) {
    void *f = hf_open_rb(path); if (!f) return 0;
    i64 sz = hf_seek(f, 0, 2); if (sz < 0) { hf_close(f); return 0; }
    static u8 buf[1 << 20]; Sha256 s; sha_init(&s);
    char d[24]; fmt_dec(d, sz); sha_add(&s, (const u8 *)d, (u64)str_len(d));
    const u64 M = 262144;
    if ((u64)sz <= (1u << 20)) { hf_seek(f, 0, 0); u64 got = 0, n; while (got < (u64)sz && (n = hf_read(f, buf + got, (u64)sz - got)) > 0) got += n; sha_add(&s, buf, got); }
    else { u64 offs[3] = {0, (u64)sz / 2, (u64)sz - M};
        for (int k = 0; k < 3; k++) { hf_seek(f, (i64)offs[k], 0); u64 got = 0, n; while (got < M && (n = hf_read(f, buf + got, M - got)) > 0) got += n; sha_add(&s, buf, got); } }
    hf_close(f); sha_hex(&s, out); return 1;
}
static void lp_norm(char *o, const char *p, int n) {
    int k = 0; for (; p[k] && k < n - 1; k++) { char c = p[k]; if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 32; o[k] = c; }
    while (k > 1 && o[k - 1] == '/') k--; o[k] = 0;
}
static int lp_inside(const char *a, const char *b) { char x[1100], y[1100]; lp_norm(x, a, sizeof x); lp_norm(y, b, sizeof y); int n = str_len(y); if (!str_starts(x, y)) return 0; return x[n] == 0 || x[n] == '/'; }

typedef struct { char dir[1024]; char title[16], appver[16], cat[8], content[64], tid[32], esha[65], name[96]; int sku, eboot, ok; } LpSrc;
static LpSrc g_src[8]; static int g_nsrc;
static int lp_read_src(LpSrc *s) {
    char p[1200]; u64 n = 0; path_join(p, s->dir, "sce_sys/param.sfo");
    u8 *d = (u8 *)plat_read_file(p, &n);
    s->sku = -1; s->eboot = -1; s->esha[0] = 0;
    if (d) { sfo_get(d, n, "TITLE_ID", s->title, sizeof s->title); sfo_get(d, n, "APP_VER", s->appver, sizeof s->appver);
             sfo_get(d, n, "CATEGORY", s->cat, sizeof s->cat); sfo_get(d, n, "CONTENT_ID", s->content, sizeof s->content); sfo_get(d, n, "TITLE", s->name, sizeof s->name); }
    path_join(p, s->dir, "title-id.txt"); { u64 m = 0; char *t = (char *)plat_read_file(p, &m); int k = 0; if (t) for (; k < (int)m && k < 31 && t[k] >= ' '; k++) s->tid[k] = t[k]; s->tid[k] = 0; }
    for (int i = 0; i < 2; i++) if (str_eq(s->title, k_lp_sku[i])) s->sku = i;
    path_join(p, s->dir, "eboot.bin");
    if (sha_file(p, s->esha)) { s->eboot = 99; for (int i = 0; i < (int)(sizeof k_lp_eboot / sizeof k_lp_eboot[0]); i++) if (str_eq(s->esha, k_lp_eboot[i].sha)) s->eboot = i; }
    s->ok = d != 0; return s->ok;
}
/* w38_rel: the player's OWN game folder (UntilDawn.exe's settings game_dir, else Game\ next to it). Languages whose files are
   already there are part of the player's edition and never imported again - whichever European edition it is. */
static char g_primary_dir[1100], g_primary_title[16];
static void lp_find_primary(const char *base) {
    char ex[1100], p[1200]; u64 n = 0; g_primary_dir[0] = g_primary_title[0] = 0;
    (void)base; plat_exe_dir(ex, sizeof ex);   /* the tools sit next to UntilDawn.exe */
    path_join(p, ex, "Config/settings.ini"); char *t = (char *)plat_read_file(p, &n);
    for (u64 i = 0; t && i < n; ) {   /* game_dir = <path> */
        const char *ln = t + i; u64 e = i; while (e < n && t[e] != '\n') e++; i = e + 1;
        while (*ln == ' ' || *ln == '\t') ln++; if (!(ln[0] == 'g' && ln[1] == 'a' && ln[2] == 'm' && ln[3] == 'e' && ln[4] == '_' && ln[5] == 'd' && ln[6] == 'i' && ln[7] == 'r')) continue;
        const char *v = ln + 8; while (*v == ' ' || *v == '\t') v++; if (*v != '=') continue; v++; while (*v == ' ' || *v == '\t' || *v == '"') v++;
        int k = 0; while (v + k < t + e && v[k] != '\r' && v[k] != '"' && v[k] != ';' && k < 1000) k++; while (k && (v[k - 1] == ' ' || v[k - 1] == '\t')) k--;
        if (k) { mem_copy(g_primary_dir, v, (u64)k); g_primary_dir[k] = 0; }
    }
    if (!g_primary_dir[0]) path_join(g_primary_dir, ex, "Game");
    path_join(p, g_primary_dir, "sce_sys/param.sfo"); u8 *d = (u8 *)plat_read_file(p, &n);
    if (d) sfo_get(d, n, "TITLE_ID", g_primary_title, sizeof g_primary_title); else g_primary_dir[0] = 0;
}
static int lp_in_primary(const LpPack *P) { return g_primary_dir[0] && lp_pack_in_game(g_primary_dir, P); }
/* plain-language guidance for a folder that is not usable as it is (shared by the CLI report and the GUI) */
typedef struct { Sb *s; int n; } LpSub;
static void lp_sub_cb(const char *name, void *ctx) { LpSub *c = (LpSub *)ctx; char p[1200]; (void)p; c->n++;
    if (c->n <= 6) { sb_s(c->s, c->n > 1 ? ", " : ""); sb_s(c->s, name); } }
typedef struct { const char *dir; Sb *s; int n; } LpSub2;
static void lp_sub2_cb(const char *name, void *ctx) { LpSub2 *c = (LpSub2 *)ctx; char p[1200], q[1100]; path_join(q, c->dir, name); path_join(p, q, "sce_sys/param.sfo");
    if (plat_file_size(p) < 0) return; c->n++; if (c->n <= 8) { sb_s(c->s, c->n > 1 ? ", " : ""); sb_s(c->s, name); } }
static void lp_folder_hint(const LpSrc *x, Sb *s) {
    if (!x->ok) {
        sb_s(s, x->dir); sb_s(s, ": no sce_sys\\param.sfo found - this is not an extracted PS4 game or update folder.");
        LpSub2 c = {x->dir, s, 0}; Sb *t = s; char tmp[600]; Sb u = {tmp, 0, sizeof tmp}; c.s = &u; plat_list_dirs(x->dir, lp_sub2_cb, &c);
        if (c.n) { sb_s(t, " It contains these PS4 folders - pick one of them instead: "); sb_s(t, tmp); sb_s(t, "."); }
        else sb_s(t, " Pick the folder that contains sce_sys and eboot.bin, e.g. ...\\CUSA00194 (not its parent folder).");
        return;
    }
    sb_s(s, x->dir); sb_s(s, ": "); sb_s(s, x->title); sb_s(s, " version "); sb_s(s, x->appver); sb_s(s, str_eq(x->cat, "gp") ? " (update)" : " (game)");
    if (x->name[0]) { sb_s(s, " \""); sb_s(s, x->name); sb_s(s, "\""); }
    const char *lab = lp_label(x->title);
    if (str_eq(lab, "UNSUPPORTED")) sb_s(s, " - not an Until Dawn edition this game version knows; nothing can be imported from it.");
    else if (str_eq(lab, "EXPERIMENTAL")) sb_s(s, " - an Until Dawn edition we have not verified yet: please make an Inventory report (ud_langscan --inventory) and attach it to a GitHub issue. Experimental import only at your own risk.");
    else if (g_primary_title[0] && str_eq(x->title, g_primary_title)) sb_s(s, " - this is your own game edition: its languages are already in the game; nothing to import.");
    else sb_s(s, " - VERIFIED edition.");
}
static void lp_print_src(int i) {
    LpSrc *s = &g_src[i];
    lr("folder "); lr_d(i + 1); lr(": "); lr(s->dir); lr("\n");
    if (!s->ok) { lr("  no sce_sys\\param.sfo - not an extracted PS4 game or update folder\n"); return; }
    lr("  title "); lr(s->title); lr(", version "); lr(s->appver); lr(str_eq(s->cat, "gp") ? " (update)" : str_eq(s->cat, "gd") ? " (game)" : ""); lr(", content "); lr(s->content); lr("\n");
    lr("  edition status: "); lr(lp_label(s->title)); if (s->sku >= 0 && !str_eq(s->appver, "01.03")) lr(" (version "), lr(s->appver), lr(": its 01.03 update folder is needed too)"); { const char *r = lp_region_of(s->title); lr(r ? " - region in the game code: " : " - not in the game's region table"); if (r) lr(r); } lr("\n");
    if (s->tid[0]) { lr("  title-id.txt: "); lr(s->tid); lr("\n"); }
    if (s->eboot == 99) lr("  eboot.bin: not a measured executable\n"); else if (s->eboot >= 0) { lr("  eboot.bin: "); lr(k_lp_eboot[s->eboot].what); lr("\n"); }
}
enum { LP_MISSING, LP_OK, LP_OLD, LP_BAD };
static u8 g_st[LP_NFILE]; static u8 g_from[LP_NFILE];
static int g_slot_ok[32], g_slot_all[32];
/* resolve and check every language file of edition sku (update folders win); returns the number OK */
static int lp_resolve(int sku, int nofp) {
    char p[1200], rel[256], fp[65]; int n_ok = 0;
    for (int s = 0; s < 32; s++) g_slot_ok[s] = g_slot_all[s] = 0;
    for (int f = 0; f < LP_NFILE; f++) {
        const LpFile *L = &k_lp_files[f]; if (L->sku != sku) continue;
        lp_rel(rel, L->guid); g_st[f] = LP_MISSING;
        for (int pass = 0; pass < 2 && g_st[f] != LP_OK; pass++)
            for (int i = 0; i < g_nsrc; i++) {
                LpSrc *s = &g_src[i]; if (s->sku != sku || (pass == 0) != str_eq(s->cat, "gp")) continue;
                path_join(p, s->dir, rel); i64 sz = plat_file_size(p); if (sz < 0) continue;
                u8 st = (u64)sz == L->size ? LP_OK : (L->size_v100 && (u64)sz == L->size_v100) ? LP_OLD : LP_BAD;
                if (st == LP_OK && !nofp && (!lp_fingerprint(p, fp) || !str_eq(fp, L->fp))) st = LP_BAD;
                if (g_st[f] == LP_MISSING || st == LP_OK) { g_st[f] = st; g_from[f] = (u8)i; }
                if (st == LP_OK) break;
            }
        g_slot_all[L->slot]++; if (g_st[f] == LP_OK) { g_slot_ok[L->slot]++; n_ok++; }
    }
    return n_ok;
}
static int lp_pack_src_ok(const LpPack *P) { for (int k = 0; k < P->nslot; k++) if (!g_slot_all[P->slot[k]] || g_slot_ok[P->slot[k]] != g_slot_all[P->slot[k]]) return 0; return 1; }

/* ---- file helpers ---- */
static int lp_copy_hash(const char *from, const char *to, u64 *bytes, char full[65]) {   /* copy + SHA-256 of what was written */
    static u8 buf[1 << 20]; void *a = hf_open_rb(from), *b = a ? hf_open_rw(to, 1, 1) : 0; u64 n, tot = 0; int ok = a && b; Sha256 s; sha_init(&s);
    if (ok) while ((n = hf_read(a, buf, sizeof buf)) > 0) { if (g_lp_cancel || hf_write(b, buf, n) != n) { ok = 0; break; } sha_add(&s, buf, n); tot += n;
        g_lp_done += n; if (g_lp_progress) g_lp_progress(g_lp_done, g_lp_total); }
    if (a) hf_close(a);
    if (b) hf_close(b);
    sha_hex(&s, full); *bytes = tot; return ok;
}
static void lp_mkdirs(const char *dir) {   /* dir, dir\localcachepink, dir\localcachepink\lumps */
    char p[1100]; plat_mkdir(dir); path_join(p, dir, "localcachepink"); plat_mkdir(p); path_join(p, p, "lumps"); plat_mkdir(p);
}
typedef struct { const char *dir; int n; } LpDelCtx;
static void lp_del_cb(const char *name, void *ctx) { LpDelCtx *c = (LpDelCtx *)ctx; char p[1100]; path_join(p, c->dir, name); if (!plat_unlink(p)) c->n++; }
/* delete a pack-shaped folder that WE wrote: its lumps\*.core, its manifest, then the empty folders. Other files stay. */
static int lp_delete_pack_dir(const char *dir) {
    char p[1100], q[1100]; LpDelCtx c = {p, 0};
    path_join(p, dir, "localcachepink/lumps");
    /* simple pass: delete every *.core listed in the manifest, then every *.core that remains (ours by construction) */
    LpManifest m; if (lp_manifest_read(dir, &m)) for (int i = 0; i < m.nf; i++) { path_join(q, dir, m.f[i].rel); if (!plat_unlink(q)) c.n++; }
    plat_list_files(p, lp_del_cb, &c);
    plat_rmdir(p); path_join(q, dir, "localcachepink"); plat_rmdir(q);
    path_join(q, dir, "language.manifest"); plat_unlink(q);
    plat_rmdir(dir);
    return c.n;
}
static void lp_cleanup(const char *base, const char *code) {   /* left-overs of an interrupted install */
    char nw[1100], old[1100], cur[1100], t[32]; Sb s = {t, 0, sizeof t};
    sb_s(&s, ".new_"); sb_s(&s, code); path_join(nw, base, t); s.n = 0; sb_s(&s, ".old_"); sb_s(&s, code); path_join(old, base, t); path_join(cur, base, code);
    if (plat_is_dir(nw)) { lp_delete_pack_dir(nw); lr("  removed an unfinished install of "); lr(code); lr("\n"); }
    if (plat_is_dir(old)) { if (!plat_is_dir(cur)) { plat_replace_file(old, cur); lr("  restored the previous "); lr(code); lr(" pack after an interrupted update\n"); } else lp_delete_pack_dir(old); }
}

/* folder renames can fail for a moment while Explorer or an antivirus scanner still holds the folder: retry ~2 s */
static void lp_sleep_ms(int ms) {
#ifdef UD_LANGSCAN_HOST
    usleep((unsigned)ms * 1000);
#else
    Sleep((u32)ms);
#endif
}
static int lp_rename_retry(const char *a, const char *b) { for (int k = 0; k < 20; k++) { if (plat_replace_file(a, b)) return 1; lp_sleep_ms(100); } return 0; }
/* ---- install / repair one verified pack ---- */
static int lp_install_one(const char *base, const LpPack *P, int have_src) {
    char cur[1100], nw[1100], old[1100], t[32], rel[256], from[1200], to[1200], fp[65], full[65], quick[65];
    Sb s = {t, 0, sizeof t}; sb_s(&s, ".new_"); sb_s(&s, P->code); path_join(nw, base, t);
    s.n = 0; sb_s(&s, ".old_"); sb_s(&s, P->code); path_join(old, base, t); path_join(cur, base, P->code);
    lp_cleanup(base, P->code);
    lr("\n"); lr(P->name); lr(" ("); lr(P->code); lr(P->speech ? ", text + voices" : ", text only - voices stay English"); lr("): ");
    if (lp_in_primary(P)) { lr("already part of your game ("); lr(g_primary_title); lr(") - nothing to import\n"); return 1; }
    if (have_src && !lp_pack_src_ok(P)) { lr("NOT installed - the edition's files for it are incomplete or not the verified 01.03 files:\n");
        for (int k = 0; k < P->nslot; k++) { lr("    "); lr(lp_slot_desc(P->slot[k])); lr(" "); lr_d(g_slot_ok[P->slot[k]]); lr(" / "); lr_d(g_slot_all[P->slot[k]]); lr("\n"); } return 0; }
    /* plan: re-use an intact file of the current pack, else take it from the edition */
    int nf = 0, reuse = 0; u64 need = 0;
    for (int f = 0; f < LP_NFILE; f++) { const LpFile *L = &k_lp_files[f]; if (!lp_file_in_pack(L, P)) continue; nf++;
        lp_rel(rel, L->guid); path_join(from, cur, rel);
        if (plat_file_size(from) == (i64)L->size && lp_fingerprint(from, fp) && str_eq(fp, L->fp)) reuse++;
        else if (!have_src || g_st[f] != LP_OK) { lr("NOT repaired - a file is damaged or missing and no edition folder (--from) supplies it\n"); return 0; }
        need += L->size; }
    u64 freeb = plat_free_bytes(base);
    if (freeb != (u64)-1 && freeb < need + need / 20) { lr("NOT installed - not enough free space: needs "); lr_mb(need + need / 20); lr(", free "); lr_mb(freeb); lr("\n"); return 0; }
    if (g_lp_progress) g_lp_progress(g_lp_done, g_lp_total);
    lp_mkdirs(nw);
    char mf[1100]; path_join(mf, nw, "language.manifest.tmp"); void *m = plat_open_write(mf);
    if (!m) { lr("NOT installed - cannot write into "); lr(base); lr("\n"); return 0; }
    plat_write(m, "; Until Dawn native port - language pack written by ud_langscan from the player's own files (our metadata, no game data)\r\nformat = 1\r\nlanguage = ");
    plat_write(m, P->code); plat_write(m, "\r\nedition = "); plat_write(m, k_lp_sku_name[P->sku]); plat_write(m, "\r\nversion = 01.03\r\nstatus = VERIFIED\r\n");
    int bad = 0; u64 bytes = 0;
    for (int f = 0; f < LP_NFILE && !bad; f++) {
        const LpFile *L = &k_lp_files[f]; if (!lp_file_in_pack(L, P)) continue;
        lp_rel(rel, L->guid); path_join(to, nw, rel); path_join(from, cur, rel);
        int use_cur = plat_file_size(from) == (i64)L->size && lp_fingerprint(from, fp) && str_eq(fp, L->fp);
        if (!use_cur) path_join(from, g_src[g_from[f]].dir, rel);
        u64 b = 0;
        if (!lp_copy_hash(from, to, &b, full) || b != L->size || !lp_fingerprint(to, fp) || !str_eq(fp, L->fp) || !lp_quick_hash(to, quick)) { bad = 1; lr(g_lp_cancel ? "CANCELLED" : "COPY FAILED: "); if (!g_lp_cancel) lr(to); lr("\n"); break; }
        bytes += b; char sz[24]; fmt_dec(sz, (i64)L->size);
        plat_write(m, "file = "); plat_write(m, rel); plat_write(m, "|"); plat_write(m, sz); plat_write(m, "|"); plat_write(m, full); plat_write(m, "|"); plat_write(m, quick); plat_write(m, "\r\n");
    }
    { char b[16]; fmt_dec(b, nf); plat_write(m, "files = "); plat_write(m, b); plat_write(m, bad ? "\r\ncomplete = 0\r\n" : "\r\ncomplete = 1\r\n"); }
    plat_close(m);
    if (bad) { lp_delete_pack_dir(nw); path_join(mf, nw, "language.manifest.tmp"); plat_unlink(mf); plat_rmdir(nw); lr("  nothing changed; the previous pack (if any) is untouched\n"); return 0; }
    path_join(to, nw, "language.manifest"); plat_replace_file(mf, to);   /* manifest last: a pack without it is never used */
    char why[160]; int nb = 0;
    if (!lp_pack_check(nw, P, 1, why, sizeof why, &nb)) { lr("CHECK FAILED after copying: "); lr(why); lr("\n"); lp_delete_pack_dir(nw); return 0; }
    if (plat_is_dir(cur) && !lp_rename_retry(cur, old)) { lr("cannot move the previous pack aside - nothing changed.\nClose any Explorer windows or programs using "); lr(cur);
        lr(" and press Repair again.\n"); lp_delete_pack_dir(nw); return 0; }
    if (!lp_rename_retry(nw, cur)) { lr("cannot rename the new pack into place - the previous pack was put back. Close any Explorer windows or programs using "); lr(base);
        lr(" and press Repair again.\n"); if (plat_is_dir(old)) lp_rename_retry(old, cur); lp_delete_pack_dir(nw); return 0; }
    if (plat_is_dir(old)) lp_delete_pack_dir(old);
    lr("INSTALLED, VERIFIED ("); lr_d(nf); lr(" files, "); lr_mb(bytes); lr(reuse ? ", " : ""); if (reuse) { lr_d(reuse); lr(" re-used from the previous pack"); } lr(") in "); lr(cur); lr("\n");
    return 1;
}
static int lp_remove_one(const char *base, const LpPack *P) {
    char cur[1100]; path_join(cur, base, P->code); lp_cleanup(base, P->code);
    lr(P->name); lr(" ("); lr(P->code); lr("): ");
    if (!plat_is_dir(cur)) { lr("not installed\n"); return 1; }
    int n = lp_delete_pack_dir(cur);
    lr(plat_is_dir(cur) ? "files removed, but the folder holds other files - left in place\n" : "removed\n"); (void)n;
    return 1;
}
static void lp_exp_cb2(const char *name, void *ctx) { char d[1100]; path_join(d, (const char *)ctx, name); LpManifest m; int ok = lp_manifest_read(d, &m);
    lr("      "); lr(name); lr(": "); if (ok) { lr("EXPERIMENTAL, "); lr_d(m.nf); lr(" files (unverified)\n"); } else lr("no manifest\n"); }
static void lp_exp_cb(const char *name, void *ctx) { char d[1100]; path_join(d, (const char *)ctx, name); lr("    edition "); lr(name); lr(":\n"); plat_list_dirs(d, lp_exp_cb2, d); }

static void lp_list_packs(const char *base, int mode) {
    lr("language packs in "); lr(base); lr(" (check: "); lr(mode == 2 ? "full SHA-256" : mode ? "sizes + quick hashes" : "sizes"); lr(")\n");
    for (int i = 0; i < LP_NPACK; i++) { const LpPack *P = &k_lp_packs[i]; char d[1100], why[160]; int nb = 0; path_join(d, base, P->code);
        lr("  "); lr(P->code); lr("  "); lr(P->name); lr(P->nslot == 2 ? " (text + voices)" : " (text only)"); lr(": ");
        if (!plat_is_dir(d)) { lr("not installed\n"); continue; }
        if (lp_pack_check(d, P, mode, why, sizeof why, &nb)) lr("VERIFIED, complete - selectable (language = "), lr(P->name), lr(")\n");
        else { lr("NOT usable: "); lr(why); lr(" -> run --repair "); lr(P->code); lr("\n"); } }
    char e[1100]; path_join(e, base, "experimental");
    if (plat_is_dir(e)) { lr("  experimental (never used unless chosen with language_experimental):\n");
        plat_list_dirs(e, lp_exp_cb, e); }
}
/* ---- inventory (no asset bytes: names, sizes, SHA-256, versions) ---- */
typedef struct { void *out; int json, hashes_only, quick, first; const char *root, *sub; int nfiles; u64 bytes; } LpInv;
static const char *lp_manifest_lookup(const char *rel, u64 *size) {   /* CUSA02636 01.03 file list (ud_manifest_v103.h) */
    /* w38_rel: the shared list, then CUSA02636's own files (as before: the primary edition's list) */
    const UdFile *T[2] = {g_ud_common_files, g_ud_files_2636}; u64 N[2] = {sizeof g_ud_common_files / sizeof g_ud_common_files[0], sizeof g_ud_files_2636 / sizeof g_ud_files_2636[0]};
    for (int t = 0; t < 2; t++) for (u64 i = 0; i < N[t]; i++) if (str_eq(T[t][i].path, rel)) { *size = T[t][i].size; return T[t][i].path; }
    return 0;
}
static const char *lp_known_name(const char *name, char *info, int n) {   /* what our metadata knows about a lump name */
    Sb s = {info, 0, n}; info[0] = 0;
    const char *g = name; while (*g && !str_starts(g, "_lump_")) g++; if (!*g) return info; g += 6;
    for (int f = 0; f < LP_NFILE; f++) if (str_starts(g, k_lp_files[f].guid)) { sb_s(&s, "VERIFIED slot: "); sb_s(&s, k_lp_sku[k_lp_files[f].sku]); sb_s(&s, " "); sb_s(&s, lp_slot_desc(k_lp_files[f].slot)); return info; }
    for (u64 i = 0; i < sizeof k_lp_unknown / sizeof k_lp_unknown[0]; i++) if (str_starts(g, k_lp_unknown[i].guid)) { char b[8]; sb_s(&s, "known level-list slot (group "); fmt_dec(b, k_lp_unknown[i].group); sb_s(&s, b); sb_s(&s, ", position "); fmt_dec(b, k_lp_unknown[i].pos); sb_s(&s, b); sb_s(&s, ") - language not verified"); return info; }
    sb_s(&s, "not a language slot in our metadata"); return info;
}
static void inv_w(LpInv *v, const char *t) { plat_write(v->out, t); }
static void inv_file_cb(const char *name, void *ctx) {
    LpInv *v = (LpInv *)ctx; char rel[600], p[1200], h[65], info[160], b[24];
    Sb s = {rel, 0, sizeof rel}; sb_s(&s, v->sub); sb_s(&s, "/"); sb_s(&s, name);
    path_join(p, v->root, rel); i64 sz = plat_file_size(p); if (sz < 0) return;
    u64 ms = 0; int known = lp_manifest_lookup(rel, &ms) != 0;
    if (known && (u64)sz == ms) return;                            /* same name and size as CUSA02636 01.03: not a candidate */
    if (!(v->quick ? lp_fingerprint(p, h) : sha_file(p, h))) return;
    lp_known_name(name, info, sizeof info); if (known) { Sb t = {info + str_len(info), 0, 60}; sb_s(&t, info[0] ? "; " : ""); sb_s(&t, "same name as CUSA02636, different size"); }
    fmt_dec(b, sz); v->nfiles++; v->bytes += (u64)sz;
    if (v->hashes_only) { inv_w(v, h); inv_w(v, "  "); inv_w(v, b); inv_w(v, "  "); inv_w(v, rel); inv_w(v, "\r\n"); return; }
    if (v->json) { inv_w(v, v->first ? "\r\n      " : ",\r\n      "); v->first = 0; inv_w(v, "{\"path\": \""); inv_w(v, rel); inv_w(v, "\", \"size\": "); inv_w(v, b); inv_w(v, v->quick ? ", \"fingerprint\": \"" : ", \"sha256\": \""); inv_w(v, h); inv_w(v, "\", \"note\": \""); inv_w(v, info); inv_w(v, "\"}"); return; }
    inv_w(v, "  "); inv_w(v, rel); inv_w(v, "  size "); inv_w(v, b); inv_w(v, v->quick ? "  fingerprint " : "  sha256 "); inv_w(v, h); if (info[0]) { inv_w(v, "  ["); inv_w(v, info); inv_w(v, "]"); } inv_w(v, "\r\n");
}
static int lp_inventory(const char *outpath, int json, int hashes_only, int quick) {
    void *o = plat_open_write(outpath); if (!o) { lr("cannot write "); lr(outpath); lr("\n"); return 2; }
    LpInv v = {o, json, hashes_only, quick, 1, 0, 0, 0, 0};
    static const char *const subs[] = {"localcachepink/lumps", "localcachepink/streams", "Movies/Mono", "audio/wwise/ps4"};
    if (json) inv_w(&v, "{\r\n  \"tool\": \"ud_langscan inventory 1 (w33_lang)\",\r\n  \"note\": \"names, sizes and hashes of files that differ from CUSA02636 01.03 - no file contents\",\r\n  \"folders\": [");
    else inv_w(&v, "; ud_langscan inventory 1 (w33_lang): names, sizes and hashes of files whose name or size differs from CUSA02636 01.03.\r\n; No file contents. Attach this file to a GitHub issue.\r\n");
    for (int i = 0; i < g_nsrc; i++) {
        LpSrc *s = &g_src[i]; const char *lab = s->ok ? lp_label(s->title) : "UNSUPPORTED"; const char *reg = s->ok ? lp_region_of(s->title) : 0;
        const char *ev = s->eboot >= 0 && s->eboot != 99 ? k_lp_eboot[s->eboot].what : s->eboot == 99 ? "not a measured executable" : "no eboot.bin";
        if (json) { inv_w(&v, i ? ",\r\n    {" : "\r\n    {"); inv_w(&v, "\"title_id\": \""); inv_w(&v, s->title); inv_w(&v, "\", \"app_ver\": \""); inv_w(&v, s->appver); inv_w(&v, "\", \"category\": \""); inv_w(&v, s->cat);
            inv_w(&v, "\", \"content_id\": \""); inv_w(&v, s->content); inv_w(&v, "\", \"title_id_txt\": \""); inv_w(&v, s->tid); inv_w(&v, "\", \"eboot_sha256\": \""); inv_w(&v, s->esha); inv_w(&v, "\", \"eboot\": \""); inv_w(&v, ev);
            inv_w(&v, "\", \"status\": \""); inv_w(&v, lab); inv_w(&v, "\", \"region_in_game_code\": \""); inv_w(&v, reg ? reg : ""); inv_w(&v, "\", \"files\": ["); v.first = 1; }
        else { inv_w(&v, "\r\n[folder "); char b[8]; fmt_dec(b, i + 1); inv_w(&v, b); inv_w(&v, "] title "); inv_w(&v, s->title); inv_w(&v, "  app_ver "); inv_w(&v, s->appver); inv_w(&v, "  category "); inv_w(&v, s->cat);
            inv_w(&v, "  content "); inv_w(&v, s->content); inv_w(&v, "\r\n  status "); inv_w(&v, lab); if (s->sku >= 0 && !str_eq(s->appver, "01.03")) inv_w(&v, " edition, but this folder is version 01.00/other: the 01.03 update is required"); if (reg) { inv_w(&v, " (game code region: "); inv_w(&v, reg); inv_w(&v, ")"); }
            inv_w(&v, "\r\n  eboot.bin sha256 "); inv_w(&v, s->esha[0] ? s->esha : "-"); inv_w(&v, " ("); inv_w(&v, ev); inv_w(&v, ")\r\n  title-id.txt: "); inv_w(&v, s->tid[0] ? s->tid : "(empty or missing)"); inv_w(&v, "\r\n"); }
        int n0 = v.nfiles; u64 b0 = v.bytes;
        if (s->ok) for (int k = 0; k < 4; k++) { char d[1100]; path_join(d, s->dir, subs[k]); v.root = s->dir; v.sub = subs[k]; plat_list_files(d, inv_file_cb, &v); }
        if (json) { char b[24]; inv_w(&v, "], \"candidate_files\": "); fmt_dec(b, v.nfiles - n0); inv_w(&v, b); inv_w(&v, ", \"candidate_bytes\": "); fmt_dec(b, (i64)(v.bytes - b0)); inv_w(&v, b); inv_w(&v, "}"); }
        lr("inventory of "); lr(s->dir); lr(": "); lr(lab); lr(", "); lr_d(v.nfiles - n0); lr(" candidate language files ("); lr_mb(v.bytes - b0); lr(")\n");
    }
    if (json) inv_w(&v, "\r\n  ]\r\n}\r\n");
    plat_close(o); lr("written: "); lr(outpath); lr("\n"); return 0;
}

/* ---- experimental import (explicit consent; isolated folder; never used unless chosen) ---- */
typedef struct { const char *src_root, *dst; void *m; int n, bad; u64 bytes; } LpExp;
static void exp_cb(const char *name, void *ctx) {
    LpExp *e = (LpExp *)ctx; char rel[600], from[1200], to[1200], full[65], quick[65], b[24]; u64 ms = 0;
    Sb s = {rel, 0, sizeof rel}; sb_s(&s, "localcachepink/lumps/"); sb_s(&s, name);
    if (lp_manifest_lookup(rel, &ms)) return;                     /* only names CUSA02636 does not have (they can never replace a file) */
    path_join(from, e->src_root, rel);
    path_join(to, e->dst, rel); if (plat_file_size(to) >= 0) return;   /* already taken from the update folder */
    u64 got = 0; if (!lp_copy_hash(from, to, &got, full) || !lp_quick_hash(to, quick)) { e->bad++; return; }
    fmt_dec(b, (i64)got); plat_write(e->m, "file = "); plat_write(e->m, rel); plat_write(e->m, "|"); plat_write(e->m, b); plat_write(e->m, "|"); plat_write(e->m, full); plat_write(e->m, "|"); plat_write(e->m, quick); plat_write(e->m, "\r\n");
    e->n++; e->bytes += got;
}
static int lp_install_experimental(const char *base) {
    LpSrc *s0 = 0; for (int i = 0; i < g_nsrc; i++) if (g_src[i].ok) { s0 = &g_src[i]; break; }
    if (!s0) { lr("no edition folder\n"); return 2; }
    if (!str_eq(lp_label(s0->title), "EXPERIMENTAL")) { lr("REFUSED: "); lr(s0->title); lr(" is "); lr(lp_label(s0->title)); lr(" - experimental import is only for editions the game code knows and we have not verified\n"); return 2; }
    char ver[16] = ""; for (int i = 0; i < g_nsrc; i++) if (g_src[i].ok && str_eq(g_src[i].title, s0->title) && (ver[0] == 0 || str_eq(g_src[i].cat, "gp"))) mem_copy(ver, g_src[i].appver, 16);
    char ed[64], ex[1100], dst[1100], nw[1100]; Sb s = {ed, 0, sizeof ed}; sb_s(&s, s0->title); sb_s(&s, "_"); sb_s(&s, ver[0] ? ver : "unknown");
    path_join(ex, base, "experimental"); plat_mkdir(base); plat_mkdir(ex); path_join(ex, ex, ed); plat_mkdir(ex); path_join(dst, ex, "all"); path_join(nw, ex, ".new_all");
    if (plat_is_dir(nw)) lp_delete_pack_dir(nw);
    lp_mkdirs(nw); char mf[1100]; path_join(mf, nw, "language.manifest.tmp"); void *m = plat_open_write(mf); if (!m) return 1;
    plat_write(m, "; EXPERIMENTAL language files copied by ud_langscan from the player's own "); plat_write(m, ed); plat_write(m, " folders - NOT verified by us\r\nformat = 1\r\nlanguage = all\r\nedition = ");
    plat_write(m, s0->title); plat_write(m, "\r\nversion = "); plat_write(m, ver); plat_write(m, "\r\nstatus = EXPERIMENTAL\r\n");
    LpExp e = {0, nw, m, 0, 0, 0};
    for (int i = 0; i < g_nsrc; i++) if (g_src[i].ok && str_eq(g_src[i].title, s0->title) && str_eq(g_src[i].cat, "gp")) { char d[1100]; path_join(d, g_src[i].dir, "localcachepink/lumps"); e.src_root = g_src[i].dir; plat_list_files(d, exp_cb, &e); }
    for (int i = 0; i < g_nsrc; i++) if (g_src[i].ok && str_eq(g_src[i].title, s0->title) && !str_eq(g_src[i].cat, "gp")) { char d[1100]; path_join(d, g_src[i].dir, "localcachepink/lumps"); e.src_root = g_src[i].dir; plat_list_files(d, exp_cb, &e); }
    { char b[16]; fmt_dec(b, e.n); plat_write(m, "files = "); plat_write(m, b); plat_write(m, "\r\ncomplete = 0\r\n"); } plat_close(m);
    if (e.bad || !e.n) { lr(e.n ? "copy errors - nothing installed\n" : "no files that CUSA02636 lacks - nothing to import\n"); lp_delete_pack_dir(nw); plat_unlink(mf); plat_rmdir(nw); return 1; }
    char to[1100]; path_join(to, nw, "language.manifest"); plat_replace_file(mf, to);
    if (plat_is_dir(dst)) lp_delete_pack_dir(dst);
    plat_replace_file(nw, dst);
    lr("EXPERIMENTAL files of "); lr(ed); lr(": "); lr_d(e.n); lr(" files ("); lr_mb(e.bytes); lr(") in "); lr(dst);
    lr("\nThey are NOT used unless you add to Config\\settings.ini:\n    language_experimental = "); lr(ed); lr("\\all\n    text_language = <the language>\n"
       "and they can be removed with --remove-experimental "); lr(ed); lr(" (verified packs are not touched). Unverified: may show wrong text, no voices or crash.\n");
    return 0;
}

/* total bytes an install of these packs would copy (for the GUI's progress bar and space check) */
static u64 lp_pack_bytes(const LpPack *P) { u64 n = 0; for (int f = 0; f < LP_NFILE; f++) if (lp_file_in_pack(&k_lp_files[f], P)) n += k_lp_files[f].size; return n; }
#ifndef UD_LANGSCAN_NO_MAIN
#ifndef UD_LANGSCAN_HOST
__declspec(dllimport) u32 __attribute__((ms_abi)) GetConsoleProcessList(u32 *, u32);
__declspec(dllimport) int __attribute__((ms_abi)) ReadConsoleInputA(void *, void *, u32, u32 *);
__declspec(dllimport) int __attribute__((ms_abi)) FlushConsoleInputBuffer(void *);
static int lp_own_console(void) { u32 ids[4]; return GetConsoleProcessList(ids, 4) == 1; }   /* started from Explorer: the console closes with us */
static void lp_wait_key(void) {
    plat_write(0, "\nPress any key to close.\n"); void *in = GetStdHandle((u32)-10); FlushConsoleInputBuffer(in);
    for (;;) { u8 rec[20]; u32 n = 0; if (!ReadConsoleInputA(in, rec, 1, &n) || !n) break; if (*(u16 *)rec == 1 /* KEY_EVENT */ && *(u32 *)(rec + 4) /* key down */) break; }
}
#else
static int lp_own_console(void) { return 0; }
static void lp_wait_key(void) {}
#endif
static const char k_lp_usage[] =
    "Until Dawn native port - language packs (ud_langscan)\n\n"
    "The easy way: start UDLanguageImporter.exe (Language Importer) - pick your second PS4 edition's folder and press Install.\n\n"
    "Command line (for scripts):\n"
    "  ud_langscan <game folder> [<update folder>]                 check an edition (e.g. E:\\PS4\\CUSA00194)\n"
    "  ud_langscan --install all --from <update folder> <game folder>   import every language that edition has and your game lacks\n"
    "      (from CUSA00194: fr de it nl tr; from CUSA02636: es pt ru pl da fi no sv; codes: --install ru,es)\n"
    "  ud_langscan --list [--verify full]                         state of the installed language packs\n"
    "  ud_langscan --repair fr --from <update folder> <game folder>      fix a damaged pack\n"
    "  ud_langscan --remove fr                                    remove one pack\n"
    "  ud_langscan --inventory <update folder> <game folder> --json      report for another edition (GitHub issue)\n"
    "Packs go to Languages next to UntilDawn.exe; then set  language = french  in Config\\settings.ini.\n";
static int lp_main(int argc, char **argv);
int e3_main(int argc, char **argv) {
    if (argc <= 1) { plat_write(0, k_lp_usage); if (lp_own_console()) lp_wait_key(); return 2; }   /* no arguments: usage, no report file */
    int r = lp_main(argc, argv);
    if (lp_own_console()) lp_wait_key();   /* e.g. a folder dropped onto the exe in Explorer */
    return r;
}
static int lp_main(int argc, char **argv) {
    const char *ldir = 0, *codes = 0, *outp = 0, *verify = "quick"; int cmd = 0, nofp = 0, json = 0, hashonly = 0, quick = 0, consent = 0; const char *exp_rm = 0;
    enum { C_SCAN, C_INSTALL, C_REPAIR, C_REMOVE, C_LIST, C_INV, C_EXP, C_EXPRM };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (str_eq(a, "--install") && i + 1 < argc) { cmd = C_INSTALL; codes = argv[++i]; }
        else if (str_eq(a, "--repair") && i + 1 < argc) { cmd = C_REPAIR; codes = argv[++i]; }
        else if (str_eq(a, "--remove") && i + 1 < argc) { cmd = C_REMOVE; codes = argv[++i]; }
        else if (str_eq(a, "--list")) cmd = C_LIST;
        else if (str_eq(a, "--verify") && i + 1 < argc) verify = argv[++i];
        else if (str_eq(a, "--inventory")) cmd = C_INV;
        else if (str_eq(a, "--install-experimental")) cmd = C_EXP;
        else if (str_eq(a, "--remove-experimental") && i + 1 < argc) { cmd = C_EXPRM; exp_rm = argv[++i]; }
        else if (str_eq(a, "--i-accept-unverified-files")) consent = 1;
        else if (str_eq(a, "--languages-dir") && i + 1 < argc) ldir = argv[++i];
        else if (str_eq(a, "--from")) { }
        else if (str_eq(a, "--out") && i + 1 < argc) outp = argv[++i];
        else if (str_eq(a, "--json")) json = 1;
        else if (str_eq(a, "--hashes-only")) hashonly = 1;
        else if (str_eq(a, "--quick")) quick = 1;
        else if (str_eq(a, "--no-fingerprint")) nofp = 1;
        else if (a[0] == '-' && a[1] == '-') { plat_write(0, "unknown option "); plat_write(0, a); plat_write(0, "\n"); return 2; }
        else if (g_nsrc < 8) { int n = str_len(a); if (n > 1000) n = 1000; mem_copy(g_src[g_nsrc].dir, a, (u64)n); g_src[g_nsrc].dir[n] = 0; g_nsrc++; }
    }
    char base[1100];
    if (ldir) { int n = str_len(ldir); if (n > 1000) n = 1000; mem_copy(base, ldir, (u64)n); base[n] = 0; } else { plat_exe_dir(base, sizeof base); path_join(base, base, "Languages"); }
    lp_find_primary(base);
    g_lr = plat_open_write("ud_langscan_report.txt");
    lr("Until Dawn native port - language packs (ud_langscan, w33_lang, build " __DATE__ ")\nNo game data inside; edition folders are only read.\n\n");
    for (int i = 0; i < g_nsrc; i++) { lp_read_src(&g_src[i]); lp_print_src(i); }
    for (int i = 0; i < g_nsrc; i++) if (lp_inside(base, g_src[i].dir) || lp_inside(g_src[i].dir, base)) { lr("\nREFUSED: the Languages folder must be outside the edition folders.\n"); if (g_lr) plat_close(g_lr); return 2; }
    int rc = 0;
    if (cmd == C_INV) {
        if (!g_nsrc) { lr("--inventory needs at least one folder\n"); rc = 2; }
        else rc = lp_inventory(outp ? outp : json ? "ud_langscan_inventory.json" : "ud_langscan_inventory.txt", json, hashonly, quick);
    } else if (cmd == C_LIST) {
        lp_list_packs(base, str_eq(verify, "full") ? 2 : str_eq(verify, "size") ? 0 : 1);
    } else if (cmd == C_EXP) {
        if (!consent) { lr("\nREFUSED: experimental import copies files we have NOT verified. They may show wrong text, have no voices or\ncrash the game. Repeat with --i-accept-unverified-files to accept this. Verified packs are never touched.\n"); rc = 2; }
        else rc = lp_install_experimental(base);
    } else if (cmd == C_EXPRM) {
        char d[1100]; path_join(d, base, "experimental"); path_join(d, d, exp_rm);
        if (!plat_is_dir(d)) { lr("no experimental folder "); lr(d); lr("\n"); rc = 1; }
        else { char a[1100]; path_join(a, d, "all"); lp_delete_pack_dir(a); plat_rmdir(d); lr(plat_is_dir(d) ? "experimental files removed; folder kept (other files)\n" : "experimental folder removed\n"); }
    } else if (cmd == C_INSTALL || cmd == C_REPAIR || cmd == C_REMOVE) {
        const LpPack *pick[LP_NPACK]; int np = 0;
        if (lp_eq_ci(codes, "all")) for (int i = 0; i < LP_NPACK; i++) pick[np++] = &k_lp_packs[i];
        else for (const char *q = codes; *q; ) { char w[8]; int k = 0; while (*q && *q != ',' && k < 7) w[k++] = *q++; w[k] = 0; if (*q == ',') q++;
            const LpPack *P = lp_pack_by_code(w); if (P) pick[np++] = P; else { lr("unknown language code "); lr(w); lr(" (verified packs: fr de it nl tr from CUSA00194; es pt ru pl da fi no sv from CUSA02636)\n"); rc = 2; } }
        for (int i = 0; i < LP_NPACK && plat_is_dir(base); i++) lp_cleanup(base, k_lp_packs[i].code);   /* left-overs of interrupted runs, every pack */
        if (cmd == C_REMOVE) { lr("\n"); for (int i = 0; i < np; i++) lp_remove_one(base, pick[i]); }
        else {
            int have[2] = {0, 0}; for (int i = 0; i < g_nsrc; i++) if (g_src[i].sku == 0 || g_src[i].sku == 1) have[g_src[i].sku] = 1;
            int need[2] = {0, 0}; for (int i = 0; i < np; i++) if (!lp_in_primary(pick[i])) need[pick[i]->sku] = 1;
            if (cmd == C_INSTALL && ((need[0] && !have[0]) || (need[1] && !have[1]))) { lr("\n--install needs the folders of the edition that has these languages (update first, then the game folder): ");
                lr(need[0] && !have[0] ? "CUSA00194 " : ""); lr(need[1] && !have[1] ? "CUSA02636" : ""); lr("\n"); rc = 2; }
            else {
                for (int k = 0; k < 2; k++) if (have[k]) { int n_ok = lp_resolve(k, nofp); lr("\n"); lr(k_lp_sku_name[k]); lr(" language files: "); lr_d(n_ok); lr(k ? " of 684 verified\n" : " of 443 verified\n"); }
                plat_mkdir(base);
                for (int i = 0; i < np; i++) if (!lp_install_one(base, pick[i], have[pick[i]->sku])) rc = 1;
                lr("\nUse: Config\\settings.ini  language = <name or code>, e.g. language = french  (English if the pack is not complete)\n");
            }
        }
    } else {
        int want = -1; for (int i = 0; i < g_nsrc; i++) if (g_src[i].sku >= 0) { want = g_src[i].sku; break; }
        if (want < 0) { static char h[2048]; lr("\nNothing to import:\n"); for (int i = 0; i < g_nsrc; i++) { Sb s = {h, 0, sizeof h}; lp_folder_hint(&g_src[i], &s); lr("  "); lr(h); lr("\n"); } rc = 1; }
        else { int n = lp_resolve(want, nofp); lr("\nlanguage files of "); lr(k_lp_sku[want]); lr(" 01.03: "); lr_d(n); lr(" verified\n");
            { int base = 0, upd = 0; for (int i = 0; i < g_nsrc; i++) if (g_src[i].sku == want) { if (str_eq(g_src[i].cat, "gp")) upd = 1; else base = 1; }
              if (upd && !base) lr("  Only the UPDATE folder was given: also give the game folder (base version), e.g. ...\\CUSA00194 next to ...\\CUSA00194-patch.\n");
              if (base && !upd && want == 0) lr("  Only the game folder was given: the 01.03 UPDATE folder is required too (it changes the 5 menu text files).\n"); }
            for (int s = 0; s < LP_NSLOT; s++) if (g_slot_all[s]) { lr(g_slot_ok[s] == g_slot_all[s] ? "  COMPLETE    " : g_slot_ok[s] ? "  INCOMPLETE  " : "  ABSENT      "); lr(lp_slot_desc(s)); lr(": "); lr_d(g_slot_ok[s]); lr(" / "); lr_d(g_slot_all[s]); lr("\n"); }
            if (want == 0 || want == 1) { lr("\npacks that --install can make (VERIFIED, complete sets only):\n"); for (int i = 0; i < LP_NPACK; i++) { if (k_lp_packs[i].sku != want) continue; lr(lp_in_primary(&k_lp_packs[i]) ? "  in your game  " : lp_pack_src_ok(&k_lp_packs[i]) ? "  yes  " : "  no   "); lr(k_lp_packs[i].code); lr("  "); lr(k_lp_packs[i].name); lr(k_lp_packs[i].speech ? " (text + voices)\n" : " (text only)\n"); } } }
    }
    if (g_lr) plat_close(g_lr);
    return rc;
}
#endif
