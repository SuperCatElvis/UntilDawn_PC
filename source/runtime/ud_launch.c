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

/* ---------------------------------------------------------------- the supported game editions (w38_rel)
   The binary patches in this program (TLS, SSE4a, RDTSC sites, ...) and every code / data address it uses are made for one
   executable: Until Dawn 01.03 as built for CUSA02636. Other regional editions are accepted when their eboot.bin is THE SAME
   PROGRAM: same size and the same bytes once three harmless, region-specific ranges are left out (MEASURED 24 Nov on
   CUSA00194 01.03, the only differences from CUSA02636 01.03 are exactly these 59 bytes):
     file 0x430 (32 B)      SELF header digest (signature data, outside the loaded program)
     file 0x188526c (28 B)  region tag + build date string ("EU2" / "EU1" + "Tue Oct 06 10:58:13 2015")
     file 0x2363c60 (20 B)  build id in the dynamic-library data
   UD_EBOOT_CODE_FP = SHA-256 of the file with those ranges zeroed (same value for CUSA02636 01.03 and CUSA00194 01.03).
   Anything else is a different program and is refused with an explanation - never run with patches made for another build. */
#define UD_EBOOT_V103_SHA "ca291314cdd5308694eb9243f1ba098e97817087e5356d78d94cf8854abebd26"
#define UD_EBOOT_V100_SHA "fdd1e3247e1f931e277336099062aa240c362b48ae0a7d68e08390dfbd743789"
#define UD_EBOOT_194_V100_SHA "5f0a12dc4bdba2700a5c22e047488382778827f227f390340c918aacffa93da2"
#define UD_EBOOT_SIZE     41400250ull
#define UD_EBOOT_CODE_FP  "57521f285989d84b807f97d15d72beafb81c891bda00eba60b6bc793e490ffd8"
static const struct { u64 off, len; } k_ud_eboot_mask[] = {{0x430, 32}, {0x188526c, 28}, {0x2363c60, 20}};

/* status: 2 = supported (tested in game by us), 1 = experimental (accepted when the code check passes; not tested by us) */
typedef struct { const char *title, *region, *eboot_sha; const UdFile *files; u64 nfiles; int status, lp_sku; } UdEdition;
static const UdEdition k_ud_editions[] = {
    {"CUSA02636", "Europe 2 (EP9000; en es pt ru pl + da fi no sv text)", UD_EBOOT_V103_SHA,
        g_ud_files_2636, sizeof g_ud_files_2636 / sizeof g_ud_files_2636[0], 2, 1},
    {"CUSA00194", "Europe 1 (EP9000; en fr de it + nl tr text)", "8e833bb6b1f782c6949b9a87000b12b460b774f73db885692d44a92a8b63be60",
        g_ud_files_194, sizeof g_ud_files_194 / sizeof g_ud_files_194[0], 2, 0},
    {"CUSA00359", "Americas (UP9000; en fr es-LA pt-BR in the game code)", "f7667bbf415edce1f6ac2a5a7d869accc4a1787990f2c776b23a4a8e69ef6d67",
        g_ud_files_359, sizeof g_ud_files_359 / sizeof g_ud_files_359[0], 1, -1},
    /* code-region table of the game (eboot 01.03): known titles whose files nobody has measured yet */
    {"CUSA01913", "UK (en)", 0, 0, 0, 1, -1},
    {"CUSA00902", "Japan (ja)", 0, 0, 0, 1, -1},
    {"CUSA01423", "Asia (en ko zh-TW zh-CN)", 0, 0, 0, 1, -1},
    {"CUSA02915", "region 9 (all 20 languages in the game code)", 0, 0, 0, 1, -1},
};
#define UD_NEDITION ((int)(sizeof k_ud_editions / sizeof k_ud_editions[0]))
static const UdEdition *g_ud_edition;        /* the player's edition, set by ud_validate (player start) or ud_detect_edition */
static int g_ud_edition_code_checked;
static u64 g_ud_nfiles_checked;        /* 1 = accepted through the code check (not a SHA-256 we list) */
static const UdEdition *ud_edition_by_title(const char *t) { for (int i = 0; i < UD_NEDITION; i++) if (str_eq(t, k_ud_editions[i].title)) return &k_ud_editions[i]; return 0; }
static const char *ud_edition_title(void) { return g_ud_edition ? g_ud_edition->title : "CUSA02636"; }
static const char *ud_tid_title(void) { return g_ud_edition ? g_ud_edition->title : 0; }
static int ud_edition_lp_sku(void) { return g_ud_edition ? g_ud_edition->lp_sku : 1; }
/* one pass over eboot.bin: plain SHA-256 and the code fingerprint (masked ranges zeroed); returns the file size or -1 */
static i64 ud_eboot_hashes(const char *path, char plain[65], char code[65]) {
    void *f = hf_open_rb(path); if (!f) return -1;
    static u8 buf[1 << 20]; Sha256 a, b; sha_init(&a); sha_init(&b); u64 n, pos = 0;
    while ((n = hf_read(f, buf, sizeof buf)) > 0) {
        sha_add(&a, buf, n);
        for (u64 k = 0; k < sizeof k_ud_eboot_mask / sizeof k_ud_eboot_mask[0]; k++) {
            u64 lo = k_ud_eboot_mask[k].off, hi = lo + k_ud_eboot_mask[k].len;
            for (u64 x = lo > pos ? lo : pos; x < hi && x < pos + n; x++) buf[x - pos] = 0;
        }
        sha_add(&b, buf, n); pos += n;
    }
    hf_close(f); sha_hex(&a, plain); sha_hex(&b, code); return (i64)pos;
}
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
    int gpu_coherence;       /* t165x: 1 = CPU reads / writes of memory the GPU wrote see the GPU's bytes (default); 0 = off */
    int gpu_coherence_off;   /* t165x: an "off" line was seen (it wins over "on" lines) */
    int halloween_hunt;      /* v0.3.0: 1 = the game's own Halloween Hunt 2015 (only its date checks see 31 October 2015); default 0 */
    /* w6_disp */
    int seen_mode, seen_rr;  /* the file has a display_mode / render_resolution line */
    int disp_auto;           /* display_system = auto: the display system (borderless + automatic resolution) is on */
    int monitor;             /* 0 = auto (primary), else the monitor's number in the log's list */
    int have_pos, win_x, win_y;   /* windowed: where the window was (saved by the program) */
    int rr_mode, rr_w, rr_h; /* render_resolution: 0 / 1 = auto, 2 = explicit */
    /* w16_rel (v0.4.0) */
    int fast_render;         /* optimised_rendering: 1 = Optimised Rendering (Fast Mode) on (default), 0 = off */
    int gpu_opt;             /* gpu_optimisations: 1 = the v0.4.0 renderer / GPU optimisations on (default), 0 = off (troubleshooting) */
    int seen_fast, seen_gpuopt, seen_fps, seen_disp;   /* the file has such a line */
    int fps_asked;           /* frame_rate_limit as written (before the 240 ceiling) */
    int fresh;               /* rc3: the file was written with the defaults at this start (its values are the defaults, not the player's) */
    /* w24_compat (not in the template yet; default off / auto) */
    int cpu_compat;          /* cpu_compat = 1: older CPUs (BMI1 / LZCNT / MOVBE / F16C replaced where missing) */
    int sys_lang1;           /* language = <name>: the PS4 system language + 1 (0 = auto: English) */
    int text_lang, speech_lang;   /* text_language / speech_language = <name>: the game's language index (0 = as the language) */
    int lang_game;           /* w33_lang: language = <name> as the game's index (for the imported-language mapping) */
    char languages_dir[1024]; /* w33_lang: languages_dir = folder of the Languages\\<code> packs (default: next to UntilDawn.exe) */
    char lang_experimental[512]; /* language_experimental = <edition>\\<lang> (EXPERIMENTAL pack, explicit only) */
    int lang_verify;         /* language_verify: 0 not set (= quick), 1 size, 2 quick, 3 full */
    char lang_overlay[1024]; /* w33_lang: language_overlay = DIR[;DIR...] - read-only folders with imported language files (default: none; not in the template) */
    /* w26_update */
    int update_check;        /* update_check: 1 = ask GitHub for a newer release at a player start (ud_update.exe); default UD_UPDATE_CHECK_DEFAULT */
    char update_skip[32];    /* update_skip_version: the release the player chose to skip ("Skip version") */
    /* w28_int (integrated development build): the merged workstreams' settings */
    int audio_late_read;     /* audio_late_read: 1 = the game's sound buffer is read after the wait (w27_audio, default 1) */
    int audio_limiter;       /* audio_limiter: 1 = peak limiter instead of hard clipping (w27_audio, default 1) */
    int gyro_cal;            /* gyro_calibration: 1 = the gyro's zero-rate offset is removed (w25_fix v2, default 1) */
    int low_vram_tex;        /* low_vram_textures: -1 auto (default; acts on cards under 12 GB only), 1 on (same condition), 0 off (w21_perf) */
    int low_driver_mem;      /* low_driver_memory: -1 auto (default, w34_mem: on when the driver's system-memory budget is small), 1 on, 0 off (w20_mem --import-gpu-only) */
    int guard_fixed;         /* guard_fixed_maps: 1 = the address guard covers the game's early fixed mappings (w20_mem, default 1; not in the template) */
    int dof;                 /* depth_of_field: 1 = as on the PS4 (default), 0 = the game's blur stages skipped (e3_loader.c patch_dof) */
    int seen_dof;
    int rs_depth_max;        /* rs_depth_max: -1 auto (default: on whenever the internal resolution is not 1080p), 1 on, 0 off (w30_visual) */
    int seen_rdm;
    int seen_alr, seen_alim, seen_gyro, seen_lvt, seen_ldm, seen_cpuc, seen_lang, seen_upd;   /* the file has such a line */
} UdSettings;
#ifndef UD_UPDATE_CHECK_DEFAULT
#define UD_UPDATE_CHECK_DEFAULT 1   /* w38_rel (user decision 25 Nov): on by default; a development build still never updates */
#endif
static UdSettings g_uds = {"", 60, 0, 1280, 720, 0, 1, "", 0, 512, 1, 1, 1, 0, 0};
static int g_ud_release;     /* started as the player-facing program (no command-line arguments) */
static int g_guard_fixed;   /* w20_mem --guard-fixed-maps (w28_int: the default; e3_main) */
static int g_ldm_mig_why;   /* w28_int: why an old low_driver_memory = 0 was kept (ud_ldm_migrate) */

static int ud_key_is(const char *a, const char *b) { while (*b) { if ((*a | 0x20) != (*b | 0x20)) return 0; a++; b++; } return *a == 0; }

static void ud_trim(char *s) { int n = str_len(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0; }
static int ud_atoi(const char *s) { int v = 0, neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }
/* w16_rel: on / off value (1, on, true, yes, enabled / 0, off, false, no, disabled; an inline ; or # comment ignored); other text: def */
/* w24_compat: language names (or the number itself) -> the game's language index 1..20 (e5_system.c) / the PS4 system id */
static const char *const g_ud_lang_names[21] = {"", "english", "french", "spanish", "german", "italian", "dutch", "portuguese", "chinese_traditional", "korean",
    "russian", "polish", "danish", "finnish", "norwegian", "swedish", "japanese", "spanish_latam", "portuguese_brazil", "turkish", "chinese_simplified"};
static const signed char g_ud_game2sys[21] = {-1, 1, 2, 3, 4, 5, 6, 7, 10, 9, 8, 16, 14, 12, 15, 13, 0, 20, 17, 19, 11};
static int ud_lang_index(const char *v) {
    char b[32]; int k = 0; while (*v == ' ' || *v == '"') v++; while (v[k] && v[k] != ';' && v[k] != '#' && v[k] != '"' && k < 31) { b[k] = v[k]; k++; } b[k] = 0; ud_trim(b);
    if (b[0] >= '0' && b[0] <= '9') { int x = ud_atoi(b); return x >= 1 && x <= 20 ? x : 0; }
    for (int i = 1; i <= 20; i++) if (ud_key_is(b, g_ud_lang_names[i])) return i;
    /* w38_rel: short codes (ISO 639-1, with region where the game has two variants); '-' or '_' both accepted */
    static const char *const codes[21] = {"", "en", "fr", "es", "de", "it", "nl", "pt", "zh-tw", "ko", "ru", "pl", "da", "fi", "no", "sv", "ja", "es-la", "pt-br", "tr", "zh-cn"};
    for (int k = 0; b[k]; k++) if (b[k] == '_') b[k] = '-';
    for (int i = 1; i <= 20; i++) if (ud_key_is(b, codes[i])) return i;
    if (ud_key_is(b, "nb") || ud_key_is(b, "norsk")) return 14;
    return 0;   /* auto / unknown */
}
static int ud_bool(const char *v, int def) {
    char b[32]; int k = 0; while (*v == ' ' || *v == '\t' || *v == '"' || *v == '\'') v++;
    while (v[k] && v[k] != ';' && v[k] != '#' && v[k] != '"' && v[k] != '\'' && k < 31) { b[k] = v[k]; k++; } b[k] = 0; ud_trim(b);
    if (!b[0]) return def;
    if (ud_key_is(b, "1") || ud_key_is(b, "on") || ud_key_is(b, "true") || ud_key_is(b, "yes") || ud_key_is(b, "enabled") || ud_key_is(b, "enable")) return 1;
    if (ud_key_is(b, "0") || ud_key_is(b, "off") || ud_key_is(b, "false") || ud_key_is(b, "no") || ud_key_is(b, "disabled") || ud_key_is(b, "disable")) return 0;
    return def;
}
enum { UD_FPS_CEILING = 240 };   /* w16_rel: the highest frame-rate limit (w14_speed: physics and cloth keep real time up to 240 fps) */

static const char *k_ud_ini_default =   /* w38_rel: the v0.5.0 defaults, in the order the user chose (25 Nov); earlier files keep their order and only get the new lines */
    "; Until Dawn (native PC build) - settings\r\n"
    "; Delete this file to restore the defaults; it is recreated on the next start.\r\n"
    "\r\n"
    "; Folder with the original game files (your own extracted CUSA02636 or CUSA00194 install with the 01.03 update).\r\n"
    "; Empty = the Game folder next to UntilDawn.exe.\r\n"
    "game_dir =\r\n"
    "\r\n"
    "; Frame-rate limit: 30, 60, 120, 240 or uncapped (uncapped can also be written as 0). 60 is the default.\r\n"
    "; uncapped still stops at 240 frames a second, so that physics and cloth keep real time; a number above 240\r\n"
    "; counts as 240. This setting is independent of optimised_rendering below.\r\n"
    "frame_rate_limit = 60\r\n"
    "\r\n"
    "; borderless (default) = the game fills the whole monitor, at the monitor's own resolution; windowed = a window.\r\n"
    "; Alt+Enter switches between the two while the game runs. The picture always keeps its 16:9 shape (black bars\r\n"
    "; on other monitor shapes, never stretched).\r\n"
    "display_mode = borderless\r\n"
    "\r\n"
    "; 1 = Halloween Hunt 2015: the game's original Halloween screen and hidden pumpkins (an event that was only active\r\n"
    "; from 28 to 31 October 2015; your Windows date is not changed). 0 = off, the normal game. Restart after changing.\r\n"
    "halloween_hunt = 0\r\n"
    "\r\n"
    "; 1 = depth of field as on the PS4 (default). 0 = off: the game's blur stages are skipped - depth of field, bokeh\r\n"
    "; and motion blur. Restart after changing.\r\n"
    "depth_of_field = 1\r\n"
    "\r\n"
    "; ---- LANGUAGE ------------------------------------------------------------------------------------------------\r\n"
    "; To change the language: write one value (the name or the code) after  language =  below, save, and restart.\r\n"
    ";   Example:  language = french     or     language = fr        auto = English\r\n"
    "; Menus, subtitles and voices change together; the game loads a language's voices with its text.\r\n"
    "; A language of YOUR edition (see the editions column) works at once. A language that only ANOTHER edition has\r\n"
    "; must be imported once from your own copy of that edition with UDLanguageImporter.exe (next to UntilDawn.exe);\r\n"
    "; without the import the game uses English. Logs\\UntilDawn.log says at start which case applies.\r\n"
    ";\r\n"
    ";   name                 code    voices       editions that contain it                    status\r\n"
    ";   english              en      yes          every edition                               verified\r\n"
    ";   spanish              es      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   portuguese           pt      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   russian              ru      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   polish               pl      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   danish               da      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   finnish              fi      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   norwegian            no      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   swedish              sv      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   french               fr      yes          CUSA00194 (Europe 1), CUSA00359 (Americas)  verified from CUSA00194\r\n"
    ";   german               de      yes          CUSA00194 (Europe 1)                        verified\r\n"
    ";   italian              it      yes          CUSA00194 (Europe 1)                        verified\r\n"
    ";   dutch                nl      text only    CUSA00194 (Europe 1)                        verified\r\n"
    ";   turkish              tr      text only    CUSA00194 (Europe 1)                        verified\r\n"
    ";   spanish_latam        es-la   yes (1)      CUSA00359 (Americas)                        EXPERIMENTAL\r\n"
    ";   portuguese_brazil    pt-br   yes (1)      CUSA00359 (Americas)                        EXPERIMENTAL\r\n"
    ";   japanese             ja      unknown      CUSA00902 (Japan)                           EXPERIMENTAL, untested\r\n"
    ";   korean               ko      unknown      CUSA01423 (Asia)                            EXPERIMENTAL, untested\r\n"
    ";   chinese_traditional  zh-tw   unknown      CUSA01423 (Asia)                            EXPERIMENTAL, untested\r\n"
    ";   chinese_simplified   zh-cn   unknown      CUSA01423 (Asia)                            EXPERIMENTAL, untested\r\n"
    "; (1) the CUSA00359 file list has text and voice files for these two languages; not yet tested in game.\r\n"
    "; EXPERIMENTAL languages are used only from an experimental import (language_experimental, see LANGUAGE_PACKS_README.txt).\r\n"
    "; Optional: text_language / speech_language split menus + subtitles from voices (same names or codes), e.g.\r\n"
    "; text_language = russian with English voices. auto = as language above.\r\n"
    "language = auto\r\n"
    "\r\n"
    "text_language = auto\r\n"
    "speech_language = auto\r\n"
    "\r\n"
    "; Optimised Rendering (Fast Mode): 1 = on (default), 0 = off.\r\n"
    "; The game goes on with its next frame while the graphics card finishes the previous one, with safety checks\r\n"
    "; that keep the game's memory consistent. Clearly faster, the same picture. It does not change the frame-rate\r\n"
    "; limit. If you see a problem, set 0 to check whether it is related.\r\n"
    "optimised_rendering = 1\r\n"
    "\r\n"
    "; Monitor: auto (the primary monitor) or its number in the monitor list of Logs\\UntilDawn.log (1, 2, ...).\r\n"
    "monitor = auto\r\n"
    "\r\n"
    "; Window size for display_mode = windowed\r\n"
    "window_width = 1280\r\n"
    "window_height = 720\r\n"
    "\r\n"
    "; Rendering resolution: auto (default) = the resolution of the monitor (or of the window): 1920x1080 on a 1080p\r\n"
    "; monitor, 2560x1440 on a 1440p monitor, 3840x2160 on a 4K monitor - never below the game's own 1920x1080, and\r\n"
    "; lowered by itself if the graphics card has too little memory (see Logs\\UntilDawn.log). Or a fixed value: 1080p,\r\n"
    "; 1440p, 2160p or WIDTHxHEIGHT, e.g. 1080p if the game runs too slowly at the monitor's resolution; the picture is\r\n"
    "; then scaled to the monitor. Restart after changing.\r\n"
    "render_resolution = auto\r\n"
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
    "; 1 = the graphics optimisations of version 0.4.0 (cheaper depth and copy work on the graphics card, faster shader\r\n"
    "; handling). Same picture. 0 = off, only for troubleshooting (some shaders may be prepared again after a change).\r\n"
    "gpu_optimisations = 1\r\n"
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
    "; when that takes a while (first start, new version, graphics driver update); Esc or Circle skips it. 0 = off.\r\n"
    "shader_preparation = 1\r\n"
    "\r\n"
    "; 1 = memory the graphics card wrote is brought back before the game's code reads or writes it (fixes the stretched\r\n"
    "; geometry and the missing lake water). 0 = off, only for troubleshooting.\r\n"
    "gpu_coherence = 1\r\n"
    "\r\n"
    "; Sound: 1 = the game's sound is read when it is played, as on the PS4 (no repeated sound buffers: removes the\r\n"
    "; crackle / spikes heard mostly at 4K and in slow scenes) (default). 0 = as version 0.4.1.\r\n"
    "audio_late_read = 1\r\n"
    "\r\n"
    "; Sound: 1 = a limiter keeps loud moments from clipping (the game's 7.1 mix, folded to stereo, can go above full\r\n"
    "; scale) (default). 0 = hard clipping as in version 0.4.1.\r\n"
    "audio_limiter = 1\r\n"
    "\r\n"
    "; Motion sensors of a DualShock 4 / DualSense: 1 = the controller's gyro offset is measured while it is still and\r\n"
    "; removed, so the \"Don't Move\" moments do not drift (default). 0 = the raw sensor values, as in version 0.4.1.\r\n"
    "gyro_calibration = 1\r\n"
    "\r\n"
    "; Graphics cards with less than 12 GB of video memory: auto (default) = texture memory in smaller blocks and\r\n"
    "; recently used textures kept (far fewer texture reloads on 6-8 GB cards). No effect on 12 GB or larger cards.\r\n"
    "; 0 = off.\r\n"
    "low_vram_textures = auto\r\n"
    "\r\n"
    "; Thin dark lines on faces at resolutions above 1080p: auto (default) = the game's tiled-lighting depth bounds\r\n"
    "; include every pixel whenever the picture is rendered at another size than 1920x1080. 1 = always, 0 = off.\r\n"
    "rs_depth_max = auto\r\n"
    "\r\n"
    "; auto (default) = on only when the graphics driver's budget for system memory is small (PCs with little RAM, e.g.\r\n"
    "; 16 GB): game memory that only the processor uses is then not handed to the graphics driver (about 1.5 GB less\r\n"
    "; driver memory). 1 = always on, 0 = always off. Restart after changing.\r\n"
    "low_driver_memory = auto\r\n"
    "\r\n"
    "; 1 = support for older processors that lack BMI1, LZCNT, MOVBE or F16C (e.g. Intel Sandy / Ivy Bridge, AMD FX):\r\n"
    "; those instructions of the game are replaced by equivalent code. Only needed if the program says the processor is\r\n"
    "; not supported (Compatibility Test: compat\\ud_compat.exe). 0 = off (default).\r\n"
    "cpu_compat = 0\r\n"
    "\r\n"
    "; 1 = check GitHub for a newer release at start (default; you are asked before anything is installed, and your game files,\r\n"
    "; saves, settings and language packs are never touched). 0 = off.\r\n"
    "update_check = 1\r\n"
    "\r\n"
    "; 1 = developer mode: capture keys (Ctrl+P, Ctrl+O, Ctrl+N, F6, F11), status text, diagnostic files in Logs\\captures\r\n"
    "developer = 0\r\n";

/* w6_disp: "auto", "1080p" / "1440p" / "2160p" / "4k", "WIDTHxHEIGHT" -> mode 1 (auto) or 2 (w, h); anything else: auto */
/* w6_disp: the display lines of the settings file once the display system is the default (UD_DISPLAY_DEFAULT_ON); until
   then it is on with  display_system = auto  in settings.ini (or --display-auto) and the template is the old one */
#if 0
    "; borderless (default) = the game fills the whole monitor, at the monitor's own resolution; windowed = a window.\r\n"
    "; Alt+Enter switches between the two while the game runs. The picture always keeps its 16:9 shape (black bars\r\n"
    "; on other monitor shapes, never stretched).\r\n"
    "display_mode = borderless\r\n"
    "\r\n"
    "; Monitor: auto (the primary monitor) or its number in the monitor list of Logs\\UntilDawn.log (1, 2, ...).\r\n"
    "monitor = auto\r\n"
    "\r\n"
    "; Window size for display_mode = windowed\r\n"
    "window_width = 1280\r\n"
    "window_height = 720\r\n"
    "\r\n"
    "; Rendering resolution: auto = the resolution of the monitor (or of the window): 1920x1080 on a 1080p monitor,\r\n"
    "; 2560x1440 on a 1440p monitor, 3840x2160 on a 4K monitor - never below the game's own 1920x1080, and lowered by\r\n"
    "; itself if the graphics card has too little memory (see Logs\\UntilDawn.log). Or a fixed value: 1080p, 1440p,\r\n"
    "; 2160p or WIDTHxHEIGHT, e.g. 1080p if the game runs too slowly at the monitor's resolution; the picture is then\r\n"
    "; scaled to the monitor. Restart after changing.\r\n"
    "render_resolution = auto\r\n"
    "\r\n"
#endif
static void ud_parse_res(const char *v, int *mode, int *w, int *h) {
    *mode = 1; *w = *h = 0;
    while (*v == ' ') v++;
    if (ud_key_is(v, "4k")) { *mode = 2; *w = 3840; *h = 2160; return; }
    int a = 0, n = 0; while (v[n] >= '0' && v[n] <= '9') { a = a * 10 + (v[n] - '0'); n++; if (a > 100000) return; }
    if (!n) return;
    if ((v[n] | 0x20) == 'p' && !v[n + 1]) { if (a >= 240 && a <= 8640) { *mode = 2; *h = a; *w = (a * 16 + 8) / 9; } return; }
    if ((v[n] | 0x20) == 'x') { int b = ud_atoi(v + n + 1); if (a >= 320 && a <= 16384 && b >= 240 && b <= 16384) { *mode = 2; *w = a; *h = b; } }
}
static char g_ud_ini_path[1100];
/* w6_disp: one "key = value" line of settings.ini changed in place (the first such line; comments and everything else stay
   as they are), or added at the end with its comment when the file has no such line */
static void ud_ini_set(const char *key, const char *val, const char *comment) {
    if (!g_ud_ini_path[0]) return;
    u64 n = 0; char *t = (char *)plat_read_file(g_ud_ini_path, &n); if (!t) return;
    int kl = str_len(key), vl_ = str_len(val), cl = comment ? str_len(comment) : 0;
    char *o = (char *)malloc(n + (u64)kl + (u64)vl_ + (u64)cl + 64); if (!o) return;
    u64 w = 0, i = 0; int done = 0;
    while (i < n) {
        u64 ls = i; while (i < n && t[i] != '\n') i++; u64 le = i; if (i < n) i++;   /* line [ls, le), then its newline */
        u64 a = ls; while (a < le && (t[a] == ' ' || t[a] == '\t')) a++;
        int hit = 0;
        if (!done && a + (u64)kl <= le) { hit = 1; for (int k = 0; k < kl; k++) if ((t[a + k] | 0x20) != (key[k] | 0x20)) { hit = 0; break; }
            if (hit) { u64 b = a + (u64)kl; while (b < le && (t[b] == ' ' || t[b] == '\t')) b++; if (b >= le || t[b] != '=') hit = 0; } }
        if (hit) { mem_copy(o + w, key, (u64)kl); w += (u64)kl; o[w++] = ' '; o[w++] = '='; o[w++] = ' '; mem_copy(o + w, val, (u64)vl_); w += (u64)vl_; o[w++] = '\r'; o[w++] = '\n'; done = 1; }
        else { mem_copy(o + w, t + ls, i - ls); w += i - ls; }
    }
    if (!done) { if (w && o[w - 1] != '\n') { o[w++] = '\r'; o[w++] = '\n'; }
        if (cl) { o[w++] = '\r'; o[w++] = '\n'; mem_copy(o + w, comment, (u64)cl); w += (u64)cl; }
        mem_copy(o + w, key, (u64)kl); w += (u64)kl; o[w++] = ' '; o[w++] = '='; o[w++] = ' '; mem_copy(o + w, val, (u64)vl_); w += (u64)vl_; o[w++] = '\r'; o[w++] = '\n'; }
    o[w] = 0;
    void *f = plat_open_write(g_ud_ini_path); if (f) { plat_write(f, o); plat_close(f); }
    free(o);
}
static void ud_parse_ini(char *t) {
    while (*t) {
        char *line = t; while (*t && *t != '\n') t++; if (*t) *t++ = 0;
        ud_trim(line); while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == ';' || *line == '#' || *line == '[') continue;
        char *eq = line; while (*eq && *eq != '=') eq++; if (!*eq) continue;
        *eq = 0; char *v = eq + 1; ud_trim(line); while (*v == ' ' || *v == '\t') v++; ud_trim(v);
        if (ud_key_is(line, "game_dir")) { int n = str_len(v); if (n >= (int)sizeof g_uds.game_dir) n = sizeof g_uds.game_dir - 1; mem_copy(g_uds.game_dir, v, (u64)n); g_uds.game_dir[n] = 0; }
        else if (ud_key_is(line, "frame_rate_limit")) {   /* w16_rel: 30 / 60 / 120 / 240 / uncapped (= 0); any other number up to 240 still works, above 240 counts as 240 */
            int x = ud_atoi(v); if (ud_key_is(v, "uncapped") || ud_key_is(v, "unlimited") || ud_key_is(v, "none") || ud_key_is(v, "off")) x = 0;
            else if (v[0] == '-') x = 0;   /* (a negative number: uncapped, as before) */
            else if (!(v[0] >= '0' && v[0] <= '9')) x = 60;   /* (unreadable: the default) */
            if (x < 0) x = 0; if (x > 100000) x = 100000;
            g_uds.fps_asked = x; g_uds.fps_limit = x > UD_FPS_CEILING ? UD_FPS_CEILING : x; g_uds.seen_fps = 1; }
        else if (ud_key_is(line, "optimised_rendering") || ud_key_is(line, "optimized_rendering") || ud_key_is(line, "fast_mode")) { g_uds.fast_render = ud_bool(v, 1); g_uds.seen_fast = 1; }   /* w16_rel */
        else if (ud_key_is(line, "gpu_optimisations") || ud_key_is(line, "gpu_optimizations")) { g_uds.gpu_opt = ud_bool(v, 1); g_uds.seen_gpuopt = 1; }   /* w16_rel */
        else if (ud_key_is(line, "display_mode")) { g_uds.borderless = ud_key_is(v, "borderless") || ud_key_is(v, "fullscreen"); g_uds.seen_mode = 1; }
        else if (ud_key_is(line, "display_system")) { g_uds.disp_auto = !(ud_key_is(v, "off") || ud_key_is(v, "legacy") || ud_key_is(v, "old") || (v[0] == '0' && !v[1])); g_uds.seen_disp = 1; }   /* w6_disp; w16_rel: on unless off / legacy / 0 (v0.4.0: the default) */
        else if (ud_key_is(line, "monitor")) { int x = ud_atoi(v); g_uds.monitor = x >= 1 && x <= 8 ? x : 0; }   /* w6_disp */
        else if (ud_key_is(line, "window_x")) { g_uds.win_x = ud_atoi(v); g_uds.have_pos |= 1; }
        else if (ud_key_is(line, "window_y")) { g_uds.win_y = ud_atoi(v); g_uds.have_pos |= 2; }
        else if (ud_key_is(line, "render_resolution")) { g_uds.seen_rr = 1; ud_parse_res(v, &g_uds.rr_mode, &g_uds.rr_w, &g_uds.rr_h); }
        else if (ud_key_is(line, "window_width")) { int x = ud_atoi(v); if (x >= 320 && x <= 16384) g_uds.window_w = x; }
        else if (ud_key_is(line, "window_height")) { int x = ud_atoi(v); if (x >= 240 && x <= 16384) g_uds.window_h = x; }
        else if (ud_key_is(line, "developer")) g_uds.developer = ud_atoi(v) != 0;
        else if (ud_key_is(line, "cpu_compat")) { g_uds.cpu_compat = ud_bool(v, 0); g_uds.seen_cpuc = 1; }   /* w24_compat */
        else if (ud_key_is(line, "language")) { int x = ud_lang_index(v); g_uds.sys_lang1 = x ? g_ud_game2sys[x] + 1 : 0; g_uds.lang_game = x; g_uds.seen_lang = 1; }   /* w24_compat */
        else if (ud_key_is(line, "audio_late_read")) { g_uds.audio_late_read = ud_bool(v, 1); g_uds.seen_alr = 1; }   /* w28_int (w27_audio) */
        else if (ud_key_is(line, "audio_limiter")) { g_uds.audio_limiter = ud_bool(v, 1); g_uds.seen_alim = 1; }   /* w28_int (w27_audio) */
        else if (ud_key_is(line, "gyro_calibration")) { g_uds.gyro_cal = ud_bool(v, 1); g_uds.seen_gyro = 1; }   /* w28_int (w25_fix) */
        else if (ud_key_is(line, "low_vram_textures")) { char b[16]; int k = 0; while (v[k] && v[k] != ' ' && v[k] != ';' && v[k] != '#' && k < 15) { b[k] = v[k]; k++; } b[k] = 0;
            g_uds.low_vram_tex = ud_key_is(b, "auto") ? -1 : ud_bool(v, -1); g_uds.seen_lvt = 1; }
        else if (ud_key_is(line, "rs_depth_max")) { char b[16]; int k = 0; while (v[k] && v[k] != ' ' && v[k] != ';' && v[k] != '#' && k < 15) { b[k] = v[k]; k++; } b[k] = 0;
            g_uds.rs_depth_max = ud_key_is(b, "auto") ? -1 : ud_bool(v, -1); g_uds.seen_rdm = 1; }   /* w30_visual: auto / 1 / 0 */   /* w28_int (w21_perf): auto / 1 / 0 */
        else if (ud_key_is(line, "depth_of_field")) { g_uds.dof = ud_bool(v, 1); g_uds.seen_dof = 1; }   /* w28_int */
        else if (ud_key_is(line, "low_driver_memory")) { char b[16]; int k = 0; while (v[k] && v[k] != ' ' && v[k] != ';' && v[k] != '#' && k < 15) { b[k] = v[k]; k++; } b[k] = 0;
            g_uds.low_driver_mem = ud_key_is(b, "auto") ? -1 : ud_bool(v, -1); g_uds.seen_ldm = 1; }   /* w34_mem: auto / 1 / 0 (unknown = auto) */   /* w28_int (w20_mem) */
        else if (ud_key_is(line, "guard_fixed_maps")) g_uds.guard_fixed = ud_bool(v, 1);   /* w28_int (w20_mem; troubleshooting only, not in the template) */
        else if (ud_key_is(line, "text_language")) g_uds.text_lang = ud_lang_index(v);
        else if (ud_key_is(line, "speech_language")) g_uds.speech_lang = ud_lang_index(v);
        else if (ud_key_is(line, "languages_dir") || ud_key_is(line, "language_experimental")) { char *dst = ud_key_is(line, "languages_dir") ? g_uds.languages_dir : g_uds.lang_experimental; int cap = ud_key_is(line, "languages_dir") ? 1024 : 512;
            int n = str_len(v); while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r')) n--; if (n >= cap) n = 0; mem_copy(dst, v, (u64)n); dst[n] = 0; }   /* w33_lang */
        else if (ud_key_is(line, "language_verify")) g_uds.lang_verify = ud_key_is(v, "full") ? 3 : ud_key_is(v, "size") ? 1 : 2;   /* w33_lang */
        else if (ud_key_is(line, "language_overlay")) { int n = str_len(v); while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r')) n--;
            if (n >= (int)sizeof g_uds.lang_overlay) n = 0; mem_copy(g_uds.lang_overlay, v, (u64)n); g_uds.lang_overlay[n] = 0; }   /* w33_lang */
        else if (ud_key_is(line, "renderer_thread")) g_uds.renderer_thread = ud_atoi(v) != 0;
        else if (ud_key_is(line, "gpu")) { int n = str_len(v); if (n >= (int)sizeof g_uds.gpu) n = sizeof g_uds.gpu - 1; mem_copy(g_uds.gpu, v, (u64)n); g_uds.gpu[n] = 0; }
        else if (ud_key_is(line, "present_mode")) g_uds.present_mode = ud_key_is(v, "fifo") ? 1 : ud_key_is(v, "mailbox") ? 2 : ud_key_is(v, "immediate") ? 3 : 0;
        else if (ud_key_is(line, "halloween_hunt")) g_uds.halloween_hunt = ud_atoi(v) == 1 || ud_key_is(v, "on") || ud_key_is(v, "true") || ud_key_is(v, "yes");   /* t167 */
        else if (ud_key_is(line, "heap_slot_reuse")) g_uds.heap_slot_reuse = ud_atoi(v) != 0;   /* t131 */
        else if (ud_key_is(line, "gpu_coherence")) {   /* t165x: on / true / yes / 1 = on; 0 / off / false / no = off (an inline ; or # comment
                                                           ignored). Review 6 X2-2: an "off" line anywhere wins - the switch must work when added above or below the default line */
            char vb[32]; int k = 0; while (v[k] && v[k] != ';' && v[k] != '#' && !(v[k] == '/' && v[k + 1] == '/') && k < 31) { vb[k] = v[k]; k++; } vb[k] = 0; ud_trim(vb);
            char *q = vb; if (*q == '"' || *q == '\'') { q++; int m = str_len(q); if (m && (q[m - 1] == '"' || q[m - 1] == '\'')) q[m - 1] = 0; ud_trim(q); while (*q == ' ' || *q == '\t') q++; }   /* (review 7 N4: quotes, // comments, 00, n, disable) */
            int dz = *q != 0; for (char *c = q; *c; c++) if (*c != '0') dz = 0;
            int off = dz || ud_key_is(q, "off") || ud_key_is(q, "false") || ud_key_is(q, "no") || ud_key_is(q, "n") || ud_key_is(q, "disable") || ud_key_is(q, "disabled");
            if (off) g_uds.gpu_coherence_off = 1;
            g_uds.gpu_coherence = !g_uds.gpu_coherence_off; }
        else if (ud_key_is(line, "shader_preparation")) { int pv = ud_atoi(v); g_uds.shader_prep = pv == 2 ? 2 : pv != 0; }   /* t159; t164: 2 = background */
        else if (ud_key_is(line, "update_check")) { g_uds.update_check = ud_bool(v, UD_UPDATE_CHECK_DEFAULT); g_uds.seen_upd = 1; }   /* w26_update */
        else if (ud_key_is(line, "update_skip_version")) { int n = 0; while (v[n] && v[n] != ' ' && v[n] != ';' && n < (int)sizeof g_uds.update_skip - 1) { g_uds.update_skip[n] = v[n]; n++; } g_uds.update_skip[n] = 0; }
        else if (ud_key_is(line, "heap_slot_mb")) { int x = ud_atoi(v); g_uds.heap_slot_mb = x == 2048 ? 2048 : x < 64 ? 64 : x > 512 ? 512 : x; }   /* t130 */
    }
}
static void ud_load_settings(const char *cfg_dir) {
    char p[1100]; path_join(p, cfg_dir, "settings.ini"); { int k = 0; while (p[k] && k < (int)sizeof g_ud_ini_path - 1) { g_ud_ini_path[k] = p[k]; k++; } g_ud_ini_path[k] = 0; }
    g_uds.disp_auto = 1; g_uds.fast_render = 1; g_uds.gpu_opt = 1; g_uds.fps_asked = 60; g_uds.update_check = UD_UPDATE_CHECK_DEFAULT;   /* w16_rel: the v0.4.0 defaults (a line in the file wins) */
    g_uds.audio_late_read = 1; g_uds.audio_limiter = 1; g_uds.gyro_cal = 1; g_uds.low_vram_tex = -1; g_uds.low_driver_mem = -1; g_uds.guard_fixed = 1; g_uds.dof = 1; g_uds.rs_depth_max = -1;   /* w28_int: the merged defaults (a file without these lines gets them) */
    u64 n = 0; char *t = (char *)plat_read_file(p, &n);
    if (!t) {   /* first start: write the defaults */
        void *f = plat_open_write(p);
        if (f) { plat_write(f, k_ud_ini_default); plat_close(f); }
        say("settings: no Config\\settings.ini - written with the defaults (first start)\n");
        n = (u64)str_len(k_ud_ini_default); t = (char *)k_ud_ini_default; g_uds.fresh = 1;   /* w16_rel: read like any other file (its display lines are the new ones: no migration) */
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

/* param.sfo of a game folder: TITLE_ID / APP_VER (0 if unreadable) */
static int ud_read_sfo(const char *dir, char id[32], char ver[16]) {
    char p[1200]; path_join(p, dir, "sce_sys/param.sfo"); u64 n = 0; u8 *d = (u8 *)plat_read_file(p, &n);
    id[0] = ver[0] = 0; if (!d) return 0;
    sfo_get(d, n, "TITLE_ID", id, 32); sfo_get(d, n, "APP_VER", ver, 16); return 1;
}
/* developer runs (no validation): the edition from param.sfo only, for the language rules and add-on names */
static void ud_detect_edition(const char *dir) {
    char id[32], ver[16]; if (g_ud_edition || !ud_read_sfo(dir, id, ver)) return;
    g_ud_edition = ud_edition_by_title(id);
}
static void ud_supported_list(UdMsg *msg) {
    um(msg, "\n\nSupported: Until Dawn (2015) for PS4 with update 01.03 - CUSA02636 or CUSA00194 (Europe); CUSA00359 (Americas) "
            "is experimental. See COMPATIBILITY.txt next to UntilDawn.exe.");
}
/* 0 = the folder holds a usable game; otherwise msg says what is wrong */
static int ud_validate(const char *dir, UdMsg *msg) {
    char p[1200], h[65], code[65];
    if (!plat_is_dir(dir)) {
        um(msg, "The game folder was not found:\n    "); um(msg, dir);
        um(msg, "\n\nUntil Dawn needs the original game files from your own copy of the PS4 game, "
                "updated to version 01.03, extracted into that folder. See README.txt next to UntilDawn.exe.");
        ud_supported_list(msg);
        return 1;
    }
    path_join(p, dir, "eboot.bin");
    i64 esz = ud_eboot_hashes(p, h, code);
    if (esz < 0) {
        um(msg, "eboot.bin is missing from the game folder:\n    "); um(msg, dir);
        um(msg, "\n\nThe folder must contain the extracted PS4 game with the 01.03 update applied: "
                "eboot.bin, all_programs.prx, sce_sys\\param.sfo, localcachepink\\, Movies\\ and audio\\.");
        return 2;
    }
    char id[32], ver[16]; int have_sfo = ud_read_sfo(dir, id, ver);
    const UdEdition *E = 0;
    for (int i = 0; i < UD_NEDITION; i++) if (k_ud_editions[i].eboot_sha && str_eq(h, k_ud_editions[i].eboot_sha)) { E = &k_ud_editions[i]; break; }
    int same_code = (u64)esz == UD_EBOOT_SIZE && str_eq(code, UD_EBOOT_CODE_FP);
    if (!same_code) {
        if (str_eq(h, UD_EBOOT_V100_SHA) || str_eq(h, UD_EBOOT_194_V100_SHA) || (have_sfo && ver[0] && !str_eq(ver, "01.03")))
            { um(msg, "The game folder holds version "); um(msg, ver[0] ? ver : "01.00"); um(msg, " of Until Dawn. This program needs version 01.03: "
                    "apply the 01.03 update to the extracted game files (the update replaces eboot.bin, all_programs.prx and a number of data files)."); }
        else {
            um(msg, "eboot.bin is not a version of Until Dawn that this program can run");
            if (E) { um(msg, " ("); um(msg, E->title); um(msg, ": its program code differs from the 01.03 build this program is made for)"); }
            um(msg, ".\n\nThis program changes the game's own code at fixed places, so it only runs the exact 01.03 program. Found:\n    title ");
            um(msg, id[0] ? id : "?"); um(msg, ", version "); um(msg, ver[0] ? ver : "?"); um(msg, "\n    SHA-256 "); um(msg, h);
            um(msg, "\n    code fingerprint "); um(msg, code);
            um(msg, "\n\nThe file may be damaged, from another version, or modified (speed patches and other mods replace this file). "
                    "If this is an unmodified 01.03 dump of another regional edition, please report the lines above.");
        }
        ud_supported_list(msg);
        um(msg, "\n\nGame folder: "); um(msg, dir);
        return 3;
    }
    /* the same program: which edition? (a listed SHA-256, else the title in param.sfo) */
    if (!E && have_sfo) E = ud_edition_by_title(id);
    char eh[65]; mem_copy(eh, h, sizeof eh);   /* the eboot's SHA-256 (h is reused for the prx) */
    path_join(p, dir, "all_programs.prx");
    if (!sha_file(p, h) || !str_eq(h, UD_PRX_V103_SHA)) {
        um(msg, "all_programs.prx in the game folder is missing or is not the version that belongs to eboot.bin 01.03.\n\nGame folder: "); um(msg, dir);
        return 4;
    }
    if (!have_sfo || !str_eq(ver, "01.03") || (E && !str_eq(id, E->title)) || (!E && !id[0])) {
        um(msg, "sce_sys\\param.sfo is missing or does not describe Until Dawn version 01.03");
        if (E && have_sfo && id[0] && !str_eq(id, E->title)) { um(msg, " of "); um(msg, E->title); um(msg, ", the edition its eboot.bin belongs to"); }
        if (have_sfo) { um(msg, " (found title "); um(msg, id[0] ? id : "?"); um(msg, ", version "); um(msg, ver[0] ? ver : "?"); um(msg, ")"); }
        um(msg, ".\n\nGame folder: "); um(msg, dir);
        return 5;
    }
    g_ud_edition = E; g_ud_edition_code_checked = !E || !E->eboot_sha || !str_eq(eh, E->eboot_sha);
    /* every data file: the files all editions share, then the edition's own (unknown edition: shared files only) */
    int nbad = 0, nmiss = 0; u64 nfiles = 0;
    UdMsg list = {0}; static char lb[2048]; list.b = lb; list.cap = sizeof lb; lb[0] = 0;
    for (int part = 0; part < 2; part++) {
        const UdFile *F = part ? (E ? E->files : 0) : g_ud_common_files;
        u64 nf = part ? (E ? E->nfiles : 0) : sizeof g_ud_common_files / sizeof g_ud_common_files[0];
        for (u64 i = 0; F && i < nf; i++) {
            path_join(p, dir, F[i].path); nfiles++;
            i64 sz = plat_file_size(p);
            if (sz == (i64)F[i].size) continue;
            if (sz < 0) nmiss++; else nbad++;
            if (nmiss + nbad <= 8) { um(&list, "    "); um(&list, F[i].path); um(&list, sz < 0 ? "  (missing)\n" : "  (wrong size)\n"); }
        }
    }
    g_ud_nfiles_checked = nfiles;
    if (nmiss || nbad) {
        um(msg, "Some of the original game files are missing or incomplete (");
        um_d(msg, nmiss); um(msg, " missing, "); um_d(msg, nbad); um(msg, " with the wrong size, of ");
        um_d(msg, (i64)nfiles); um(msg, " checked for "); um(msg, id); um(msg, "):\n\n"); um(msg, lb);
        if (nmiss + nbad > 8) um(msg, "    ...\n");
        um(msg, "\nCopy the complete extracted game (with the 01.03 update) into:\n    "); um(msg, dir);
        return 6;
    }
    return 0;
}

/* ---------------------------------------------------------------- w6_disp: display mode and automatic resolution
   The player no longer chooses a percentage. The display system (a player start, or --display-auto / any --display-* /
   --monitor / --window / --sim-monitor option in a developer run) decides:
     display mode   borderless (default: a window covering the chosen monitor at its own resolution) | windowed
     monitor        auto = the primary one, or a number from the list in the log; a number that does not exist: primary
     picture area   the largest 16:9 rectangle of the window (pillarbox on 21:9 / 32:9, letterbox on 16:10 / 4:3; the
                    presentation never stretches)
     internal resolution (3D, interface, text, movies' target): auto = the picture area's, or render_resolution /
                    --render-res. The renderer's scaled targets support the ratios 1, 5/4, 4/3, 3/2, 7/4, 2 ... 4 of
                    1920x1080 (the pixel mapping of gcn2spv.c is verified for these): the smallest of them that is not
                    below the wanted height is used and the picture is filtered down to the area when it is larger
                    (2304x1296 -> 2400x1350). Never below 1 (1920x1080): a smaller window or monitor shows the 1080p
                    picture filtered down, as every version did. The ratio is fixed for the session (shaders are
                    translated per ratio and the interface's viewport is patched into the game's code at start): moving
                    the window to another monitor or Alt+Enter changes the presentation only.
     video memory   the screen-sized targets take about 950 MB x ratio^2 (measured: cave sessions 60_05 / res133_03 /
                    res200_02); a ratio is used only if that plus 1.5 GB of textures fits the image budget the card
                    will get (cards under 12 GB: the --vram-fit budget, else half the card) - otherwise the next lower
                    ratio, said in the log. --no-vram-step: off.
   --render-scale N stays the developer switch: given, it decides the ratio and nothing here changes it.
   Saving (player start, or --display-save): display_mode on Alt+Enter, the window's size and place when the user changed
   them, the monitor when a borderless window was moved to another one. */
static int g_dopt_on, g_dopt_save, g_dopt_mode = -1, g_dopt_mon = -1, g_dopt_cw, g_dopt_ch, g_dopt_simw, g_dopt_simh, g_dopt_no_vram;
static int g_rr_mode, g_rr_w, g_rr_h;   /* --render-res: 0 not given, 1 auto, 2 explicit */
static int g_disp_start_mon = -1, g_disp_start_mode;
static const int k_disp_pct[] = {100, 125, 133, 150, 175, 200, 225, 250, 275, 300, 325, 350, 375, 400};
enum { DISP_NPCT = sizeof k_disp_pct / sizeof k_disp_pct[0] };
static int disp_pct_h(int pct) { return pct == 133 ? 1440 : (1080 * pct + 99) / 100; }
static int disp_pct_w(int pct) { return pct == 133 ? 2560 : (1920 * pct + 99) / 100; }
static int disp_pick(int th) { for (int i = 0; i < DISP_NPCT; i++) if (disp_pct_h(k_disp_pct[i]) >= th) return i; return DISP_NPCT - 1; }
static u64 disp_need_mb(int pct) { u64 n = pct == 133 ? 16 : (u64)pct * pct, d = pct == 133 ? 9 : 10000; return 950 * n / d; }
static void disp_fmt(char *o, int v) { char t[24]; fmt_dec(t, v); int k = 0; while (t[k]) { o[k] = t[k]; k++; } o[k] = 0; }
static void disp_save(void) {   /* window thread: something worth keeping changed */
    say("display: "); say(g_disp.borderless ? "borderless on monitor " : "windowed "); if (g_disp.borderless) say_dec(g_disp.mon + 1); else { say_dec(g_disp.w); say("x"); say_dec(g_disp.h); say(" at "); say_dec(g_disp.x); say(","); say_dec(g_disp.y); }
    say(g_dopt_save ? " (saved to Config\\settings.ini)\n" : " (not saved: not a player start)\n");
    if (!g_dopt_save) return;
    char v[24];
    if (g_disp.borderless != g_disp_start_mode) { ud_ini_set("display_mode", g_disp.borderless ? "borderless" : "windowed", 0); g_disp_start_mode = g_disp.borderless; }
    if (g_disp.borderless) { if (g_disp.mon != g_disp_start_mon) { disp_fmt(v, g_disp.mon + 1); ud_ini_set("monitor", v, 0); g_disp_start_mon = g_disp.mon; } }
    else { disp_fmt(v, g_disp.w); ud_ini_set("window_width", v, 0); disp_fmt(v, g_disp.h); ud_ini_set("window_height", v, 0);
           disp_fmt(v, g_disp.x); ud_ini_set("window_x", v, "; Where the window was (written by the program)\r\n"); disp_fmt(v, g_disp.y); ud_ini_set("window_y", v, 0); }
}
/* after the options are parsed, before the game's code is patched and the window is made */
static void disp_setup(int replay) {
    int rr_mode = g_rr_mode ? g_rr_mode : g_uds.rr_mode, rr_w = g_rr_mode ? g_rr_w : g_uds.rr_w, rr_h = g_rr_mode ? g_rr_h : g_uds.rr_h;
    int forced = g_rs_pct > 100;   /* --render-scale: the developer switch decides */
    if (replay || !g_dopt_on) {   /* no display system: only an explicit internal resolution, as --render-scale would give it */
        if (g_rr_mode == 2 && !forced) { int th = rr_h < (rr_w * 9 + 8) / 16 ? rr_h : (rr_w * 9 + 8) / 16; g_rs_pct = k_disp_pct[disp_pick(th)];
            say("display: internal resolution "); say_dec(disp_pct_w(g_rs_pct)); say("x"); say_dec(disp_pct_h(g_rs_pct)); say(" (--render-res "); say_dec(rr_w); say("x"); say_dec(rr_h); say(")\n"); }
        return;
    }
    g_disp.borderless = g_dopt_mode >= 0 ? g_dopt_mode : g_ud_release ? (g_uds.seen_mode ? g_uds.borderless : 1) : 1;
    g_disp.mon_req = g_dopt_mon >= 0 ? g_dopt_mon : g_ud_release ? g_uds.monitor : 0;
    g_disp.cw_req = g_dopt_cw ? g_dopt_cw : g_ud_release ? g_uds.window_w : 1280; g_disp.ch_req = g_dopt_ch ? g_dopt_ch : g_ud_release ? g_uds.window_h : 720;
    if (g_ud_release && g_uds.have_pos == 3 && !g_dopt_cw && g_dopt_mon < 0) { g_disp.have_pos = 1; g_disp.px = g_uds.win_x; g_disp.py = g_uds.win_y; }
    g_disp.sim_w = g_dopt_simw; g_disp.sim_h = g_dopt_simh;
    if (!plat_disp_resolve()) { say("display: no monitor information from Windows - the window as the settings say, internal resolution 1920x1080\n"); g_win_borderless = g_disp.borderless; g_win_cw = g_disp.cw_req; g_win_ch = g_disp.ch_req; return; }
    g_disp.on = 1; g_disp_start_mon = g_disp.mon_req ? g_disp.mon : -1; g_disp_start_mode = g_disp.borderless; g_disp_changed_cb = disp_save;
    g_win_borderless = g_disp.borderless; g_win_cw = g_disp.w; g_win_ch = g_disp.h; g_win_rs_mon_w = g_mon[g_disp.mon].w; g_win_rs_mon_h = g_mon[g_disp.mon].ht;
    say("display: "); say_dec(g_nmon); say(g_nmon == 1 ? " monitor" : " monitors"); say(g_disp.dpi_mode == 2 ? " (DPI-aware per monitor)" : g_disp.dpi_mode == 1 ? " (DPI-aware, system)" : " (NOT DPI-aware)"); say(":");
    for (int i = 0; i < g_nmon; i++) { say(i ? "; " : " "); say_dec(i + 1); say(": "); say_dec(g_mon[i].w); say("x"); say_dec(g_mon[i].ht); say(" at "); say_dec(g_mon[i].x); say(","); say_dec(g_mon[i].y); say(g_mon[i].primary ? " primary" : ""); }
    say("\n");
    if (g_disp.mon_req > g_nmon) { say("display: monitor "); say_dec(g_disp.mon_req); say(" does not exist - the primary monitor is used\n"); }
    if (g_disp.sim_w) { say("display: TEST --sim-monitor: as if the monitor were "); say_dec(g_disp.sim_w); say("x"); say_dec(g_disp.sim_h); say("\n"); }
    int ow = g_disp.out_w, oh = g_disp.out_h, pw, ph;
    if ((i64)ow * 9 > (i64)oh * 16) { ph = oh; pw = (int)(((i64)oh * 16 + 4) / 9); } else { pw = ow; ph = (int)(((i64)ow * 9 + 8) / 16); }
    say("display: "); say(g_disp.borderless ? "borderless on monitor " : "windowed on monitor "); say_dec(g_disp.mon + 1); say(", "); say_dec(ow); say("x"); say_dec(oh);
    say(", picture area "); say_dec(pw); say("x"); say_dec(ph); say(pw < ow ? " (pillarbox: black bars left and right)" : ph < oh ? " (letterbox: black bars above and below)" : " (the whole of it)"); say("\n");
    if (forced) { say("display: internal resolution from --render-scale "); say_dec(g_rs_pct); say(" (developer switch)\n"); return; }
    int th = ph; if (rr_mode == 2) th = rr_h < (rr_w * 9 + 8) / 16 ? rr_h : (rr_w * 9 + 8) / 16;
    int ci = disp_pick(th), c0 = ci;
    if (ci > 0 && !g_dopt_no_vram) {
        char nm[96]; u64 vram = vk_early_vram(nm, sizeof nm); if (g_sim_vram) vram = g_sim_vram;
        if (vram) {
            u64 usable = vram > (4ull << 30) ? vram - (2ull << 30) : vram / 2, cap = vram < (12ull << 30) ? ((usable * 9 / 16) & ~((256ull << 20) - 1)) : vram / 2;
            if (!g_opt_no_adaptive_mem || g_opt_vram_fit || vram >= (12ull << 30)) { } else cap = vram / 2;   /* (--no-adaptive-mem without --vram-fit: no fitted budget) */
            while (ci > 0 && (disp_need_mb(k_disp_pct[ci]) + 1500) > (cap >> 20)) ci--;
            say("display: video memory "); say_dec((i64)(vram >> 20)); say(" MB"); if (nm[0]) { say(" ("); say(nm); say(")"); } say(", image budget "); say_dec((i64)(cap >> 20)); say(" MB; screen-sized targets need about "); say_dec((i64)disp_need_mb(k_disp_pct[c0])); say(" MB at ");
            say_dec(disp_pct_w(k_disp_pct[c0])); say("x"); say_dec(disp_pct_h(k_disp_pct[c0]));
            if (ci != c0) { say(" - TOO LITTLE VIDEO MEMORY: internal resolution lowered to "); say_dec(disp_pct_w(k_disp_pct[ci])); say("x"); say_dec(disp_pct_h(k_disp_pct[ci])); say(" (about "); say_dec((i64)disp_need_mb(k_disp_pct[ci])); say(" MB); the picture is scaled to the window"); }
            say("\n");
        } else say("display: the graphics card's memory could not be read before the start - no check of the internal resolution against it\n");
    }
    g_rs_pct = k_disp_pct[ci];
    int iw = disp_pct_w(g_rs_pct), ih = disp_pct_h(g_rs_pct);
    say("display: internal resolution "); say_dec(iw); say("x"); say_dec(ih); say(rr_mode == 2 ? " (render_resolution " : " (auto: the picture area's; wanted "); if (rr_mode == 2) { say_dec(rr_w); say("x"); say_dec(rr_h); } else { say_dec(pw); say("x"); say_dec(ph); }
    say(iw == pw && ih == ph ? "), shown pixel for pixel" : iw > pw ? "), filtered down to the picture area" : "), enlarged to the picture area"); say(g_rs_pct == 100 ? "; the game's own resolution\n" : "\n");
}

/* ---------------------------------------------------------------- w28_int (w28i_10): low_driver_memory = 0 written by earlier dev builds
   w28i_1 .. w28i_9 wrote "low_driver_memory = 0" by themselves (template of a fresh file, or the "(added by this version)" block
   added to an older file). auto is now the default. Such a line is changed to auto ONLY when the file is provably as the program
   wrote it: the line sits directly under one of those two exact comment blocks, AND every other setting in the file still has
   its default value (only lines the program itself writes or the player must set - game_dir, display_mode, monitor, window
   size / place, update_skip_version - may differ). Anything else - another comment, another value, any changed setting - may be
   the player's own choice: the 0 is kept and the log says how to switch to auto. Returns 1 migrated, 2 kept, 0 not applicable. */
static const char *const k_ldm_old_blocks[2] = {
    "; 1 = game memory that only the processor uses is not handed to the graphics driver (about 1.5 GB less driver\r\n"
    "; memory; may help PCs with 16 GB of RAM). Experimental. 0 = off (default). Restart after changing.\r\n"
    "low_driver_memory = 0\r\n",
    "; (added by this version) 1 = game memory only the processor uses is not handed to the graphics driver (about\r\n"
    "; 1.5 GB less driver memory). Experimental. 0 = off (default). Restart after changing.\r\n"
    "low_driver_memory = 0\r\n" };
static const char *const k_ldm_new_block =
    "; auto (default) = on only when the graphics driver's budget for system memory is small (PCs with little RAM, e.g.\r\n"
    "; 16 GB): game memory that only the processor uses is then not handed to the graphics driver (about 1.5 GB less\r\n"
    "; driver memory). 1 = always on, 0 = always off. Restart after changing.\r\n"
    "low_driver_memory = auto\r\n";
static const char *const k_ud_defaults[][2] = {   /* every setting earlier versions wrote, with its default value */
    {"frame_rate_limit", "60"}, {"optimised_rendering", "1"}, {"render_resolution", "auto"}, {"renderer_thread", "1"}, {"gpu", "auto"},
    {"present_mode", "auto"}, {"gpu_optimisations", "1"}, {"heap_slot_mb", "512"}, {"heap_slot_reuse", "1"}, {"shader_preparation", "1"},
    {"gpu_coherence", "1"}, {"halloween_hunt", "0"}, {"language", "auto"}, {"text_language", "auto"}, {"speech_language", "auto"},
    {"audio_late_read", "1"}, {"audio_limiter", "1"}, {"gyro_calibration", "1"}, {"low_vram_textures", "auto"}, {"depth_of_field", "1"},
    {"low_driver_memory", "0"}, {"rs_depth_max", "auto"}, {"cpu_compat", "0"}, {"update_check", "0"}, {"developer", "0"} };
static const char *const k_ud_prog_keys[] = {"game_dir", "display_mode", "monitor", "window_width", "window_height", "window_x", "window_y", "update_skip_version"};
static int ud_find(const char *t, const char *s) { int n = str_len(s); for (int i = 0; t[i]; i++) { int k = 0; while (k < n && t[i + k] == s[k]) k++; if (k == n) return i; } return -1; }
/* w38_rel: the language comments of an earlier version's settings file are replaced by the new list (all 20 languages, codes,
   editions, voices, import). Only those exact comment lines change; every value (language = ... etc.) stays as the player set it. */
static const char *const k_lang_old_doc[2] = {"; Language of the menus, subtitles and voices, chosen by the game itself as on a PS4 set to that language:\r\n; auto (English) or english, russian, polish, spanish, portuguese, swedish, norwegian, finnish, danish.\r\n; Tested with these game files (CUSA02636): russian and polish work. Languages this edition does not contain\r\n; (e.g. french, german, italian) fall back to English. Restart after changing.\r\n", "; Optional: menus / subtitles (text_language) and voices (speech_language) in different languages, through the\r\n; game's own override, e.g. text_language = russian with English voices. auto = as language above. Use only\r\n; languages that work with language = (the override does not check what the game files contain).\r\n"};
static const char *k_lang_new_doc =
    "; ---- LANGUAGE ------------------------------------------------------------------------------------------------\r\n"
    "; To change the language: write one value (the name or the code) after  language =  below, save, and restart.\r\n"
    ";   Example:  language = french     or     language = fr        auto = English\r\n"
    "; Menus, subtitles and voices change together; the game loads a language's voices with its text.\r\n"
    "; A language of YOUR edition (see the editions column) works at once. A language that only ANOTHER edition has\r\n"
    "; must be imported once from your own copy of that edition with UDLanguageImporter.exe (next to UntilDawn.exe);\r\n"
    "; without the import the game uses English. Logs\\UntilDawn.log says at start which case applies.\r\n"
    ";\r\n"
    ";   name                 code    voices       editions that contain it                    status\r\n"
    ";   english              en      yes          every edition                               verified\r\n"
    ";   spanish              es      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   portuguese           pt      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   russian              ru      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   polish               pl      yes          CUSA02636 (Europe 2)                        verified\r\n"
    ";   danish               da      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   finnish              fi      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   norwegian            no      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   swedish              sv      text only    CUSA02636 (Europe 2)                        verified\r\n"
    ";   french               fr      yes          CUSA00194 (Europe 1), CUSA00359 (Americas)  verified from CUSA00194\r\n"
    ";   german               de      yes          CUSA00194 (Europe 1)                        verified\r\n"
    ";   italian              it      yes          CUSA00194 (Europe 1)                        verified\r\n"
    ";   dutch                nl      text only    CUSA00194 (Europe 1)                        verified\r\n"
    ";   turkish              tr      text only    CUSA00194 (Europe 1)                        verified\r\n"
    ";   spanish_latam        es-la   yes (1)      CUSA00359 (Americas)                        EXPERIMENTAL\r\n"
    ";   portuguese_brazil    pt-br   yes (1)      CUSA00359 (Americas)                        EXPERIMENTAL\r\n"
    ";   japanese             ja      unknown      CUSA00902 (Japan)                           EXPERIMENTAL, untested\r\n"
    ";   korean               ko      unknown      CUSA01423 (Asia)                            EXPERIMENTAL, untested\r\n"
    ";   chinese_traditional  zh-tw   unknown      CUSA01423 (Asia)                            EXPERIMENTAL, untested\r\n"
    ";   chinese_simplified   zh-cn   unknown      CUSA01423 (Asia)                            EXPERIMENTAL, untested\r\n"
    "; (1) the CUSA00359 file list has text and voice files for these two languages; not yet tested in game.\r\n"
    "; EXPERIMENTAL languages are used only from an experimental import (language_experimental, see LANGUAGE_PACKS_README.txt).\r\n"
    "; Optional: text_language / speech_language split menus + subtitles from voices (same names or codes), e.g.\r\n"
    "; text_language = russian with English voices. auto = as language above.\r\n";
static void ud_lang_doc_migrate(void) {
    if (g_uds.fresh || !g_ud_ini_path[0] || !g_dopt_save) return;
    u64 n = 0; char *t = (char *)plat_read_file(g_ud_ini_path, &n); if (!t) return;
    char *z = (char *)malloc(n + 1); if (!z) return; mem_copy(z, t, n); z[n] = 0;
    int at = ud_find(z, k_lang_old_doc[0]); if (at < 0) { free(z); return; }
    int ol = str_len(k_lang_old_doc[0]), nl = str_len(k_lang_new_doc);
    char *o = (char *)malloc(n + (u64)nl + 1); if (!o) { free(z); return; }
    mem_copy(o, z, (u64)at); mem_copy(o + at, k_lang_new_doc, (u64)nl); mem_copy(o + at + nl, z + at + ol, n - (u64)at - (u64)ol); u64 m = n - (u64)ol + (u64)nl; o[m] = 0;
    int a2 = ud_find(o, k_lang_old_doc[1]);   /* the old text_language comment, now part of the new list */
    if (a2 >= 0) { int l2 = str_len(k_lang_old_doc[1]); mem_copy(o + a2, o + a2 + l2, m - (u64)a2 - (u64)l2 + 1); }
    void *f = plat_open_write(g_ud_ini_path); if (f) { plat_write(f, o); plat_close(f); }
    say("settings: the language notes in Config\\settings.ini were updated (all 20 languages, codes and editions); your values are unchanged\n");
    free(o); free(z);
}

static int ud_ldm_migrate(void) {
    if (!g_uds.seen_ldm || g_uds.low_driver_mem != 0 || g_uds.fresh || !g_ud_ini_path[0]) return 0;
    u64 n = 0; char *t = (char *)plat_read_file(g_ud_ini_path, &n); if (!t) return 0;
    char *z = (char *)malloc(n + 1); if (!z) return 0; mem_copy(z, t, n); z[n] = 0;
    int at = -1, blk = -1; for (int b = 0; b < 2 && at < 0; b++) { at = ud_find(z, k_ldm_old_blocks[b]); blk = b; }
    int why = at < 0 ? 1 : 0;   /* 1: the line or its comment is not the program's own text */
    if (!why) {   /* every other setting at its default? */
        char *c = (char *)malloc(n + 1); if (!c) { free(z); return 0; } mem_copy(c, z, n + 1);
        for (char *q = c; *q && !why; ) {
            char *line = q; while (*q && *q != '\n') q++; if (*q) *q++ = 0;
            ud_trim(line); while (*line == ' ' || *line == '\t') line++;
            if (!*line || *line == ';' || *line == '#' || *line == '[') continue;
            char *eq = line; while (*eq && *eq != '=') eq++; if (!*eq) { why = 2; break; }
            *eq = 0; char *v = eq + 1; ud_trim(line); while (*v == ' ' || *v == '\t') v++; ud_trim(v);
            int prog = 0; for (unsigned k = 0; k < sizeof k_ud_prog_keys / sizeof k_ud_prog_keys[0]; k++) if (ud_key_is(line, k_ud_prog_keys[k])) prog = 1;
            if (prog) continue;
            int known = 0; for (unsigned k = 0; k < sizeof k_ud_defaults / sizeof k_ud_defaults[0]; k++) if (ud_key_is(line, k_ud_defaults[k][0])) { known = 1; if (!ud_key_is(v, k_ud_defaults[k][1])) why = 2; }
            if (!known) why = 2;
        }
        free(c);
    }
    if (why) {
        g_ldm_mig_why = why;
        say("settings: low_driver_memory = 0 kept - ");
        say(why == 1 ? "the line is not the text an earlier development build wrote" : "the file has other settings changed by hand");
        say(", so 0 may be your own choice. The default is now auto (on only when the graphics driver's budget for system memory is small); to use it, write  low_driver_memory = auto  in Config\\settings.ini\n");
        free(z); return 2;
    }
    g_uds.low_driver_mem = -1;
    if (g_dopt_save) {
        int ol = str_len(k_ldm_old_blocks[blk]), nl = str_len(k_ldm_new_block);
        char *o = (char *)malloc(n + (u64)nl + 1);
        if (o) { mem_copy(o, z, (u64)at); mem_copy(o + at, k_ldm_new_block, (u64)nl); mem_copy(o + at + nl, z + at + ol, n - (u64)at - (u64)ol); o[n - ol + nl] = 0;
            void *f = plat_open_write(g_ud_ini_path); if (f) { plat_write(f, o); plat_close(f); } free(o); }
    }
    say("settings: low_driver_memory = 0 was written by an earlier development build (the file is otherwise as the program wrote it) - now auto, the new default");
    say(g_dopt_save ? " (Config\\settings.ini updated)\n" : " (not saved: not a player start)\n");
    free(z); return 1;
}
