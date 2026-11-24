/*
 * ud_update.c - w26_update: ud_update.exe, the updater of the Until Dawn PC build (Windows x64, no C library;
 * build: build_update.sh). Started by UntilDawn.exe before the game (Config\settings.ini update_check = on), or by hand.
 * The logic is in ud_update_core.h / ud_update_main.h (shared with the Linux test build); this file is the Windows
 * host: files, WinHTTP (HTTPS only, GitHub hosts only), the question dialog, a small progress window, the log.
 * Privacy: the only network traffic is GET https://api.github.com/repos/SuperCatElvis/UntilDawn_PC/releases/latest
 * (and, when the player chooses Install, the two release assets). Nothing about the PC or the player is sent; the
 * User-Agent is "UntilDawn_PC-updater/<version>"; cookies are off.
 */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#ifndef UD_VERSION
#define UD_VERSION "0.5.0"
#endif
#define WINAPI __attribute__((ms_abi))
#define IMP __declspec(dllimport)
/* kernel32 */
IMP void *WINAPI CreateFileA(const char *, u32, u32, void *, u32, u32, void *);
IMP int WINAPI ReadFile(void *, void *, u32, u32 *, void *);
IMP int WINAPI WriteFile(void *, const void *, u32, u32 *, void *);
IMP int WINAPI CloseHandle(void *);
IMP int WINAPI FlushFileBuffers(void *);
IMP u32 WINAPI GetFileAttributesA(const char *);
IMP int WINAPI CreateDirectoryA(const char *, void *);
IMP int WINAPI MoveFileExA(const char *, const char *, u32);
IMP int WINAPI DeleteFileA(const char *);
IMP int WINAPI RemoveDirectoryA(const char *);
IMP void *WINAPI FindFirstFileA(const char *, void *);
IMP int WINAPI FindNextFileA(void *, void *);
IMP int WINAPI FindClose(void *);
IMP u32 WINAPI GetModuleFileNameA(void *, char *, u32);
IMP char *WINAPI GetCommandLineA(void);
IMP void WINAPI ExitProcess(u32);
IMP void *WINAPI CreateThread(void *, u64, u32 (WINAPI *)(void *), void *, u32, u32 *);
IMP u32 WINAPI WaitForSingleObject(void *, u32);
IMP u32 WINAPI GetLastError(void);
IMP void WINAPI GetLocalTime(u16 *);
IMP u32 WINAPI GetCurrentThreadId(void);
IMP int WINAPI GetFileSizeEx(void *, i64 *);
IMP void *WINAPI LoadLibraryA(const char *);
IMP void *WINAPI GetProcAddress(void *, const char *);
IMP u32 WINAPI GetEnvironmentVariableA(const char *, char *, u32);
/* msvcrt */
IMP void *malloc(u64); IMP void free(void *);
/* user32 */
IMP int WINAPI MessageBoxA(void *, const char *, const char *, u32);
IMP void *WINAPI CreateWindowExA(u32, const char *, const char *, u32, int, int, int, int, void *, void *, void *, void *);
IMP int WINAPI DestroyWindow(void *);
IMP int WINAPI SetWindowTextA(void *, const char *);
IMP int WINAPI PeekMessageA(void *, void *, u32, u32, u32);
IMP int WINAPI TranslateMessage(const void *);
IMP i64 WINAPI DispatchMessageA(const void *);
IMP int WINAPI GetSystemMetrics(int);
IMP void *WINAPI SetWindowsHookExA(int, i64 (WINAPI *)(int, u64, i64), void *, u32);
IMP int WINAPI UnhookWindowsHookEx(void *);
IMP i64 WINAPI CallNextHookEx(void *, int, u64, i64);
IMP int WINAPI SetDlgItemTextA(void *, int, const char *);
/* winhttp */
IMP void *WINAPI WinHttpOpen(const u16 *, u32, const u16 *, const u16 *, u32);
IMP int WINAPI WinHttpSetTimeouts(void *, int, int, int, int);
IMP void *WINAPI WinHttpConnect(void *, const u16 *, u16, u32);
IMP void *WINAPI WinHttpOpenRequest(void *, const u16 *, const u16 *, const u16 *, const u16 *, const u16 **, u32);
IMP int WINAPI WinHttpSendRequest(void *, const u16 *, u32, void *, u32, u32, u64);
IMP int WINAPI WinHttpReceiveResponse(void *, void *);
IMP int WINAPI WinHttpQueryHeaders(void *, u32, const u16 *, void *, u32 *, u32 *);
IMP int WINAPI WinHttpQueryDataAvailable(void *, u32 *);
IMP int WINAPI WinHttpReadData(void *, void *, u32, u32 *);
IMP int WINAPI WinHttpCloseHandle(void *);
IMP int WINAPI WinHttpSetOption(void *, u32, void *, u32);
IMP int WINAPI WinHttpQueryOption(void *, u32, void *, u32 *);

void *memset(void *d, int c, u64 n) { u8 *a = (u8 *)d; while (n--) *a++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { u8 *a = (u8 *)d; const u8 *b = (const u8 *)s; while (n--) *a++ = *b++; return d; }
int _fltused = 0;

#define BADH ((void *)(i64)-1)
static int g_up_quiet;   /* --quiet: no dialogs, message boxes or progress window (tests) */
/* ------------------------------------------------------------------ host: files */
static void *up_fopen_r(const char *p) { void *h = CreateFileA(p, 0x80000000u, 1 | 4, 0, 3, 0x80, 0); return h == BADH ? 0 : h; }
static void *up_fopen_w(const char *p) { void *h = CreateFileA(p, 0x40000000u, 0, 0, 2, 0x80, 0); return h == BADH ? 0 : h; }
static i64 up_fread(void *f, void *b, u64 n) { u32 got = 0; if (n > (1u << 30)) n = 1u << 30; return ReadFile(f, b, (u32)n, &got, 0) ? (i64)got : -1; }
static int up_fwrite(void *f, const void *b, u64 n) {
    const u8 *p = (const u8 *)b;
    while (n) { u32 k = n > (1u << 30) ? (1u << 30) : (u32)n, w = 0; if (!WriteFile(f, p, k, &w, 0) || w != k) return 0; p += k; n -= k; }
    return 1;
}
static int up_fclose(void *f, int flush) { int ok = 1; if (flush) ok = FlushFileBuffers(f) != 0; return CloseHandle(f) && ok; }
static int up_exists(const char *p) { u32 a = GetFileAttributesA(p); if (a == 0xFFFFFFFFu) return 0; return (a & 0x10) ? 2 : 1; }
static int up_mkdir1(const char *p) { return CreateDirectoryA(p, 0) || up_exists(p) == 2; }
static int up_move(const char *a, const char *b) { return MoveFileExA(a, b, 1 /* REPLACE_EXISTING */ | 8 /* WRITE_THROUGH */) != 0; }
static int up_delete(const char *p) { return DeleteFileA(p) != 0; }
static int up_rmdir(const char *p) { return RemoveDirectoryA(p) != 0; }
static int up_list(const char *dir, int (*cb)(void *, const char *, int), void *ctx) {
    static char names[256][262]; static int isd[256];   /* (one level at a time; up_rmtree recurses after the listing) */
    char pat[1200]; int k = 0; while (dir[k] && k < 1190) { pat[k] = dir[k]; k++; } pat[k++] = '/'; pat[k++] = '*'; pat[k] = 0;
    __attribute__((aligned(8))) u8 fd[600];
    void *h = FindFirstFileA(pat, fd); if (h == BADH) return 0;
    char (*nm)[262] = (char (*)[262])malloc(sizeof names); int *dd = (int *)malloc(sizeof isd); int n = 0;
    if (!nm || !dd) { FindClose(h); return 0; }
    do { const char *s = (const char *)(fd + 44); if ((s[0] == '.' && !s[1]) || (s[0] == '.' && s[1] == '.' && !s[2])) continue;
         int j = 0; while (s[j] && j < 261) { nm[n][j] = s[j]; j++; } nm[n][j] = 0; dd[n] = (*(u32 *)fd & 0x10) != 0; n++; } while (n < 256 && FindNextFileA(h, fd));
    FindClose(h);
    for (int i = 0; i < n; i++) if (!cb(ctx, nm[i], dd[i])) break;
    free(nm); free(dd); return 1;
}
static void *up_alloc(u64 n) { return malloc(n); }
static void up_free(void *p) { free(p); }
static void *g_logf;
static void up_log_raw(const char *s) { if (!g_logf) return; u32 n = 0; while (s[n]) n++; u32 w; WriteFile(g_logf, s, n, &w, 0); }
static void up_crash(void) { if (g_logf) FlushFileBuffers(g_logf); ExitProcess(9); }

/* ------------------------------------------------------------------ host: UI */
static void *g_prog;
static void up_pump(void) { __attribute__((aligned(8))) u8 m[64]; while (PeekMessageA(m, 0, 0, 0, 1 /* PM_REMOVE */)) { TranslateMessage(m); DispatchMessageA(m); } }
static void up_progress(const char *t) {
    if (g_up_quiet) return;
    if (!g_prog) {
        int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1), w = 460, h = 90;
        g_prog = CreateWindowExA(0x8 /* TOPMOST */ | 0x80 /* TOOLWINDOW */, "STATIC", t, 0x80000000u /* POPUP */ | 0x10000000u /* VISIBLE */ | 0x00800000u /* BORDER */ | 0x1 /* SS_CENTER */ | 0x200 /* SS_CENTERIMAGE */,
                                 (sw - w) / 2, (sh - h) / 2, w, h, 0, 0, 0, 0);
    } else SetWindowTextA(g_prog, t);
    up_pump();
}
static void up_progress_close(void) { if (g_prog) { DestroyWindow(g_prog); g_prog = 0; up_pump(); } }

static int up_http_get(const char *url, int api, int (*sink)(void *, const u8 *, u32), void *sctx, i64 *clen, char *err, int errn);
#include "ud_update_core.h"

/* ------------------------------------------------------------------ host: HTTPS GET with WinHTTP */
static void up_wide(u16 *o, int n, const char *s, int len) { int k = 0; while (k < len && s[k] && k < n - 1) { o[k] = (u16)(u8)s[k]; k++; } o[k] = 0; }
static void up_err_code(char *err, int errn, const char *what, u32 code) {
    UpSb s = {err, 0, errn}; err[0] = 0; sb_put(&s, what); sb_put(&s, " failed, WinHTTP error "); sb_num(&s, code);
    sb_put(&s, code == 12002 ? " (timed out)" : code == 12007 ? " (name not resolved: offline?)" : code == 12029 ? " (cannot connect)" :
               code == 12175 || code == 12045 || code == 12044 || code == 12037 || code == 12038 || code == 12057 ? " (secure connection / certificate refused)" :
               code == 12030 ? " (connection ended)" : "");
}
/* the host of a URL that the updater may talk to (after redirects too) */
static int up_host_ok(const char *u) {
    if (!up_starts(u, "https://")) return 0;
    const char *h = u + 8; char host[128]; int k = 0; while (h[k] && h[k] != '/' && h[k] != ':' && k < 127) { host[k] = (char)up_lc(h[k]); k++; } host[k] = 0;
    if (h[k] == ':') return 0;   /* no other ports */
    return up_eq(host, "api.github.com") || up_eq(host, "github.com") || up_eq(host, "objects.githubusercontent.com") || up_eq(host, "release-assets.githubusercontent.com");
}
static int up_http_get(const char *url, int api, int (*sink)(void *, const u8 *, u32), void *sctx, i64 *clen, char *err, int errn) {
    *clen = -1;
    if (!up_host_ok(url)) { up_cpy(err, "URL refused (not HTTPS or not a GitHub host)", errn); return 1; }
    const char *h = url + 8; int hl = 0; while (h[hl] && h[hl] != '/') hl++;
    u16 host[128], path[1024], agent[64];
    up_wide(host, 128, h, hl); up_wide(path, 1024, h + hl, 100000);
    { char a[64]; UpSb s = {a, 0, sizeof a}; a[0] = 0; sb_put(&s, "UntilDawn_PC-updater/"); sb_put(&s, UD_VERSION); up_wide(agent, 64, a, 64); }
    void *ses = WinHttpOpen(agent, 4 /* AUTOMATIC_PROXY (8.1+) */, 0, 0, 0);
    if (!ses) ses = WinHttpOpen(agent, 0 /* DEFAULT_PROXY */, 0, 0, 0);
    if (!ses) { up_err_code(err, errn, "WinHttpOpen", GetLastError()); return 1; }
    u32 prot = 0x800 | 0x2000;   /* TLS 1.2 + 1.3 */
    if (!WinHttpSetOption(ses, 84 /* SECURE_PROTOCOLS */, &prot, 4)) { prot = 0x800; WinHttpSetOption(ses, 84, &prot, 4); }
    if (api) WinHttpSetTimeouts(ses, 3000, 3000, 3000, 4000); else WinHttpSetTimeouts(ses, 5000, 5000, 10000, 20000);
    void *con = 0, *req = 0; int rc = 1;
    static const u16 get[] = {'G', 'E', 'T', 0};
    con = WinHttpConnect(ses, host, 443, 0);
    if (!con) { up_err_code(err, errn, "connect", GetLastError()); goto out; }
    req = WinHttpOpenRequest(con, get, path, 0, 0, 0, 0x00800000 /* SECURE */);
    if (!req) { up_err_code(err, errn, "request", GetLastError()); goto out; }
    { u32 off = 1 /* DISABLE_COOKIES */ | 4 /* DISABLE_AUTHENTICATION */; WinHttpSetOption(req, 63 /* DISABLE_FEATURE */, &off, 4);
      u32 pol = 1 /* REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP */; WinHttpSetOption(req, 88 /* REDIRECT_POLICY */, &pol, 4); }
    { u16 hd[200]; const char *a = api ? "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28" : "Accept: application/octet-stream";
      up_wide(hd, 200, a, 200);
      if (!WinHttpSendRequest(req, hd, 0xFFFFFFFFu, 0, 0, 0, 0)) { up_err_code(err, errn, "send", GetLastError()); goto out; } }
    if (!WinHttpReceiveResponse(req, 0)) { up_err_code(err, errn, "receive", GetLastError()); goto out; }
    { u16 fu[2048]; u32 n = sizeof fu; char fa[2048];
      if (WinHttpQueryOption(req, 34 /* URL */, fu, &n)) { int k = 0; for (; k < (int)(n / 2) && fu[k] && k < 2047; k++) fa[k] = fu[k] < 128 ? (char)fu[k] : '?'; fa[k] = 0;
          if (!up_host_ok(fa)) { UpSb s = {err, 0, errn}; err[0] = 0; sb_put(&s, "redirected to a host that is not GitHub: "); sb_put(&s, fa); goto out; } } }
    u32 status = 0, sl = 4;
    if (!WinHttpQueryHeaders(req, 19 /* STATUS_CODE */ | 0x20000000 /* NUMBER */, 0, &status, &sl, 0)) { up_err_code(err, errn, "status", GetLastError()); goto out; }
    if (status != 200) { UpSb s = {err, 0, errn}; err[0] = 0; sb_put(&s, "HTTP status "); sb_num(&s, status); sb_put(&s, status == 403 || status == 429 ? " (GitHub rate limit?)" : status == 404 ? " (no release)" : ""); goto out; }
    { u32 cl = 0, l4 = 4; if (WinHttpQueryHeaders(req, 5 /* CONTENT_LENGTH */ | 0x20000000, 0, &cl, &l4, 0)) *clen = cl; }
    static u8 buf[1 << 16];
    for (;;) {
        u32 avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { up_err_code(err, errn, "download", GetLastError()); goto out; }
        if (!avail) break;
        while (avail) {
            u32 want = avail > sizeof buf ? (u32)sizeof buf : avail, got = 0;
            if (!WinHttpReadData(req, buf, want, &got)) { up_err_code(err, errn, "download", GetLastError()); goto out; }
            if (!got) break;
            avail -= got < avail ? got : avail;
            if (!sink(sctx, buf, got)) { rc = 2; goto out; }
        }
        up_pump();
    }
    rc = 0;
out:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return rc == 2 ? 0 : rc;   /* (a sink refusal is reported by the caller) */
}


/* ------------------------------------------------------------------ host: the question */
static void *g_hook;
static i64 WINAPI up_cbt(int code, u64 wp, i64 lp) {
    if (code == 5 /* HCBT_ACTIVATE */) {
        SetDlgItemTextA((void *)wp, 6 /* IDYES */, "Install");
        SetDlgItemTextA((void *)wp, 7 /* IDNO */, "Skip version");
        SetDlgItemTextA((void *)wp, 2 /* IDCANCEL */, "Not now");
        UnhookWindowsHookEx(g_hook); g_hook = 0;
    }
    return CallNextHookEx(0, code, wp, lp);
}
static int up_ask(const UpRel *r, const char *cur) {
    static char t[2600]; UpSb s = {t, 0, sizeof t}; t[0] = 0;
    sb_put(&s, "A new version of Until Dawn (PC build) is available: "); sb_put(&s, r->ver); sb_put(&s, "  (installed: "); sb_put(&s, cur); sb_put(&s, ")\n\n");
    if (r->body[0]) {   /* the start of the release notes, plain */
        char b[700]; int k = 0, lines = 0;
        for (const char *p = r->body; *p && k < 690 && lines < 12; p++) { char ch = *p; if (ch == '\r') continue; if (ch == '#' || ch == '*' || ch == '`') continue; if (ch == '\n') lines++; b[k++] = ch; }
        b[k] = 0; sb_put(&s, b); sb_put(&s, k >= 690 || lines >= 12 ? "\n...\n\n" : "\n\n");
    }
    sb_put(&s, "Install: download");
    if (r->zip_size > 0) { sb_put(&s, " ("); sb_num(&s, (r->zip_size + (1 << 19)) >> 20); sb_put(&s, " MB)"); }
    sb_put(&s, " and install now, then start the game. Only the program files are replaced; your game files, saves, settings, "
               "shader cache and logs are not touched. The previous version is kept in Cache\\update\\backup.\n"
               "Skip version: do not ask again for this version.\nNot now: start the game; ask again next time.\n\n"
               "(Turn the check off with  update_check = off  in Config\\settings.ini.)");
    g_hook = SetWindowsHookExA(5 /* WH_CBT */, up_cbt, 0, GetCurrentThreadId());
    int r2 = MessageBoxA(0, t, "Until Dawn - update available", 3 /* YESNOCANCEL */ | 0x20 /* QUESTION */ | 0x10000 /* SETFOREGROUND */ | 0x40000 /* TOPMOST */);
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = 0; }
    return r2 == 6 ? 0 : r2 == 7 ? 2 : 1;
}
static void up_message(const char *title, const char *text, int error) {
    up_log_raw("update: message: "); up_log_raw(text); up_log_raw("\n");
    if (g_up_quiet) return;
    MessageBoxA(0, text, title, (error ? 0x30 /* EXCLAMATION */ : 0x40 /* INFORMATION */) | 0x10000 | 0x40000);
}
/* the check on its own thread: whatever WinHTTP does (DNS on a broken network ...), the game waits at most ms */
static UpCtx *g_dc; static UpRel *g_dr; static volatile int g_dres = 2;
static u32 WINAPI up_check_thread(void *p) { (void)p; int k = up_check(g_dc, g_dr); g_dres = k; return 0; }
static int up_check_deadline(UpCtx *c, UpRel *r, int ms) {
    g_dc = c; g_dr = r; g_dres = 2;
    void *th = CreateThread(0, 0, up_check_thread, 0, 0, 0);
    if (!th) { int k = up_check(c, r); return k ? 2 : 0; }
    u32 w = WaitForSingleObject(th, ms > 0 ? (u32)ms : 6000);
    if (w != 0) { up_logn("no answer within ", ms, " ms - the game starts (checked again next time)"); return 2; }   /* the thread ends with the process */
    CloseHandle(th);
    return g_dres == 0 ? 0 : 2;
}
static void up_exe_dir(char *out, int n) {
    u32 k = GetModuleFileNameA(0, out, (u32)n); if (!k || k >= (u32)n) { out[0] = '.'; out[1] = 0; return; }
    while (k && out[k - 1] != '\\' && out[k - 1] != '/') k--;
    if (k) k--; out[k] = 0;
}
static int up_log_open(const char *p) {
    void *h = CreateFileA(p, 0x40000000u, 1, 0, 4 /* OPEN_ALWAYS */, 0x80, 0); if (h == BADH) return 0;
    i64 sz = 0; GetFileSizeEx(h, &sz);
    if (sz > (1 << 20)) { CloseHandle(h); h = CreateFileA(p, 0x40000000u, 1, 0, 2 /* CREATE_ALWAYS: a log over 1 MB starts again */, 0x80, 0); if (h == BADH) return 0; }
    else { CloseHandle(h); h = CreateFileA(p, 4 /* FILE_APPEND_DATA */, 1, 0, 4, 0x80, 0); if (h == BADH) return 0; }
    g_logf = h;
    u16 st[8]; GetLocalTime(st); char t[64]; UpSb s = {t, 0, sizeof t}; t[0] = 0;
    sb_num(&s, st[0]); sb_put(&s, st[1] < 10 ? "-0" : "-"); sb_num(&s, st[1]); sb_put(&s, st[3] < 10 ? "-0" : "-"); sb_num(&s, st[3]);
    sb_put(&s, st[4] < 10 ? " 0" : " "); sb_num(&s, st[4]); sb_put(&s, st[5] < 10 ? ":0" : ":"); sb_num(&s, st[5]); sb_put(&s, st[6] < 10 ? ":0" : ":"); sb_num(&s, st[6]);
    up_log_raw("\n["); up_log_raw(t); up_log_raw("] ud_update " UD_VERSION "\n");
    return 1;
}
#include "ud_update_main.h"

static char *up_next_arg(char **p) {
    char *s = *p; while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    char *r; if (*s == '"') { r = ++s; while (*s && *s != '"') s++; } else { r = s; while (*s && *s != ' ' && *s != '\t') s++; }
    if (*s) *s++ = 0; *p = s; return r;
}
void entry(void) {
    static char *argv[64]; int argc = 0; char *cl = GetCommandLineA(), *a;
    while (argc < 63 && (a = up_next_arg(&cl))) argv[argc++] = a;
    { char ev[8]; u32 n = GetEnvironmentVariableA("UD_UPDATE_NO_DIALOGS", ev, sizeof ev); if (n && n < sizeof ev && ev[0] != '0') g_up_quiet = 1; }   /* unattended tests: nothing on screen */
    int rc = up_main(argc, argv);
    if (g_logf) { up_logn("exit code ", rc, 0); FlushFileBuffers(g_logf); CloseHandle(g_logf); }
    ExitProcess((u32)rc);
}
