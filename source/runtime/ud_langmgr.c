/* ud_langmgr.c - Until Dawn Language Importer, UDLanguageImporter.exe (w33_lang build 12; optional, test builds only).
 * A small native Win32 window over ud_langscan's own functions (ud_langscan.c included with UD_LANGSCAN_NO_MAIN):
 * edition detection, fingerprint checks, safe install (temp folder + rename), repair, remove, inventory, experimental
 * import with consent. Edition folders are only read. Packs go to the Languages folder of the recompilation
 * (next to UntilDawn.exe, found automatically, or chosen with "Change..."). Copies run on a worker thread; Cancel
 * deletes the unfinished pack and leaves the previous one as it was.
 * One tick per language (build 8, user decision 22 Nov): a language is installed with its text and, where the edition
 * has it, its voice acting - the game loads voices by the subtitle language (P10/P11). Separate text/voice choice is
 * future work (TEXT_VOICE_RUNTIME.md). Dutch/Turkish: text only (English voices).
 * Build: build_langmgr.sh -> UDLanguageImporter.exe. */
#define UD_LANGSCAN_NO_MAIN
#include "ud_langscan.c"

#define W __attribute__((ms_abi))
__declspec(dllimport) int W GetMessageA(void *, void *, u32, u32);
__declspec(dllimport) i64 W SendMessageA(void *, u32, u64, i64);
__declspec(dllimport) int W EnableWindow(void *, int);
__declspec(dllimport) int W GetWindowTextA(void *, char *, int);
__declspec(dllimport) int W ShowWindow(void *, int);
__declspec(dllimport) int W UpdateWindow(void *);
__declspec(dllimport) int W InvalidateRect(void *, const void *, int);
__declspec(dllimport) void *W BeginPaint(void *, void *);
__declspec(dllimport) int W EndPaint(void *, const void *);
__declspec(dllimport) int W FillRect(void *, const void *, void *);
__declspec(dllimport) void W PostQuitMessage(int);
__declspec(dllimport) u64 W SetTimer(void *, u64, u32, void *);
__declspec(dllimport) void *W GetStockObject(int);
__declspec(dllimport) void *W CreateSolidBrush(u32);
__declspec(dllimport) void *W SHBrowseForFolderA(void *);
__declspec(dllimport) int W SHGetPathFromIDListA(void *, char *);
__declspec(dllimport) i32 W CoInitializeEx(void *, u32);
__declspec(dllimport) void W CoTaskMemFree(void *);

enum { ID_GAME = 100, ID_UPD, ID_BGAME, ID_BUPD, ID_SCAN, ID_INFO, ID_TARGET, ID_BTARGET, ID_ALL, ID_INSTALL, ID_VERIFY, ID_REPAIR, ID_REMOVE, ID_CANCEL, ID_INV, ID_EXP,
       ID_LOG, ID_SPACE, ID_PROG, ID_NOTE, ID_TXT = 200, ID_ST = 280 };
#define NROW 10
typedef struct { char code[8], name[40]; const LpPack *P; int avail, inst, ok; char status[160]; } Row;
static Row gm_row[NROW]; static int gm_nrow;
static void *gm_hw, *gm_game, *gm_upd, *gm_info, *gm_target, *gm_all, *gm_log, *gm_space, *gm_prog, *gm_note;
static void *gm_txt[NROW], *gm_st[NROW], *gm_btn[8];
/* installed packs (whatever edition is selected): verify / repair / remove act on these ticks only */
typedef struct { const LpPack *P; int ok, nbad; } IRow;
static IRow gm_irow[LP_NPACK]; static int gm_nirow;
static void *gm_itxt[LP_NPACK], *gm_ist[LP_NPACK], *gm_ihead;
static char gm_base[1100];
static volatile int gm_busy, gm_lock;
static char gm_out[65536]; static volatile int gm_outn;

static void out_lock(void) { while (__atomic_exchange_n(&gm_lock, 1, __ATOMIC_ACQUIRE)) Sleep(0); }
static void out_unlock(void) { __atomic_store_n(&gm_lock, 0, __ATOMIC_RELEASE); }
static void gui_text(const char *s) {
    out_lock();
    for (; *s; s++) { if (*s == '\n' && gm_outn < (int)sizeof gm_out - 2) gm_out[gm_outn++] = '\r'; if (gm_outn < (int)sizeof gm_out - 1) gm_out[gm_outn++] = *s; }
    gm_out[gm_outn] = 0; out_unlock();
}
static void gui_progress(u64 d, u64 t) { (void)d; (void)t; }
static void *mk(const char *cls, const char *txt, u32 style, int x, int y, int w, int h, int id) {
    void *c = CreateWindowExA(str_eq(cls, "EDIT") ? 0x200 : 0, cls, txt, 0x50000000u | style, x, y, w, h, gm_hw, (void *)(u64)id, GetModuleHandleA(0), 0);
    SendMessageA(c, 0x30, (u64)GetStockObject(17), 0);
    return c;
}
static void get_text(void *h, char *b, int n) { GetWindowTextA(h, b, n); int k = str_len(b); while (k && (b[k - 1] == ' ' || b[k - 1] == '\\' || b[k - 1] == '/')) b[--k] = 0; }
static int checked(void *h) { return SendMessageA(h, 0xF0, 0, 0) == 1; }
static void set_check(void *h, int v) { SendMessageA(h, 0xF1, v ? 1 : 0, 0); }
static const char *lang_name(const char *c) {
    static const char *const t[][2] = {{"en", "English"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"}, {"es", "Spanish"}, {"pt", "Portuguese"}, {"ru", "Russian"},
        {"pl", "Polish"}, {"nl", "Dutch"}, {"tr", "Turkish"}, {"sv", "Swedish"}, {"no", "Norwegian"}, {"fi", "Finnish"}, {"da", "Danish"}, {"ja", "Japanese"},
        {"ko", "Korean"}, {"zh-TW", "Chinese (Traditional)"}, {"zh-CN", "Chinese (Simplified)"}, {"es-LA", "Spanish (Latin America)"}, {"pt-BR", "Portuguese (Brazil)"}};
    for (u64 i = 0; i < sizeof t / sizeof t[0]; i++) if (str_eq(t[i][0], c)) return t[i][1];
    return c;
}

/* ---- the recompilation's Languages folder: next to UntilDawn.exe (this folder or its parent), settings languages_dir wins ---- */
static int find_target(char *out, const char *dir) {
    char p[1100]; path_join(p, dir, "UntilDawn.exe"); if (plat_file_size(p) < 0) return 0;
    path_join(out, dir, "Languages");
    path_join(p, dir, "Config/settings.ini"); u64 n = 0; char *d = (char *)plat_read_file(p, &n);
    if (d) for (u64 i = 0; i < n; ) { char *ln = d + i; u64 e = i; while (e < n && d[e] != '\n') e++; d[e] = 0; i = e + 1;
        if (str_starts(ln, "languages_dir")) { char *v = ln + 13; while (*v == ' ' || *v == '=') v++; int k = str_len(v); while (k && (v[k - 1] == '\r' || v[k - 1] == ' ')) v[--k] = 0; if (k) mem_copy(out, v, (u64)k + 1); } }
    return 1;
}
static void show_target(void) { char t[1200]; Sb s = {t, 0, sizeof t}; sb_s(&s, "Install into: "); sb_s(&s, gm_base[0] ? gm_base : "(not found - press Change... and pick the folder that contains UntilDawn.exe)"); SetWindowTextA(gm_target, t); }

static void space_line(void) {
    u64 sel = 0; for (int i = 0; i < gm_nrow; i++) if (gm_row[i].P && gm_row[i].avail && checked(gm_txt[i])) sel += lp_pack_bytes(gm_row[i].P);
    u64 fr = gm_base[0] ? plat_free_bytes(gm_base) : (u64)-1;
    if (fr == (u64)-1 && gm_base[0]) { char up[1100]; path_join(up, gm_base, ".."); fr = plat_free_bytes(up); }
    char t[200], b[24]; Sb q = {t, 0, sizeof t};
    sb_s(&q, "Required: "); fmt_dec(b, (i64)((sel + 500000) / 1000000)); sb_s(&q, b); sb_s(&q, " MB    Free: ");
    if (fr == (u64)-1) sb_s(&q, "?"); else { fmt_dec(b, (i64)(fr / 1000000)); sb_s(&q, b); sb_s(&q, " MB"); if (sel && fr < sel + sel / 20) sb_s(&q, "   - NOT ENOUGH SPACE"); }
    SetWindowTextA(gm_space, t);
}
static void sync_all(void) {   /* "All languages" = every available text box ticked */
    int any = 0, all = 1; for (int i = 0; i < gm_nrow; i++) if (gm_row[i].avail) { any = 1; if (!checked(gm_txt[i])) all = 0; }
    set_check(gm_all, any && all); EnableWindow(gm_all, any);
}
static void row_text_changed(int i) { (void)i; sync_all(); space_line(); }   /* one tick per language: text and its voice acting together */

/* scan the chosen folders and rebuild the language list */
static void gui_scan(void) {
    char g[1024], u[1024]; get_text(gm_game, g, sizeof g); get_text(gm_upd, u, sizeof u);
    if (g[0]) {   /* the UPDATE folder picked as the game folder: move it to the update field, look for the game folder next to it */
        LpSrc t; mem_copy(t.dir, g, (u64)str_len(g) + 1); lp_read_src(&t);
        if (t.ok && str_eq(t.cat, "gp") && (!u[0] || str_eq(u, g))) {
            mem_copy(u, g, (u64)str_len(g) + 1); SetWindowTextA(gm_upd, u); g[0] = 0;
            static const char *const suf[] = {"-patch", "-update", " update", "_patch"};
            for (int k = 0; k < 4 && !g[0]; k++) { int n = str_len(u), m = str_len(suf[k]); if (n > m && str_eq(u + n - m, suf[k])) { char c[1100]; mem_copy(c, u, (u64)(n - m)); c[n - m] = 0;
                char p[1100]; path_join(p, c, "sce_sys/param.sfo"); if (plat_file_size(p) >= 0) mem_copy(g, c, (u64)str_len(c) + 1); } }
            SetWindowTextA(gm_game, g);
            if (!g[0]) lr("The folder you picked is the UPDATE (01.03). It was moved to the update field; please also pick the GAME folder (base version).\n");
        }
    }
    if (g[0] && !u[0]) {   /* update folder next to the game folder: "<game>-patch" / "<game>-update" / "<game> update" */
        static const char *const suf[] = {"-patch", "-update", " update", "_patch"};
        for (int k = 0; k < 4; k++) { char c[1100]; Sb s = {c, 0, sizeof c}; sb_s(&s, g); sb_s(&s, suf[k]); char p[1100]; path_join(p, c, "sce_sys/param.sfo");
            if (plat_file_size(p) >= 0) { SetWindowTextA(gm_upd, c); mem_copy(u, c, (u64)str_len(c) + 1); break; } } }
    g_nsrc = 0;
    if (u[0]) { mem_copy(g_src[g_nsrc].dir, u, (u64)str_len(u) + 1); g_nsrc++; }
    if (g[0]) { mem_copy(g_src[g_nsrc].dir, g, (u64)str_len(g) + 1); g_nsrc++; }
    static char info[2048]; Sb s = {info, 0, sizeof info}; info[0] = 0;
    int sku = -1; const char *title = "", *label = "";
    for (int i = 0; i < g_nsrc; i++) {
        LpSrc *x = &g_src[i]; lp_read_src(x);
        sb_s(&s, i ? "\r\n" : "");
        if (!x->ok || str_eq(lp_label(x->title), "UNSUPPORTED")) { lp_folder_hint(x, &s); continue; }
        sb_s(&s, x->title); sb_s(&s, "  version "); sb_s(&s, x->appver); sb_s(&s, str_eq(x->cat, "gp") ? " (update)" : " (game)"); sb_s(&s, "  -  "); sb_s(&s, lp_label(x->title));
        const char *r = lp_region_of(x->title); if (r) { sb_s(&s, "  - region "); sb_s(&s, r); }
        if (!title[0]) { title = x->title; label = lp_label(x->title); } if (x->sku >= 0 && sku < 0) sku = x->sku;
    }
    lp_find_primary(gm_base);   /* w38_rel: the player's own edition (settings game_dir / Game) */
    int own = sku >= 0 && g_primary_title[0] && str_eq(title, g_primary_title);
    if (own) sb_s(&s, "\r\nThis is your own game edition: its languages are already in the game - nothing to import from it.");
    else if (sku == 0 || sku == 1) {
        int n = lp_resolve(sku, 0), old = 0; for (int f = 0; f < LP_NFILE; f++) if (k_lp_files[f].sku == sku && g_st[f] == LP_OLD) old++;
        char b[16]; fmt_dec(b, n); sb_s(&s, "\r\nLanguage files verified: "); sb_s(&s, b); sb_s(&s, sku ? " of 684." : " of 443.");
        if (old) sb_s(&s, "  Some menu files are still version 01.00: also select the 01.03 UPDATE folder (required).");
    }
    else if (title[0] && str_eq(label, "EXPERIMENTAL")) sb_s(&s, "\r\nThis Until Dawn edition is not verified yet: please press Inventory report and attach the file to a GitHub issue. Experimental import only at your own risk.");
    SetWindowTextA(gm_info, info);
    /* rows: the languages this edition offers in the game code, English excluded (always in the game) */
    gm_nrow = 0; const char *L = title[0] ? lp_region_langs(title) : "";
    for (const char *q = L; *q && gm_nrow < NROW; ) {
        char c[8]; int k = 0; while (*q && *q != ' ' && k < 7) c[k++] = *q++; c[k] = 0; while (*q == ' ') q++;
        if (str_eq(c, "en")) continue;
        Row *R = &gm_row[gm_nrow++]; mem_copy(R->code, c, 8); { const char *nm = lang_name(c); int n = str_len(nm); if (n > 39) n = 39; mem_copy(R->name, nm, (u64)n); R->name[n] = 0; }
        R->P = sku == 0 || sku == 1 ? lp_pack_by_code(c) : 0; if (R->P && R->P->sku != sku) R->P = 0; R->inst = R->ok = 0; R->avail = 0;
        Sb t = {R->status, 0, sizeof R->status};
        if (own || (R->P && lp_in_primary(R->P))) { sb_s(&t, "already in your game"); R->P = 0; }
        else if (!R->P) sb_s(&t, "not verified - not offered (Inventory report helps us add it)");
        else {
            char d[1100], why[160]; int nb = 0;
            if (gm_base[0]) { path_join(d, gm_base, R->P->code); R->inst = plat_is_dir(d); R->ok = R->inst && lp_pack_check(d, R->P, 0, why, sizeof why, &nb); }
            R->avail = lp_pack_src_ok(R->P);
            sb_s(&t, "SUPPORTED   ");
            sb_s(&t, R->inst ? (R->ok ? "installed (ticking it installs it again)" : "installed but DAMAGED - see below, Repair") : "not installed");
            sb_s(&t, R->avail ? "   |   source complete   |   " : "   |   SOURCE INCOMPLETE / OUTDATED   |   ");
            char mb[16]; fmt_dec(mb, (i64)((lp_pack_bytes(R->P) + 500000) / 1000000)); sb_s(&t, mb); sb_s(&t, " MB");
        }
    }
    for (int i = 0; i < NROW; i++) {
        int on = i < gm_nrow; ShowWindow(gm_txt[i], on ? 5 : 0); ShowWindow(gm_st[i], on ? 5 : 0);
        if (!on) continue;
        Row *R = &gm_row[i]; char t[96]; Sb q = {t, 0, sizeof t}; sb_s(&q, R->name);
        sb_s(&q, R->P ? (R->P->speech ? " - text + voice acting" : " - text only (English voices)") : R->status[0] == 'a' ? "" : " - contents not verified");
        SetWindowTextA(gm_txt[i], t);
        EnableWindow(gm_txt[i], R->avail); set_check(gm_txt[i], R->avail && !R->inst);   /* import only what this edition supplies; default: not yet installed */
        SetWindowTextA(gm_st[i], R->status);
    }
    if (gm_nrow == 0) { ShowWindow(gm_txt[0], 5); SetWindowTextA(gm_txt[0], title[0] ? "(nothing to import from this edition)" : "(select a PS4 game folder)"); EnableWindow(gm_txt[0], 0); set_check(gm_txt[0], 0); }
    /* installed packs: re-checked on every scan (sizes + quick hashes of every file) */
    gm_nirow = 0;
    for (int k = 0; k < LP_NPACK && gm_base[0]; k++) { char d[1100], why[160]; int nb = 0; path_join(d, gm_base, k_lp_packs[k].code); if (!plat_is_dir(d)) continue;
        IRow *I = &gm_irow[gm_nirow++]; I->P = &k_lp_packs[k]; I->ok = lp_pack_check(d, I->P, 1, why, sizeof why, &nb); I->nbad = nb; }
    for (int i = 0; i < LP_NPACK; i++) {
        int on = i < gm_nirow; ShowWindow(gm_itxt[i], on ? 5 : 0); ShowWindow(gm_ist[i], on ? 5 : 0); if (!on) continue;
        IRow *I = &gm_irow[i]; char t[96], st[160], b[16]; Sb q = {t, 0, sizeof t}, w = {st, 0, sizeof st};
        sb_s(&q, I->P->name); sb_s(&q, I->P->speech ? " (text + voices)" : " (text only)"); SetWindowTextA(gm_itxt[i], t);
        if (I->ok) sb_s(&w, "installed, VERIFIED");
        else { sb_s(&w, "DAMAGED ("); fmt_dec(b, I->nbad); sb_s(&w, b); sb_s(&w, ") - Repair from "); sb_s(&w, k_lp_sku_name[I->P->sku]); }
        SetWindowTextA(gm_ist[i], st); set_check(gm_itxt[i], !I->ok);
    }
    SetWindowTextA(gm_ihead, gm_nirow ? "3. Installed language packs (Verify / Repair / Remove act on the ticked ones):" : "3. Installed language packs: none");
    sync_all(); space_line(); show_target();
}

/* ---- worker ---- */
static int gm_job; static u8 gm_pick[LP_NPACK];
static WINAPI u32 worker(void *arg) {
    (void)arg; int have[2] = {0, 0}; for (int i = 0; i < g_nsrc; i++) if (g_src[i].sku == 0 || g_src[i].sku == 1) have[g_src[i].sku] = 1;
    if (gm_job == ID_INSTALL || gm_job == ID_REPAIR) {
        for (int k = 0; k < 2; k++) if (have[k]) lp_resolve(k, 0);
        int need[2] = {0, 0}; for (int i = 0; i < LP_NPACK; i++) if (gm_pick[i]) need[k_lp_packs[i].sku] = 1;
        if (gm_job == ID_INSTALL && ((need[0] && !have[0]) || (need[1] && !have[1]))) { lr("Install needs the game folder and 01.03 update folder of the edition that has these languages: ");
            lr(need[0] && !have[0] ? "CUSA00194 " : ""); lr(need[1] && !have[1] ? "CUSA02636" : ""); lr("\n"); }
        else { plat_mkdir(gm_base); for (int i = 0; i < LP_NPACK; i++) lp_cleanup(gm_base, k_lp_packs[i].code);
            g_lp_total = 0; g_lp_done = 0; for (int i = 0; i < LP_NPACK; i++) if (gm_pick[i]) g_lp_total += lp_pack_bytes(&k_lp_packs[i]);
            for (int i = 0; i < LP_NPACK && !g_lp_cancel; i++) if (gm_pick[i]) lp_install_one(gm_base, &k_lp_packs[i], have[k_lp_packs[i].sku]);
            if (g_lp_cancel) lr("\nCancelled: the unfinished pack was deleted; packs installed before are unchanged.\n");
            else lr("\nTo play: Config\\settings.ini  language = <name or code>, e.g. language = french\n"); }
    } else if (gm_job == ID_REMOVE) { for (int i = 0; i < LP_NPACK; i++) if (gm_pick[i]) lp_remove_one(gm_base, &k_lp_packs[i]); }
    else if (gm_job == ID_VERIFY) lp_list_packs(gm_base, 2);
    else if (gm_job == ID_INV) { char p[1100]; path_join(p, gm_base, "../ud_langscan_inventory.json"); lp_inventory(p, 1, 0, 0);
        lr("Attach this file to a GitHub issue (names, sizes and hashes only - no game data).\n"); }
    else if (gm_job == ID_EXP) lp_install_experimental(gm_base);
    lr("--- done ---\n");
    __atomic_store_n(&gm_busy, 0, __ATOMIC_RELEASE);
    return 0;
}
static void start_job(int job) {
    if (gm_busy) return;
    if (!gm_base[0]) { MessageBoxA(gm_hw, "The Languages folder of the game is not known yet: press Change... and select the folder that contains UntilDawn.exe.", "Language Importer", 0x30); return; }
    for (int i = 0; i < LP_NPACK; i++) gm_pick[i] = 0;
    if (job == ID_INSTALL || job == ID_REPAIR || job == ID_REMOVE) {
        int any = 0;
        if (job == ID_INSTALL) { for (int r = 0; r < gm_nrow; r++) if (gm_row[r].P && gm_row[r].avail && checked(gm_txt[r]))
                for (int i = 0; i < LP_NPACK; i++) if (&k_lp_packs[i] == gm_row[r].P) { gm_pick[i] = 1; any = 1; } }
        else for (int r = 0; r < gm_nirow; r++) if (checked(gm_itxt[r])) for (int i = 0; i < LP_NPACK; i++) if (&k_lp_packs[i] == gm_irow[r].P) { gm_pick[i] = 1; any = 1; }
        if (!any) { MessageBoxA(gm_hw, "Install: tick at least one language under 2 (only languages this edition supplies can be ticked).\nRepair / Remove: tick at least one installed pack under 3.", "Language Importer", 0x40); return; }
        if (job == ID_REMOVE && MessageBoxA(gm_hw, "Remove the ticked language packs from the Languages folder?\n(Your PS4 edition folders are not touched.)", "Language Importer", 0x24) != 6) return;
        if (job == ID_INSTALL) { u64 sel = 0; for (int i = 0; i < LP_NPACK; i++) if (gm_pick[i]) sel += lp_pack_bytes(&k_lp_packs[i]);
            u64 fr = plat_free_bytes(gm_base); if (fr == (u64)-1) { char up[1100]; path_join(up, gm_base, ".."); fr = plat_free_bytes(up); }
            if (fr != (u64)-1 && fr < sel + sel / 20) { MessageBoxA(gm_hw, "Not enough free disk space for the selected languages.", "Language Importer", 0x10); return; } }
    }
    if (job == ID_EXP && MessageBoxA(gm_hw, "EXPERIMENTAL IMPORT\n\nThis copies language files from an edition we have NOT verified. They may show wrong text, have no voices or crash "
                              "the game. They go to Languages\\experimental\\ and are used ONLY if you set language_experimental in settings.ini.\n"
                              "Verified language packs are not touched.\n\nDo you accept this risk?", "Language Importer - unverified files", 0x134) != 6) return;
    g_lp_cancel = 0; g_lp_done = 0; g_lp_total = 0; gm_job = job; gm_busy = 1;
    if (!CreateThread(0, 1u << 20, (void *)worker, 0, 0, 0)) { gm_busy = 0; lr("could not start the worker thread\n"); }
}
/* folder picker: the Explorer-style IFileOpenDialog (FOS_PICKFOLDERS: address bar, navigation pane, search), built into
   Windows Vista+; starts in `start` (or the last folder used); falls back to SHBrowseForFolder only if COM fails */
__declspec(dllimport) i32 W CoCreateInstance(const void *, void *, u32, const void *, void **);
__declspec(dllimport) i32 W SHCreateItemFromParsingName(const u16 *, void *, const void *, void **);
typedef struct { u32 a; u16 b, c; u8 d[8]; } Guid;
static const Guid k_clsid_fod = {0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const Guid k_iid_fod   = {0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
static const Guid k_iid_si    = {0x43826D1E, 0xE718, 0x42EE, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};
typedef i32 (W *ComFn)();
#define VT(o, i) (((ComFn *)(*(void ***)(o)))[i])
static char gm_last[1100];
static int browse_modern(char *path, const char *title, const char *start, int *com_ok) {
    void *dlg = 0; *com_ok = 0;
    if (CoCreateInstance(&k_clsid_fod, 0, 1 /* CLSCTX_INPROC_SERVER */, &k_iid_fod, &dlg) < 0 || !dlg) return 0;
    *com_ok = 1;
    u32 opt = 0; ((i32 (W *)(void *, u32 *))VT(dlg, 10))(dlg, &opt);
    ((i32 (W *)(void *, u32))VT(dlg, 9))(dlg, opt | 0x20 /* PICKFOLDERS */ | 0x40 /* FORCEFILESYSTEM */ | 0x800 /* PATHMUSTEXIST */);
    u16 w[1100];
    if (MultiByteToWideChar(0, 0, title, -1, w, 1100)) ((i32 (W *)(void *, const u16 *))VT(dlg, 17))(dlg, w);
    if (start && start[0] && plat_is_dir(start) && MultiByteToWideChar(0, 0, start, -1, w, 1100)) {
        void *it = 0; if (SHCreateItemFromParsingName(w, 0, &k_iid_si, &it) >= 0 && it) { ((i32 (W *)(void *, void *))VT(dlg, 12))(dlg, it); ((i32 (W *)(void *))VT(it, 2))(it); } }
    int ok = 0;
    if (((i32 (W *)(void *, void *))VT(dlg, 3))(dlg, gm_hw) >= 0) {   /* Show; cancelled -> HRESULT_FROM_WIN32(ERROR_CANCELLED) < 0 */
        void *it = 0;
        if (((i32 (W *)(void *, void **))VT(dlg, 20))(dlg, &it) >= 0 && it) {
            u16 *nm = 0;
            if (((i32 (W *)(void *, u32, u16 **))VT(it, 5))(it, 0x80058000u /* SIGDN_FILESYSPATH */, &nm) >= 0 && nm) {
                ok = WideCharToMultiByte(0, 0, nm, -1, path, 1100, 0, 0) > 0; CoTaskMemFree(nm); }
            ((i32 (W *)(void *))VT(it, 2))(it);
        }
    }
    ((i32 (W *)(void *))VT(dlg, 2))(dlg);
    return ok;
}
static int browse(char *path, const char *title, const char *cur) {
    char start[1100]; start[0] = 0;
    if (cur && cur[0]) { int n = str_len(cur); mem_copy(start, cur, (u64)n + 1); if (!plat_is_dir(start)) { while (n && start[n - 1] != '\\' && start[n - 1] != '/') n--; start[n ? n - 1 : 0] = 0; } }
    if (!start[0] || !plat_is_dir(start)) mem_copy(start, gm_last, sizeof gm_last);
    path[0] = 0; int com_ok = 0, ok = browse_modern(path, title, start, &com_ok);
    if (!com_ok) {   /* old tree dialog, only when COM is not available */
        struct { void *owner, *root; char *disp; const char *title; u32 flags; void *fn; i64 lp; int img; } bi = {gm_hw, 0, path, title, 0x41, 0, 0, 0};
        void *id = SHBrowseForFolderA(&bi);
        if (id) { ok = SHGetPathFromIDListA(id, path); CoTaskMemFree(id); }
    }
    if (ok) { int n = str_len(path); mem_copy(gm_last, path, (u64)n + 1); }
    return ok;
}
static WINAPI i64 prog_proc(void *h, u32 m, u64 w, i64 l) {
    if (m == 0xF) {
        u8 ps[80]; void *dc = BeginPaint(h, ps); int r[4]; GetClientRect(h, r);
        static void *bg, *fg; if (!bg) { bg = CreateSolidBrush(0xE0E0E0); fg = CreateSolidBrush(0x3C9A3C); }
        FillRect(dc, r, bg);
        u64 t = g_lp_total, d = g_lp_done; if (t) { int f[4] = {0, 0, (int)((u64)r[2] * (d > t ? t : d) / t), r[3]}; FillRect(dc, f, fg); }
        EndPaint(h, ps); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static WINAPI i64 wnd_proc(void *h, u32 m, u64 w, i64 l) {
    switch (m) {
    case 0x111: {
        int id = (int)(w & 0xFFFF); char p[1100];
        if (gm_busy && id != ID_CANCEL) return 0;
        char cur[1100]; cur[0] = 0;
        if (id == ID_BGAME) GetWindowTextA(gm_game, cur, sizeof cur); else if (id == ID_BUPD) { GetWindowTextA(gm_upd, cur, sizeof cur); if (!cur[0]) GetWindowTextA(gm_game, cur, sizeof cur); }
        else if (id == ID_BTARGET && gm_base[0]) { int n = str_len(gm_base); mem_copy(cur, gm_base, (u64)n + 1); }
        if (id == ID_BGAME) { if (browse(p, "Select the extracted PS4 GAME folder (e.g. CUSA00194)", cur)) { SetWindowTextA(gm_game, p); SetWindowTextA(gm_upd, ""); gui_scan(); } }
        else if (id == ID_BUPD) { if (browse(p, "Select the extracted UPDATE folder (01.03), if separate", cur)) { SetWindowTextA(gm_upd, p); gui_scan(); } }
        else if (id == ID_BTARGET) { if (browse(p, "Select the Until Dawn folder (the one with UntilDawn.exe)", cur)) {
                char t[1100]; if (find_target(t, p)) mem_copy(gm_base, t, (u64)str_len(t) + 1);
                else if (MessageBoxA(gm_hw, "UntilDawn.exe is not in this folder. Use a Languages folder here anyway (for tests)?", "Language Importer", 0x24) == 6) path_join(gm_base, p, "Languages");
                gui_scan(); } }
        else if (id == ID_SCAN) { lr("Rescanning the edition folders and every installed pack...\n"); gui_scan();
            for (int i = 0; i < gm_nirow; i++) { lr("  "); lr(gm_irow[i].P->name); lr(gm_irow[i].ok ? ": installed, VERIFIED\n" : ": DAMAGED - Repair\n"); }
            if (!gm_nirow) lr("  no installed packs\n"); }
        else if (id == ID_CANCEL) g_lp_cancel = 1;
        else if (id == ID_ALL) { int v = checked(gm_all); for (int i = 0; i < gm_nrow; i++) if (gm_row[i].avail) { set_check(gm_txt[i], v); row_text_changed(i); } sync_all(); space_line(); }
        else if (id >= ID_TXT && id < ID_TXT + NROW) row_text_changed(id - ID_TXT);
        else if (id == ID_INSTALL || id == ID_REPAIR || id == ID_REMOVE || id == ID_VERIFY || id == ID_INV || id == ID_EXP) start_job(id);
        return 0; }
    case 0x113: {
        static char tmp[65536]; int n = 0;
        out_lock(); if (gm_outn) { mem_copy(tmp, gm_out, (u64)gm_outn + 1); n = gm_outn; gm_outn = 0; gm_out[0] = 0; } out_unlock();
        if (n) { SendMessageA(gm_log, 0xB1, 0x7FFFFFFF, 0x7FFFFFFF); SendMessageA(gm_log, 0xC2, 0, (i64)tmp); }
        InvalidateRect(gm_prog, 0, 0);
        static int was; int b = gm_busy;
        int canimp = 0; for (int i = 0; i < gm_nrow; i++) if (gm_row[i].avail) canimp = 1;
        for (int i = 0; i < 8; i++) if (gm_btn[i]) EnableWindow(gm_btn[i], i == 7 ? b : i == 0 ? (!b && canimp) : (i == 2 || i == 3) ? (!b && gm_nirow) : !b);
        if (was && !b) gui_scan();
        was = b; return 0; }
    case 0x10: if (gm_busy) { g_lp_cancel = 1; MessageBoxA(h, "Cancelling the running copy - close again when the log says done.", "Language Importer", 0x40); return 0; } break;
    case 2: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int e3_main(int argc, char **argv) {
    (void)argc; (void)argv;
    CoInitializeEx(0, 2);
    { char me[1100], up[1100]; plat_exe_dir(me, sizeof me); path_join(up, me, "..");
      if (!find_target(gm_base, me) && !find_target(gm_base, up)) gm_base[0] = 0; }
    g_lr_hook = gui_text; g_lp_progress = gui_progress;
    void *inst = GetModuleHandleA(0);
    struct { u32 cb, style; void *proc; int ce, we; void *inst, *icon, *cursor, *brush; const char *menu, *cls; void *sm; } wc;
    { u8 *z = (u8 *)&wc; for (u64 i = 0; i < sizeof wc; i++) z[i] = 0; }
    wc.cb = sizeof wc; wc.proc = (void *)wnd_proc; wc.inst = inst; wc.cursor = LoadCursorA(0, (const char *)32512); wc.brush = (void *)(u64)16; wc.cls = "UdLangMgr";
    RegisterClassExA(&wc); wc.proc = (void *)prog_proc; wc.cls = "UdLangProg"; RegisterClassExA(&wc);
    gm_hw = CreateWindowExA(0, "UdLangMgr", "Until Dawn Language Importer", 0x10CA0000u, (int)0x80000000, (int)0x80000000, 860, 880, 0, 0, inst, 0);
    int y = 10;
    mk("STATIC", "1. Your second PS4 edition (only read, never changed) - game folder:", 0, 10, y, 820, 18, 0); y += 20;
    gm_game = mk("EDIT", "", 0x10080, 10, y, 720, 22, ID_GAME); mk("BUTTON", "Browse...", 0x10000, 740, y, 90, 22, ID_BGAME); y += 28;
    mk("STATIC", "   its update folder (found automatically when it sits next to the game folder):", 0, 10, y, 820, 18, 0); y += 20;
    gm_upd = mk("EDIT", "", 0x10080, 10, y, 720, 22, ID_UPD); mk("BUTTON", "Browse...", 0x10000, 740, y, 90, 22, ID_BUPD); y += 28;
    gm_info = mk("STATIC", "Select the game folder; it is checked automatically.", 0, 10, y, 820, 62, ID_INFO); y += 66;
    gm_target = mk("STATIC", "", 0, 10, y + 4, 720, 18, ID_TARGET); mk("BUTTON", "Change...", 0x10000, 740, y, 90, 22, ID_BTARGET); y += 30;
    mk("STATIC", "2. Import from this edition:", 0, 10, y, 400, 18, 0); y += 20;
    gm_all = mk("BUTTON", "All languages", 0x10003, 10, y, 300, 20, ID_ALL); y += 24;
    for (int i = 0; i < NROW; i++) {
        int yy = y + (i < 9 ? i : 8) * 24;   /* at most 9 rows are ever shown (the largest region list without English) */
        gm_txt[i] = mk("BUTTON", "", 0x10003, 20, yy, 330, 20, ID_TXT + i);
        gm_st[i] = mk("STATIC", "", 0, 360, yy + 3, 475, 18, ID_ST + i);
        ShowWindow(gm_txt[i], 0); ShowWindow(gm_st[i], 0);
    }
    y += 9 * 24 + 4;
    gm_ihead = mk("STATIC", "", 0, 10, y, 820, 18, 0); y += 20;
    /* w38_rel: up to 13 packs (both European editions) - two columns */
    for (int i = 0; i < LP_NPACK; i++) { int cx = i & 1 ? 425 : 20, cy = y + (i / 2) * 22;
        gm_itxt[i] = mk("BUTTON", "", 0x10003, cx, cy, 215, 20, 300 + i); gm_ist[i] = mk("STATIC", "", 0, cx + 220, cy + 3, 180, 18, 320 + i);
        ShowWindow(gm_itxt[i], 0); ShowWindow(gm_ist[i], 0); }
    y += ((LP_NPACK + 1) / 2) * 22 + 4;
    gm_note = mk("STATIC", "Each ticked language is installed complete: its text and, where the edition has it, its voice acting "
                          "(the game uses them together). Dutch, Turkish, Danish, Finnish, Norwegian and Swedish have no voice acting (English voices).", 0, 10, y, 820, 32, ID_NOTE); y += 36;
    gm_space = mk("STATIC", "", 0, 10, y, 820, 18, ID_SPACE); y += 24;
    gm_btn[0] = mk("BUTTON", "Install", 0x10000, 10, y, 110, 28, ID_INSTALL);
    gm_btn[1] = mk("BUTTON", "Verify (full)", 0x10000, 130, y, 110, 28, ID_VERIFY);
    gm_btn[2] = mk("BUTTON", "Repair", 0x10000, 250, y, 100, 28, ID_REPAIR);
    gm_btn[3] = mk("BUTTON", "Remove", 0x10000, 360, y, 100, 28, ID_REMOVE);
    gm_btn[4] = mk("BUTTON", "Inventory report", 0x10000, 470, y, 120, 28, ID_INV);
    gm_btn[5] = mk("BUTTON", "Experimental import...", 0x10000, 600, y, 150, 28, ID_EXP);
    gm_btn[6] = mk("BUTTON", "Rescan", 0x10000, 760, y, 70, 28, ID_SCAN); y += 36;
    gm_prog = CreateWindowExA(0x200, "UdLangProg", "", 0x50000000u, 10, y, 710, 20, gm_hw, (void *)(u64)ID_PROG, inst, 0);
    gm_btn[7] = mk("BUTTON", "Cancel", 0x10000, 730, y - 2, 100, 24, ID_CANCEL); y += 28;
    gm_log = mk("EDIT", "", 0x00200000 | 0x4 | 0x800 | 0x40, 10, y, 820, 840 - y - 10, ID_LOG);
    gui_scan();
    SetTimer(gm_hw, 1, 100, 0);
    u8 msg[48];
    while (GetMessageA(msg, 0, 0, 0) > 0) { TranslateMessage(msg); DispatchMessageA(msg); }
    return 0;
}
