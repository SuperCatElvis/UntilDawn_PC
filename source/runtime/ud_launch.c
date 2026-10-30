/*
 * ud_launch.c - the player-facing start of UntilDawn.exe (project brief section 5).
 *
 *   UntilDawn_PC\
 *     UntilDawn.exe
 *     Game\      the player's own extracted CUSA02636 install, version 01.03 (eboot.bin, all_programs.prx,
 *                sce_sys\param.sfo, localcachepink\, Movies\, audio\)
 *     Config\    settings.ini (created with defaults on the first start)
 *     Logs\      UntilDawn.log, renderer log, captures (developer mode)
 *
 * Double-clicking the exe (no command-line arguments): the settings are read, the game folder is validated, and
 * the game starts. If the game files are missing or not the supported version, a message box says exactly what is
 * wrong and where the files are expected, and nothing else happens. Nothing of the original game is shipped: the
 * manifest (ud_manifest_v103.h) holds only file names and sizes, and the two executables are recognised by their
 * SHA-256.
 *
 * Started with arguments (developer runs: e6_loader-style command lines), none of this applies.
 */
#include "ud_manifest_v103.h"

/* ---------------------------------------------------------------- SHA-256 (FIPS 180-4) */
typedef struct { u32 h[8]; u64 len; u8 buf[64]; u32 n; } Sha256;
static const u32 k_sha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha256 *s, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_sha[i] + w[i];
        u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(Sha256 *s) {
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0; s->n = 0;
}
static void sha_add(Sha256 *s, const u8 *p, u64 n) {
    s->len += n;
    while (n) {
        if (!s->n && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
        u32 c = 64 - s->n; if (c > n) c = (u32)n;
        mem_copy(s->buf + s->n, p, c); s->n += c; p += c; n -= c;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}
static void sha_hex(Sha256 *s, char out[65]) {
    u64 bits = s->len * 8; u8 pad = 0x80, z = 0;
    sha_add(s, &pad, 1);
    while (s->n != 56) sha_add(s, &z, 1);
    u8 L[8]; for (int i = 0; i < 8; i++) L[i] = (u8)(bits >> (56 - 8 * i));
    sha_add(s, L, 8);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) out[i * 8 + k] = hx[(s->h[i] >> (28 - 4 * k)) & 15];
    out[64] = 0;
}
/* SHA-256 of a whole file; 0 if it cannot be read */
static int sha_file(const char *path, char out[65]) {
    void *f = hf_open_rb(path); if (!f) return 0;
    static u8 buf[1 << 20]; Sha256 s; sha_init(&s); u64 n;
    while ((n = hf_read(f, buf, sizeof buf)) > 0) sha_add(&s, buf, n);
    hf_close(f); sha_hex(&s, out); return 1;
}

/* ---------------------------------------------------------------- the supported game version */
/* CUSA02636 (EP9000, "Until Dawn") with the 01.03 update applied. The binary patches in this program (TLS, SSE4a,
   RDTSC sites) are made for exactly these two executables. */
#define UD_EBOOT_V103_SHA "ca291314cdd5308694eb9243f1ba098e97817087e5356d78d94cf8854abebd26"
#define UD_EBOOT_V100_SHA "fdd1e3247e1f931e277336099062aa240c362b48ae0a7d68e08390dfbd743789"
#define UD_PRX_V103_SHA   "b0020e11d8236bfd0c57821b673027ad615670df5175557e74ee0b61a27ca3af"

/* ---------------------------------------------------------------- settings (Config\settings.ini) */
/* Only settings that actually change something are listed. Each one is applied in ud_apply_settings(). */
typedef struct {
    char game_dir[1024];     /* empty: <exe folder>\Game */
    int fps_limit;           /* frame-rate limit: 30, 60, 120, 144, 165 ... ; 0 = uncapped */
    int borderless;          /* 0 = window, 1 = borderless window covering the primary monitor */
    int window_w, window_h;  /* client size of the window (display_mode = windowed) */
    int developer;           /* 1: capture keys (P / O / N), status text in the window, diagnostic reports */
    int renderer_thread;     /* 0.1.4: 1 = the renderer on its own thread (asynchronous submits); 0 = on the game's thread, as in 0.1.3 */
    char gpu[64];            /* t121: "auto" / empty, a number from the log's GPU list, or part of a GPU's name */
    int present_mode;        /* t121: 0 auto, 1 fifo, 2 mailbox, 3 immediate */
    int heap_slot_mb;        /* t130: distance between the game's heap address slots: 512 (default), 64..512, or 2048 = the game's own */
    int heap_slot_reuse;     /* t131: 1 = a released heap slot is reused at once (first fit); 0 = the game's round-robin */
    int shader_prep;         /* t159: 1 = shaders of earlier sessions prepared before the game starts (e6_prep.c); 0 = off */
} UdSettings;
static UdSettings g_uds = {"", 60, 0, 1280, 720, 0, 1, "", 0, 512, 1, 1};
static int g_ud_release;     /* started as the player-facing program (no command-line arguments) */

static int ud_key_is(const char *a, const char *b) { while (*b) { if ((*a | 0x20) != (*b | 0x20)) return 0; a++; b++; } return *a == 0; }
static void ud_trim(char *s) { int n = str_len(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0; }
static int ud_atoi(const char *s) { int v = 0, neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

static const char *k_ud_ini_default =
    "; Until Dawn (native PC build) - settings\r\n"
    "; Delete this file to restore the defaults; it is recreated on the next start.\r\n"
    "\r\n"
    "; Folder with the original game files (your own extracted CUSA02636 install with the 01.03 update).\r\n"
    "; Empty = the Game folder next to UntilDawn.exe.\r\n"
    "game_dir =\r\n"
    "\r\n"
    "; Frame-rate limit: 30, 60, 120, 144, 165 (or any number), 0 = uncapped.\r\n"
    "; 60 matches the PS4 version. Higher limits work, but have not yet been checked across every scene.\r\n"
    "frame_rate_limit = 60\r\n"
    "\r\n"
    "; windowed or borderless (borderless = a window covering the whole primary monitor)\r\n"
    "display_mode = windowed\r\n"
    "\r\n"
    "; Window size for display_mode = windowed\r\n"
    "window_width = 1280\r\n"
    "window_height = 720\r\n"
    "\r\n"
    "; 1 = the renderer runs on its own thread, alongside the game (faster in most scenes).\r\n"
    "; 0 = the renderer runs on the game's thread, as in version 0.1.3 - try this if the game crashes or stutters.\r\n"
    "renderer_thread = 1\r\n"
    "\r\n"
    "; Graphics card: auto (normally the dedicated one), or a number or part of the name from the GPU list in\r\n"
    "; Logs\\UntilDawn.log (\"GPU 0: ...\", \"GPU 1: ...\"), e.g. gpu = 1 or gpu = Radeon\r\n"
    "gpu = auto\r\n"
    "\r\n"
    "; How frames are shown: auto, fifo (in step with the display), mailbox or immediate. auto uses mailbox and\r\n"
    "; switches to fifo by itself if frames take very long to reach the display (some laptops with two GPUs).\r\n"
    "present_mode = auto\r\n"
    "\r\n"
    "; Distance between the game's heap address slots in MB: 512 (default; keeps all of the game's memory where the\r\n"
    "; GPU can read it), or 2048 = the game's own layout (the GPU loses sight of memory mapped after about 30 minutes).\r\n"
    "heap_slot_mb = 512\r\n"
    "\r\n"
    "; 1 = a heap address slot the game has released is reused at once (keeps memory use level); 0 = the game's own\r\n"
    "; order (each new heap takes the next slot; memory use grows for about two hours of play).\r\n"
    "heap_slot_reuse = 1\r\n"
    "\r\n"
    "; 1 = shaders are prepared before the game starts: on the first start those listed in pipelines.udm (read\r\n"
    "; from your own game files), later also those the game needed in earlier sessions. A progress screen appears\r\n"
    "; when that takes a while (first start, graphics driver update); Esc or Circle skips it. 0 = off.\r\n"
    "shader_preparation = 1\r\n"
    "\r\n"
    "; 1 = developer mode: capture keys (Ctrl+P, Ctrl+O, Ctrl+N, F6, F11), status text, diagnostic files in Logs\\captures\r\n"
    "developer = 0\r\n";

static void ud_parse_ini(char *t) {
    while (*t) {
        char *line = t; while (*t && *t != '\n') t++; if (*t) *t++ = 0;
        ud_trim(line); while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == ';' || *line == '#' || *line == '[') continue;
        char *eq = line; while (*eq && *eq != '=') eq++; if (!*eq) continue;
        *eq = 0; char *v = eq + 1; ud_trim(line); while (*v == ' ' || *v == '\t') v++; ud_trim(v);
        if (ud_key_is(line, "game_dir")) { int n = str_len(v); if (n >= (int)sizeof g_uds.game_dir) n = sizeof g_uds.game_dir - 1; mem_copy(g_uds.game_dir, v, (u64)n); g_uds.game_dir[n] = 0; }
        else if (ud_key_is(line, "frame_rate_limit")) { int x = ud_atoi(v); g_uds.fps_limit = x < 0 ? 0 : x > 1000 ? 1000 : x; }
        else if (ud_key_is(line, "display_mode")) g_uds.borderless = ud_key_is(v, "borderless");
        else if (ud_key_is(line, "window_width")) { int x = ud_atoi(v); if (x >= 320 && x <= 16384) g_uds.window_w = x; }
        else if (ud_key_is(line, "window_height")) { int x = ud_atoi(v); if (x >= 240 && x <= 16384) g_uds.window_h = x; }
        else if (ud_key_is(line, "developer")) g_uds.developer = ud_atoi(v) != 0;
        else if (ud_key_is(line, "renderer_thread")) g_uds.renderer_thread = ud_atoi(v) != 0;
        else if (ud_key_is(line, "gpu")) { int n = str_len(v); if (n >= (int)sizeof g_uds.gpu) n = sizeof g_uds.gpu - 1; mem_copy(g_uds.gpu, v, (u64)n); g_uds.gpu[n] = 0; }
        else if (ud_key_is(line, "present_mode")) g_uds.present_mode = ud_key_is(v, "fifo") ? 1 : ud_key_is(v, "mailbox") ? 2 : ud_key_is(v, "immediate") ? 3 : 0;
        else if (ud_key_is(line, "heap_slot_reuse")) g_uds.heap_slot_reuse = ud_atoi(v) != 0;   /* t131 */
        else if (ud_key_is(line, "shader_preparation")) g_uds.shader_prep = ud_atoi(v) != 0;   /* t159 */
        else if (ud_key_is(line, "heap_slot_mb")) { int x = ud_atoi(v); g_uds.heap_slot_mb = x == 2048 ? 2048 : x < 64 ? 64 : x > 512 ? 512 : x; }   /* t130 */
    }
}
static void ud_load_settings(const char *cfg_dir) {
    char p[1100]; path_join(p, cfg_dir, "settings.ini");
    u64 n = 0; char *t = (char *)plat_read_file(p, &n);
    if (!t) {   /* first start: write the defaults */
        void *f = plat_open_write(p);
        if (f) { plat_write(f, k_ud_ini_default); plat_close(f); }
        return;
    }
    char *z = (char *)malloc(n + 1); if (!z) return;
    mem_copy(z, t, n); z[n] = 0; ud_parse_ini(z); free(z);
}

/* ---------------------------------------------------------------- validation of the game folder */
typedef struct { char *b; int n, cap; } UdMsg;
static void um(UdMsg *m, const char *s) { while (*s && m->n + 1 < m->cap) m->b[m->n++] = *s++; m->b[m->n] = 0; }
static void um_d(UdMsg *m, i64 v) { char t[24]; fmt_dec(t, v); um(m, t); }

/* param.sfo: value of a string key (0 if absent) */
static int sfo_get(const u8 *d, u64 n, const char *key, char *out, int outn) {
    if (n < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') return 0;
    u32 kt = *(const u32 *)(d + 8), dt = *(const u32 *)(d + 12), cnt = *(const u32 *)(d + 16);
    for (u32 i = 0; i < cnt && 20 + 16ull * i + 16 <= n; i++) {
        const u8 *e = d + 20 + 16 * i;
        u16 ko = *(const u16 *)e; u32 len = *(const u32 *)(e + 4), doff = *(const u32 *)(e + 12);
        if (kt + ko >= n || dt + (u64)doff + len > n) continue;
        if (!str_eq((const char *)d + kt + ko, key)) continue;
        int k = 0; while (k < (int)len && k < outn - 1 && d[dt + doff + k]) { out[k] = (char)d[dt + doff + k]; k++; }
        out[k] = 0; return 1;
    }
    return 0;
}

/* 0 = the folder holds the supported game files; otherwise msg says what is wrong */
static int ud_validate(const char *dir, UdMsg *msg) {
    char p[1200], h[65];
    if (!plat_is_dir(dir)) {
        um(msg, "The game folder was not found:\n    "); um(msg, dir);
        um(msg, "\n\nUntil Dawn needs the original game files from your own copy of the PS4 game (CUSA02636, "
                "updated to version 01.03), extracted into that folder. See README.txt next to UntilDawn.exe.");
        return 1;
    }
    path_join(p, dir, "eboot.bin");
    if (!sha_file(p, h)) {
        um(msg, "eboot.bin is missing from the game folder:\n    "); um(msg, dir);
        um(msg, "\n\nThe folder must contain the extracted PS4 game (CUSA02636) with the 01.03 update applied: "
                "eboot.bin, all_programs.prx, sce_sys\\param.sfo, localcachepink\\, Movies\\ and audio\\.");
        return 2;
    }
    if (!str_eq(h, UD_EBOOT_V103_SHA)) {
        if (str_eq(h, UD_EBOOT_V100_SHA))
            um(msg, "The game folder holds version 01.00 of Until Dawn. This program needs version 01.03: "
                    "apply the 01.03 update to the extracted game files (the update replaces eboot.bin, "
                    "all_programs.prx and 151 data files).");
        else { um(msg, "eboot.bin is not the supported version of Until Dawn (CUSA02636, version 01.03).\n\nFound SHA-256:\n    "); um(msg, h);
               um(msg, "\nexpected:\n    " UD_EBOOT_V103_SHA "\n\nThe file may be damaged, from another region or another version, "
                       "or modified (speed patches and other mods replace this file)."); }
        um(msg, "\n\nGame folder: "); um(msg, dir);
        return 3;
    }
    path_join(p, dir, "all_programs.prx");
    if (!sha_file(p, h) || !str_eq(h, UD_PRX_V103_SHA)) {
        um(msg, "all_programs.prx in the game folder is missing or is not the version that belongs to eboot.bin 01.03.\n\nGame folder: "); um(msg, dir);
        return 4;
    }
    path_join(p, dir, "sce_sys/param.sfo");
    { u64 n = 0; u8 *d = (u8 *)plat_read_file(p, &n); char id[32] = "", ver[16] = "";
      if (d) { sfo_get(d, n, "TITLE_ID", id, sizeof id); sfo_get(d, n, "APP_VER", ver, sizeof ver); }
      if (!d || !str_eq(id, "CUSA02636") || !str_eq(ver, "01.03")) {
          um(msg, "sce_sys\\param.sfo is missing or does not describe Until Dawn CUSA02636 version 01.03");
          if (d) { um(msg, " (found title "); um(msg, id[0] ? id : "?"); um(msg, ", version "); um(msg, ver[0] ? ver : "?"); um(msg, ")"); }
          um(msg, ".\n\nGame folder: "); um(msg, dir);
          return 5;
      } }
    /* every data file: present with the right size */
    int nbad = 0, nmiss = 0; u64 nfiles = sizeof g_ud_manifest / sizeof g_ud_manifest[0];
    UdMsg list = {0}; static char lb[2048]; list.b = lb; list.cap = sizeof lb; lb[0] = 0;
    for (u64 i = 0; i < nfiles; i++) {
        path_join(p, dir, g_ud_manifest[i].path);
        i64 sz = plat_file_size(p);
        if (sz == (i64)g_ud_manifest[i].size) continue;
        if (sz < 0) nmiss++; else nbad++;
        if (nmiss + nbad <= 8) { um(&list, "    "); um(&list, g_ud_manifest[i].path); um(&list, sz < 0 ? "  (missing)\n" : "  (wrong size)\n"); }
    }
    if (nmiss || nbad) {
        um(msg, "Some of the original game files are missing or incomplete (");
        um_d(msg, nmiss); um(msg, " missing, "); um_d(msg, nbad); um(msg, " with the wrong size, of ");
        um_d(msg, (i64)nfiles); um(msg, " checked):\n\n"); um(msg, lb);
        if (nmiss + nbad > 8) um(msg, "    ...\n");
        um(msg, "\nCopy the complete extracted game (with the 01.03 update) into:\n    "); um(msg, dir);
        return 6;
    }
    return 0;
}
