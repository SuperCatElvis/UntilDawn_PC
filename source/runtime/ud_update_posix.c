/* ud_update_posix.c - w26_update: the updater's logic on Linux (tests only; no network: the GitHub request fails as
   "offline"). Build: cc -O1 -o ud_update_posix ud_update_posix.c ; then tests/update/run_update_tests.sh. Unit tests:
   ud_update_posix --selftest */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#ifndef UD_VERSION
#define UD_VERSION "0.4.1"
#endif
static void *up_fopen_r(const char *p) { return fopen(p, "rb"); }
static void *up_fopen_w(const char *p) { return fopen(p, "wb"); }
static i64 up_fread(void *f, void *b, u64 n) { size_t r = fread(b, 1, n, (FILE *)f); return ferror((FILE *)f) ? -1 : (i64)r; }
static int up_fwrite(void *f, const void *b, u64 n) { return fwrite(b, 1, n, (FILE *)f) == n; }
static int up_fclose(void *f, int flush) { int ok = 1; if (flush) { ok = fflush((FILE *)f) == 0; fsync(fileno((FILE *)f)); } return (fclose((FILE *)f) == 0) && ok; }
static int up_exists(const char *p) { struct stat st; if (stat(p, &st)) return 0; return S_ISDIR(st.st_mode) ? 2 : 1; }
static int up_mkdir1(const char *p) { return mkdir(p, 0755) == 0 || up_exists(p) == 2; }
static int g_fail_move_target_match;   /* test hook: none */
static int up_move(const char *a, const char *b) { (void)g_fail_move_target_match; return rename(a, b) == 0; }
static int up_delete(const char *p) { return unlink(p) == 0; }
static int up_rmdir(const char *p) { return rmdir(p) == 0; }
static int up_list(const char *dir, int (*cb)(void *, const char *, int), void *ctx) {
    DIR *d = opendir(dir); if (!d) return 0;
    char names[512][256]; int isd[512], n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 512) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(names[n], 256, "%s", e->d_name); char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); isd[n] = up_exists(p) == 2; n++; }
    closedir(d);
    for (int i = 0; i < n; i++) if (!cb(ctx, names[i], isd[i])) break;
    return 1;
}
static int g_up_quiet;
static void *up_alloc(u64 n) { return malloc(n); }
static void up_free(void *p) { free(p); }
static FILE *g_log;
static void up_log_raw(const char *s) { fputs(s, stderr); if (g_log) { fputs(s, g_log); fflush(g_log); } }
static void up_crash(void) { _exit(9); }
static void up_progress(const char *t) { (void)t; }
static int up_http_get(const char *url, int api, int (*sink)(void *, const u8 *, u32), void *sctx, i64 *clen, char *err, int errn) {
    (void)url; (void)api; (void)sink; (void)sctx; *clen = -1; snprintf(err, errn, "no network in the Linux test build"); return 1;
}
#include "ud_update_core.h"
static int up_ask(const UpRel *r, const char *cur) { fprintf(stderr, "ASK: install %s over %s? (no dialog in the test build: not now)\n", r->ver, cur); return 1; }
static void up_message(const char *title, const char *text, int error) { if (!g_up_quiet) fprintf(stderr, "MESSAGE%s [%s]: %s\n", error ? " (error)" : "", title, text); }
static int up_check_deadline(UpCtx *c, UpRel *r, int ms) { (void)ms; int k = up_check(c, r); return k == 0 ? 0 : 2; }
static void up_exe_dir(char *out, int n) { snprintf(out, n, "."); }
static int up_log_open(const char *p) { g_log = fopen(p, "ab"); return g_log != 0; }
static void up_progress_close(void) {}
#include "ud_update_main.h"

/* ------------------------------------------------------------------ unit tests */
static int g_nf, g_np;
#define T(cond, what) do { if (cond) g_np++; else { g_nf++; fprintf(stderr, "FAIL %s (line %d)\n", what, __LINE__); } } while (0)
static int pol(const char *r) { char w[64]; return up_policy(r, w, sizeof w); }
static int selftest(const char *fixdir) {
    /* SHA-256 / CRC-32 known answers */
    { UpSha s; char h[65]; up_sha_init(&s); up_sha_add(&s, (const u8 *)"abc", 3); up_sha_hex(&s, h);
      T(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc");
      T(up_crc32((const u8 *)"123456789", 9) == 0xCBF43926u, "crc32 check value"); }
    /* versions */
    { UpVer a, b;
      T(up_ver_parse("v0.4.2", &a) && up_ver_parse("0.4.1", &b) && up_ver_cmp(&a, &b) == 1, "0.4.2 > 0.4.1");
      T(up_ver_parse("v0.10.0", &a) && up_ver_parse("0.9.9", &b) && up_ver_cmp(&a, &b) == 1, "0.10.0 > 0.9.9 (numeric)");
      T(up_ver_parse("0.4", &a) && up_ver_parse("0.4.0", &b) && up_ver_cmp(&a, &b) == 0, "0.4 == 0.4.0");
      T(up_ver_parse("1.0.0.1", &a) && up_ver_parse("1.0", &b) && up_ver_cmp(&a, &b) == 1, "1.0.0.1 > 1.0");
      T(!up_ver_parse("v0.4.2-rc1", &a), "prerelease tag rejected");
      T(!up_ver_parse("", &a) && !up_ver_parse("v", &a) && !up_ver_parse("0..1", &a) && !up_ver_parse("1.2.3.4.5", &a) && !up_ver_parse("latest", &a) && !up_ver_parse("1.2.", &a), "malformed versions"); }
    /* sha txt */
    { char h[65];
      T(up_parse_sha_txt("8cf592d77b5d80bd3ae5256cef59dfe99848637b93682b158c1426f22ecc916e  UntilDawn_PC_v0.4.1.zip\n", 91, "UntilDawn_PC_v0.4.1.zip", h) && !strcmp(h, "8cf592d77b5d80bd3ae5256cef59dfe99848637b93682b158c1426f22ecc916e"), "sha txt (real 0.4.1 format)");
      T(up_parse_sha_txt("8CF592D77B5D80BD3AE5256CEF59DFE99848637B93682B158C1426F22ECC916E *UntilDawn_PC_v0.4.1.zip\r\n", 92, "UntilDawn_PC_v0.4.1.zip", h), "sha txt upper case + binary marker + CRLF");
      T(up_parse_sha_txt("8cf592d77b5d80bd3ae5256cef59dfe99848637b93682b158c1426f22ecc916e\n", 65, "x.zip", h), "sha txt hash only");
      T(!up_parse_sha_txt("8cf592d77b5d80bd3ae5256cef59dfe99848637b93682b158c1426f22ecc916e  Other.zip\n", 76, "UntilDawn_PC_v0.4.1.zip", h), "sha txt for another file refused");
      T(!up_parse_sha_txt("8cf592d77b5d80bd3ae5256cef59dfe9984863  x\n", 42, "x", h), "short hash refused"); }
    /* install policy */
    T(pol("UntilDawn.exe") == 1 && pol("ud_update.exe") == 1 && pol("pipelines.udm") == 1 && pol("README.txt") == 1 && pol("CHANGELOG_v0.4.2.md") == 1, "program files at the root");
    T(pol("compat/ud_compat.exe") == 1 && pol("Compat/run_compat.bat") == 1 && pol("compat/README_COMPAT.txt") == 1, "compat files");
    T(pol("Config/settings.ini") == 0 && pol("config/x.txt") == 0 && pol("Game/eboot.bin") == 0 && pol("GAME/PUT_YOUR_GAME_FILES_HERE.txt") == 0, "Config / Game never");
    T(pol("savedata/a.txt") == 0 && pol("Cache/vulkan_pipelines.bin") == 0 && pol("Logs/UntilDawn.log") == 0, "savedata / Cache / Logs never");
    T(pol("evil.dll") == 0 && pol("settings.ini") == 0 && pol("compat/x.dll") == 0 && pol("compat/sub/x.exe") == 0 && pol("other/x.exe") == 0 && pol("Config/") == 0, "non-program files skipped");
    T(pol("../x.exe") < 0 && pol("a/../../x.exe") < 0 && pol("/x.exe") < 0 && pol("C:/x.exe") < 0 && pol("compat//x.exe") < 0 && pol("./x.exe") < 0 && pol("x\x01.exe") < 0 && pol("") < 0, "unsafe names refused");
    /* JSON */
    { char e[160]; UpRel r; u64 n = 0; char p[1200];
      snprintf(p, sizeof p, "%s/api_v0.4.2.json", fixdir); char *t = up_read_file(p, 1 << 20, &n);
      T(t && up_parse_release(t, n, &r, e, sizeof e) == 0 && !strcmp(r.ver, "0.4.2") && r.zip_size == 3800000 && strstr(r.body, "Release notes"), "GitHub-format answer parsed");
      if (t) {   /* variants made from it */
          char *v = (char *)malloc(n + 200);
#define VARIANT(from, to, expect, what) do { char *q = strstr(t, from); T(q != 0, "fixture has " from); if (q) { size_t o = q - t; memcpy(v, t, o); strcpy(v + o, to); strcat(v, q + strlen(from)); T(up_parse_release(v, strlen(v), &r, e, sizeof e) == expect, what); } } while (0)
          VARIANT("\"prerelease\": false", "\"prerelease\": true", 5, "pre-release refused");
          VARIANT("\"draft\": false", "\"draft\": true", 5, "draft refused");
          VARIANT("https://github.com/SuperCatElvis/UntilDawn_PC/releases/download/v0.4.2/UntilDawn_PC_v0.4.2.zip\"", "https://github.com/Someone/UntilDawn_PC/releases/download/v0.4.2/UntilDawn_PC_v0.4.2.zip\"", 6, "zip from another repository refused");
          VARIANT("https://github.com/SuperCatElvis/UntilDawn_PC/releases/download/v0.4.2/UntilDawn_PC_v0.4.2.zip\"", "http://github.com/SuperCatElvis/UntilDawn_PC/releases/download/v0.4.2/UntilDawn_PC_v0.4.2.zip\"", 6, "plain-HTTP zip refused");
          VARIANT("download/v0.4.2/UntilDawn_PC_v0.4.2.zip.sha256.txt", "download/v0.4.1/UntilDawn_PC_v0.4.2.zip.sha256.txt", 0, "checksum file from another tag: parsed");
          T(!r.sha_url[0] && r.zip_digest[0], "  ... that .sha256.txt is not used (only the digest)");
          VARIANT("\"digest\": \"sha256:0000000000000000000000000000000000000000000000000000000000000000\"", "\"digest\": null", 0, "no digest, .sha256.txt present: accepted");
          T(!r.zip_digest[0] && r.sha_url[0], "  ... verification by .sha256.txt only");
          VARIANT("\"digest\": \"sha256:0000000000000000000000000000000000000000000000000000000000000000\"", "\"digest\": \"sha256:12zz\"", 0, "malformed digest counts as none");
          T(!r.zip_digest[0], "  ... digest ignored");
          VARIANT("/download/v0.4.2/UntilDawn_PC_v0.4.2.zip.sha256.txt\"", "/download/v0.4.2/other.txt\"", 0, "digest only (no .sha256.txt, the real publishing practice): accepted");
          T(!strcmp(r.zip_digest, "0000000000000000000000000000000000000000000000000000000000000000") && !r.sha_url[0], "  ... digest taken from the zip asset");
          { char *q = strstr(t, "\"digest\": \"sha256:"); char *q2 = strstr(t, "/download/v0.4.2/UntilDawn_PC_v0.4.2.zip.sha256.txt\"");
            if (q && q2) { char *w = strdup(t); char *a1 = w + (q - t), *a2 = w + (q2 - t); memcpy(a1 + 1, "xxxxxx", 6); memcpy(a2 + 10, "XX", 2);
              T(up_parse_release(w, strlen(w), &r, e, sizeof e) == 7, "neither digest nor .sha256.txt: refused (never unverified)"); free(w); } }
          VARIANT("\"tag_name\": \"v0.4.2\"", "\"tag_name\": \"v0.4.2-rc1\"", 4, "rc tag refused");
          VARIANT("\"tag_name\": \"v0.4.2\"", "\"tag_name\": \"v0.4.3\"", 6, "assets of another version refused");
          VARIANT("\"size\": 3800000", "\"size\": 999999999999", 8, "oversized package refused");
          free(v); free(t); }
      /* the REAL answer of GitHub for v0.4.1 (saved by job G11): asset UntilDawn_0.4.1.zip with digest, no .sha256.txt */
      snprintf(p, sizeof p, "%s/api_real_v0.4.1.json", fixdir); t = up_read_file(p, 1 << 20, &n);
      g_up_legacy_name = 0;
      T(t && up_parse_release(t, n, &r, e, sizeof e) == 6, "real v0.4.1 answer, PC_v name only (UP_ACCEPT_LEGACY_NAME 0): not offered");
      g_up_legacy_name = 1;
      T(t && up_parse_release(t, n, &r, e, sizeof e) == 0 && !strcmp(r.zip_name, "UntilDawn_0.4.1.zip") && r.zip_size == 3723348 && !r.sha_url[0], "real v0.4.1 answer with legacy names: package found, no .sha256.txt");
      T(!strcmp(r.zip_digest, "8cf592d77b5d80bd3ae5256cef59dfe99848637b93682b158c1426f22ecc916e"), "real v0.4.1 digest = 8cf592d7... (the SHA-256 of release\\UntilDawn_PC_v0.4.1.zip)");
      g_up_legacy_name = UP_ACCEPT_LEGACY_NAME; if (t) free(t);
      T(UP_ACCEPT_LEGACY_NAME == 1, "default build accepts both names");
      {   /* the three accepted names, their order of preference, case, other versions */
#define URLP "https://github.com/SuperCatElvis/UntilDawn_PC/releases/download/v0.4.2/"
#define AS(name, d) "{\"size\":" #d ",\"digest\":\"sha256:" #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d #d "\",\"browser_download_url\":\"" URLP name "\"}"
#define REL(list) "{\"tag_name\":\"v0.4.2\",\"draft\":false,\"prerelease\":false,\"assets\":[" list "]}"
          struct { const char *j; int rc; const char *name; char d; const char *what; } cs[] = {
              {REL(AS("UntilDawn_v0.4.2.zip", 1)), 0, "UntilDawn_v0.4.2.zip", '1', "UntilDawn_v<ver>.zip accepted"},
              {REL(AS("UntilDawn_0.4.2.zip", 2)), 0, "UntilDawn_0.4.2.zip", '2', "UntilDawn_<ver>.zip accepted"},
              {REL(AS("UntilDawn_PC_v0.4.2.zip", 3)), 0, "UntilDawn_PC_v0.4.2.zip", '3', "UntilDawn_PC_v<ver>.zip accepted"},
              {REL(AS("untildawn_V0.4.2.ZIP", 4)), 0, "untildawn_V0.4.2.ZIP", '4', "name case-insensitive"},
              {REL(AS("UntilDawn_PC_v0.4.2.zip", 3) "," AS("UntilDawn_0.4.2.zip", 2) "," AS("UntilDawn_v0.4.2.zip", 1)), 0, "UntilDawn_v0.4.2.zip", '1', "all three: UntilDawn_v preferred (with its own digest)"},
              {REL(AS("UntilDawn_PC_v0.4.2.zip", 3) "," AS("UntilDawn_0.4.2.zip", 2)), 0, "UntilDawn_0.4.2.zip", '2', "UntilDawn_<ver> preferred over UntilDawn_PC_v"},
              {REL(AS("UntilDawn_v0.4.1.zip", 1)), 6, 0, 0, "UntilDawn_v of another version refused"},
              {REL(AS("UntilDawn_0.4.2.1.zip", 1)), 6, 0, 0, "longer version in the name refused"},
              {REL(AS("UntilDawn_v0.4.2.zip.bak", 1)), 6, 0, 0, "other extension refused"},
              {REL(AS("MyUntilDawn_v0.4.2.zip", 1)), 6, 0, 0, "other prefix refused"},
              {"{\"tag_name\":\"v0.4.2\",\"assets\":[{\"browser_download_url\":\"" URLP "UntilDawn_v0.4.2.zip\"}]}", 7, 0, 0, "UntilDawn_v without any hash refused"},
              {"{\"tag_name\":\"v0.4.2\",\"assets\":[{\"browser_download_url\":\"" URLP "UntilDawn_v0.4.2.zip\"},{\"browser_download_url\":\"" URLP "untildawn_v0.4.2.zip.SHA256.txt\"}]}", 0, "UntilDawn_v0.4.2.zip", 0, "UntilDawn_v + its .sha256.txt (any case) accepted"},
          };
          for (unsigned k = 0; k < sizeof cs / sizeof cs[0]; k++) {
              int rc = up_parse_release(cs[k].j, strlen(cs[k].j), &r, e, sizeof e);
              int ok = rc == cs[k].rc && (!cs[k].name || !strcmp(r.zip_name, cs[k].name)) && (!cs[k].d || r.zip_digest[0] == cs[k].d);
              if (k == sizeof cs / sizeof cs[0] - 1) ok = ok && r.sha_url[0] && !r.zip_digest[0];
              T(ok, cs[k].what);
          }
          g_up_legacy_name = 0;
          T(up_parse_release(cs[1].j, strlen(cs[1].j), &r, e, sizeof e) == 6 && up_parse_release(cs[0].j, strlen(cs[0].j), &r, e, sizeof e) == 0, "UP_ACCEPT_LEGACY_NAME 0: UntilDawn_<ver> off, UntilDawn_v still on");
          g_up_legacy_name = UP_ACCEPT_LEGACY_NAME; }
    }
    /* any major / minor / patch (user, 16 Nov): numeric per component, tags with or without v, names follow the version */
    { struct { const char *a, *b; int c; } vc[] = {
          {"1.0.0", "0.4.1", 1}, {"1.0.0", "0.99.99", 1}, {"0.10.0", "0.9.9", 1}, {"1.2.10", "1.2.9", 1}, {"10.0.0", "9.9.9", 1},
          {"v10.0.0", "9.99.999", 1}, {"0.9.9", "1.0.0", -1}, {"v1.0.0", "1.0.0", 0}, {"1.0", "v1.0.0", 0}, {"2.0.0", "10.0.0", -1}, {"123456.0.0", "99999.9.9", 1}};
      for (unsigned k = 0; k < sizeof vc / sizeof vc[0]; k++) { UpVer a, b; char m[96]; snprintf(m, sizeof m, "version %s vs %s = %d", vc[k].a, vc[k].b, vc[k].c);
          T(up_ver_parse(vc[k].a, &a) && up_ver_parse(vc[k].b, &b) && up_ver_cmp(&a, &b) == vc[k].c, m); }
      char e[160]; UpRel r;
#define U2 "https://github.com/SuperCatElvis/UntilDawn_PC/releases/download/"
#define DG "\"digest\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
      const char *j1 = "{\"tag_name\":\"v1.0.0\",\"assets\":[{" DG "\"browser_download_url\":\"" U2 "v1.0.0/UntilDawn_v1.0.0.zip\"}]}";
      T(up_parse_release(j1, strlen(j1), &r, e, sizeof e) == 0 && !strcmp(r.ver, "1.0.0"), "tag v1.0.0: UntilDawn_v1.0.0.zip");
      const char *j2 = "{\"tag_name\":\"1.2.10\",\"assets\":[{" DG "\"browser_download_url\":\"" U2 "1.2.10/UntilDawn_1.2.10.zip\"}]}";
      T(up_parse_release(j2, strlen(j2), &r, e, sizeof e) == 0 && !strcmp(r.ver, "1.2.10"), "tag 1.2.10 without v: UntilDawn_1.2.10.zip");
      const char *j3 = "{\"tag_name\":\"v10.0.0\",\"assets\":[{" DG "\"browser_download_url\":\"" U2 "v10.0.0/UntilDawn_PC_v10.0.0.zip\"}]}";
      T(up_parse_release(j3, strlen(j3), &r, e, sizeof e) == 0 && !strcmp(r.ver, "10.0.0"), "tag v10.0.0: UntilDawn_PC_v10.0.0.zip");
      const char *j4 = "{\"tag_name\":\"v1.0.0\",\"assets\":[{" DG "\"browser_download_url\":\"" U2 "v1.0.0/UntilDawn_v1.0.zip\"}]}";
      T(up_parse_release(j4, strlen(j4), &r, e, sizeof e) == 6, "tag v1.0.0 with asset UntilDawn_v1.0.zip: not the same version text, refused");
      const char *j5 = "{\"tag_name\":\"1.0.0\",\"assets\":[{" DG "\"browser_download_url\":\"" U2 "v1.0.0/UntilDawn_v1.0.0.zip\"}]}";
      T(up_parse_release(j5, strlen(j5), &r, e, sizeof e) == 6, "asset in another tag's folder (v1.0.0 vs tag 1.0.0) refused"); }
    fprintf(stderr, "selftest: %d passed, %d failed\n", g_np, g_nf);
    return g_nf ? 1 : 0;
}
/* --unzip-check ZIP DIR: every entry inflated + CRC-checked, written under DIR/<name> (for comparison with Python) */
static int unzip_check(const char *zp, const char *out) {
    u64 n = 0; char *z = up_read_file(zp, UP_MAX_ZIP, &n); if (!z) { fprintf(stderr, "cannot read %s\n", zp); return 1; }
    static UpZEnt e[UP_MAX_ENTRIES]; char err[160]; int k = up_zip_list((u8 *)z, n, e, UP_MAX_ENTRIES, err, sizeof err);
    if (k < 0) { fprintf(stderr, "zip: %s\n", err); return 1; }
    for (int i = 0; i < k; i++) {
        if (e[i].name[strlen(e[i].name) - 1] == '/') continue;
        u8 *b = malloc(e[i].usize + 1); if (up_zip_extract((u8 *)z, n, &e[i], b, err, sizeof err)) { fprintf(stderr, "%s: %s\n", e[i].name, err); return 1; }
        char p[1200]; snprintf(p, sizeof p, "%s/%s", out, e[i].name); up_mkparent(p); up_write_file(p, b, e[i].usize);
        char w[64]; printf("%s rel=%s method=%d size=%u policy=%d\n", e[i].name, e[i].rel, e[i].method, e[i].usize, up_policy(e[i].rel, w, sizeof w)); free(b);
    }
    free(z); return 0;
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--selftest")) return selftest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--unzip-check")) return unzip_check(argv[2], argv[3]);
    return up_main(argc, argv);
}
