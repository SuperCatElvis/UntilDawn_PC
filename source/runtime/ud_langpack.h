/* ud_langpack.h - language packs (w33_lang): Languages\<code>\ folders made once by ud_langscan from the player's own
 * second edition, read by UntilDawn.exe. Shared by the runtime (e3_loader.c) and the tool (ud_langscan.c).
 * Our metadata only (ud_langpack_data.h); a pack's manifest is written by the tool from the player's own files.
 *
 * Pack folder:   <Languages>\<code>\language.manifest
 *                <Languages>\<code>\localcachepink\lumps\levels.game.thegame.leveldescription_lump_<guid>.core
 * Manifest (text): "format = 1", "language = fr", "edition = CUSA00194", "version = 01.03", "status = VERIFIED",
 *                  "complete = 1", "files = N", then one line per file:
 *                  "file = <relative path>|<size>|<SHA-256 of the whole file>|<quick hash>"
 * quick hash = SHA-256 over the decimal size + the first and last 64 KiB (whole file if <= 128 KiB).
 * Coverage (measured 20 Nov): CUSA00194 has 443 language-only files = 5 x 70 text + 3 x 31 speech lumps; every other
 * file (movies, streams, Wwise init bank, all other lumps) is identical to CUSA02636, so a pack holds ALL of the
 * edition's language-specific text and audio for that language. */
#ifndef UD_LANGPACK_H
#define UD_LANGPACK_H
#include "ud_langpack_data.h"

typedef struct { const char *code, *name; int game_idx; int slot[4]; int nslot; int sku; int speech; } LpPack;
/* w38_rel: packs from EITHER European edition (k_lp_slot indices; sku 0 = CUSA00194, 1 = CUSA02636), so a player whose game
   folder is one edition can import the other's languages. The dubbed languages need their text and speech slots; nl / tr are
   text only. The four Nordic text slots Y1-Y4 are not told apart (which is which was never measured), so each Nordic pack
   holds all four (4 x 70 small text files); the game picks the right one by the language index. */
static const LpPack k_lp_packs[] = {
    {"fr", "French",     2,  {0, 1, -1, -1},   2, 0, 1},
    {"de", "German",     4,  {2, 3, -1, -1},   2, 0, 1},
    {"it", "Italian",    5,  {4, 5, -1, -1},   2, 0, 1},
    {"nl", "Dutch",      6,  {7, -1, -1, -1},  1, 0, 0},   /* slot Z (P10 p4) - text only, English voices */
    {"tr", "Turkish",    19, {6, -1, -1, -1},  1, 0, 0},   /* slot X (P10 p5) - text only, English voices */
    {"es", "Spanish",    3,  {8, 9, -1, -1},   2, 1, 1},
    {"pt", "Portuguese", 7,  {10, 11, -1, -1}, 2, 1, 1},
    {"ru", "Russian",    10, {12, 13, -1, -1}, 2, 1, 1},
    {"pl", "Polish",     11, {14, 15, -1, -1}, 2, 1, 1},
    {"da", "Danish",     12, {16, 17, 18, 19}, 4, 1, 0},
    {"fi", "Finnish",    13, {16, 17, 18, 19}, 4, 1, 0},
    {"no", "Norwegian",  14, {16, 17, 18, 19}, 4, 1, 0},
    {"sv", "Swedish",    15, {16, 17, 18, 19}, 4, 1, 0},
};
static const char *const k_lp_sku_name[2] = {"CUSA00194", "CUSA02636"};
#define LP_NPACK ((int)(sizeof k_lp_packs / sizeof k_lp_packs[0]))
static const LpPack *lp_pack_by_code(const char *c) {
    for (int i = 0; i < LP_NPACK; i++) { const char *a = k_lp_packs[i].code; int k = 0; while (a[k] && (c[k] | 0x20) == a[k]) k++; if (!a[k] && !c[k]) return &k_lp_packs[i]; }
    return 0;
}
static const LpPack *lp_pack_by_idx(int idx) { for (int i = 0; i < LP_NPACK; i++) if (k_lp_packs[i].game_idx == idx) return &k_lp_packs[i]; return 0; }
static int lp_file_in_pack(const LpFile *L, const LpPack *P) { if (L->sku != P->sku) return 0; for (int k = 0; k < P->nslot; k++) if (L->slot == P->slot[k]) return 1; return 0; }
static int lp_pack_nfiles(const LpPack *P) { int n = 0; for (int f = 0; f < (int)(sizeof k_lp_files / sizeof k_lp_files[0]); f++) n += lp_file_in_pack(&k_lp_files[f], P); return n; }
static void lp_rel(char *out, const char *guid) {
    Sb s = {out, 0, 200}; sb_s(&s, "localcachepink/lumps/levels.game.thegame.leveldescription_lump_"); sb_s(&s, guid); sb_s(&s, ".core");
}
/* w38_rel: 1 = every file of pack P is in game folder dir with its 01.03 size, i.e. the language is part of that edition */
static int lp_pack_in_game(const char *dir, const LpPack *P) {
    char rel[200], p[1100]; int n = 0;
    for (int f = 0; f < (int)(sizeof k_lp_files / sizeof k_lp_files[0]); f++) {
        const LpFile *L = &k_lp_files[f]; if (!lp_file_in_pack(L, P)) continue;
        lp_rel(rel, L->guid); path_join(p, dir, rel); if (plat_file_size(p) != (i64)L->size) return 0; n++;
    }
    return n > 0;
}
/* quick hash (see top); 0 if unreadable */
static int lp_quick_hash(const char *path, char out[65]) {
    void *f = hf_open_rb(path); if (!f) return 0;
    i64 sz = hf_seek(f, 0, 2); if (sz < 0) { hf_close(f); return 0; }
    static u8 qb[131072]; Sha256 s; sha_init(&s);
    char d[24]; fmt_dec(d, sz); sha_add(&s, (const u8 *)d, (u64)str_len(d));
    const u64 M = 65536; u64 offs[2] = {0, (u64)sz > 2 * M ? (u64)sz - M : M}; int nb = (u64)sz > 2 * M ? 2 : 1; u64 len0 = (u64)sz > 2 * M ? M : (u64)sz;
    for (int k = 0; k < nb; k++) { u64 want = k ? M : len0, got = 0, n; hf_seek(f, (i64)offs[k], 0); while (got < want && (n = hf_read(f, qb + got, want - got)) > 0) got += n; sha_add(&s, qb, got); }
    hf_close(f); sha_hex(&s, out); return 1;
}
/* full SHA-256 of a file (same as sha_file, kept separate for the pack code) */
static int lp_full_hash(const char *path, char out[65]) { return sha_file(path, out); }

/* manifest: parse into a small table (the files themselves are checked by the caller) */
typedef struct { char rel[200]; u64 size; char full[65], quick[65]; } LpMfFile;
typedef struct { char language[8], edition[16], version[16], status[16]; int complete, nfiles, format; LpMfFile *f; int nf, cap; } LpManifest;
static int lp_hexlen(const char *s) { int n = 0; while ((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')) n++; return n; }
static void lp_copy_tok(char *o, const char *v, int n, char stop) { int k = 0; while (v[k] && v[k] != stop && v[k] != '\r' && v[k] != '\n' && k < n - 1) { o[k] = v[k]; k++; } o[k] = 0; while (k && o[k - 1] == ' ') o[--k] = 0; }
static int lp_manifest_read(const char *pack_dir, LpManifest *m) {
    static LpMfFile tab[512]; char p[1100]; u64 n = 0;
    mem_copy(m, &(LpManifest){0}, sizeof *m); m->f = tab; m->cap = 512;
    path_join(p, pack_dir, "language.manifest");
    char *d = (char *)plat_read_file(p, &n); if (!d) return 0;
    for (u64 i = 0; i < n; ) {
        const char *ln = d + i; u64 e = i; while (e < n && d[e] != '\n') e++;
        d[e] = 0; i = e + 1;
        if (ln[0] == ';' || ln[0] == '#') continue;
        const char *eq = ln; while (*eq && *eq != '=') eq++; if (!*eq) continue;
        char key[24]; int kl = (int)(eq - ln); if (kl > 23) kl = 23; for (int k = 0; k < kl; k++) key[k] = ln[k]; key[kl] = 0; while (kl && key[kl - 1] == ' ') key[--kl] = 0;
        const char *v = eq + 1; while (*v == ' ') v++;
        if (str_eq(key, "format")) m->format = (int)str_to_u64(v);
        else if (str_eq(key, "language")) lp_copy_tok(m->language, v, sizeof m->language, 0);
        else if (str_eq(key, "edition")) lp_copy_tok(m->edition, v, sizeof m->edition, 0);
        else if (str_eq(key, "version")) lp_copy_tok(m->version, v, sizeof m->version, 0);
        else if (str_eq(key, "status")) lp_copy_tok(m->status, v, sizeof m->status, 0);
        else if (str_eq(key, "complete")) m->complete = (int)str_to_u64(v);
        else if (str_eq(key, "files")) m->nfiles = (int)str_to_u64(v);
        else if (str_eq(key, "file") && m->nf < m->cap) {
            LpMfFile *F = &m->f[m->nf]; lp_copy_tok(F->rel, v, sizeof F->rel, '|');
            const char *q = v; while (*q && *q != '|') q++; if (*q) q++;
            F->size = str_to_u64(q); while (*q && *q != '|') q++; if (*q) q++;
            if (lp_hexlen(q) != 64) continue; mem_copy(F->full, q, 64); F->full[64] = 0; q += 64; if (*q == '|') q++;
            if (lp_hexlen(q) != 64) continue; mem_copy(F->quick, q, 64); F->quick[64] = 0;
            m->nf++;
        }
    }
    return m->format == 1;
}
/* check a VERIFIED pack folder against our metadata and its manifest. mode 0 = sizes, 1 = + quick hash, 2 = + full SHA-256.
   Returns 1 if complete and intact; *why gets a short reason otherwise. */
static int lp_pack_check(const char *pack_dir, const LpPack *P, int mode, char *why, int whyn, int *nbad) {
    LpManifest m; Sb w = {why, 0, whyn}; why[0] = 0; *nbad = 0;
    if (!lp_manifest_read(pack_dir, &m)) { sb_s(&w, "no language.manifest"); return 0; }
    if (!str_eq(m.language, P->code) || !str_eq(m.status, "VERIFIED") || !m.complete) { sb_s(&w, "manifest is not a complete VERIFIED "); sb_s(&w, P->code); sb_s(&w, " pack"); return 0; }
    int need = lp_pack_nfiles(P), seen = 0; char rel[200], p[1100], h[65];
    for (int f = 0; f < (int)(sizeof k_lp_files / sizeof k_lp_files[0]); f++) {
        const LpFile *L = &k_lp_files[f]; if (!lp_file_in_pack(L, P)) continue;
        lp_rel(rel, L->guid); const LpMfFile *F = 0;
        for (int i = 0; i < m.nf; i++) if (str_eq(m.f[i].rel, rel)) { F = &m.f[i]; break; }
        path_join(p, pack_dir, rel); i64 sz = plat_file_size(p);
        int ok = F && F->size == L->size && sz == (i64)L->size;
        if (ok && mode >= 1) ok = lp_quick_hash(p, h) && str_eq(h, F->quick);
        if (ok && mode >= 2) ok = lp_full_hash(p, h) && str_eq(h, F->full);
        if (ok) seen++; else (*nbad)++;
    }
    if (seen != need) { sb_s(&w, "files missing or damaged: "); char b[16]; fmt_dec(b, need - seen); sb_s(&w, b); sb_s(&w, " of "); fmt_dec(b, need); sb_s(&w, b); return 0; }
    return 1;
}
#endif
