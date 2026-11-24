/*
 * ud_update_core.h - w26_update: the portable part of the updater (ud_update.exe; design: NOTES.md of w26_update).
 *
 * Everything here is plain C without a C library: SHA-256, CRC-32, inflate, a ZIP reader, a small JSON reader for the
 * GitHub "latest release" answer, version comparison, the install policy (which files of a release are program
 * files) and the transactional install with journal / rollback / recovery. The host (ud_update.c on Windows,
 * ud_update_test.c on Linux) defines, BEFORE including this file:
 *   types u8 u16 u32 u64 i64, and the functions
 *   void *up_fopen_r(const char *), *up_fopen_w(const char *); i64 up_fread(void *, void *, u64);
 *   int up_fwrite(void *, const void *, u64) (1 ok); int up_fclose(void *, int flush) (1 ok);
 *   int up_exists(const char *) (0 none, 1 file, 2 folder); int up_mkdir1(const char *) (1 ok or exists);
 *   int up_move(const char *from, const char *to) (1 ok; replaces a file at to);
 *   int up_delete(const char *), up_rmdir(const char *) (1 ok);
 *   int up_list(const char *dir, int (*cb)(void *, const char *name, int isdir), void *ctx) (1 ok);
 *   void *up_alloc(u64), up_free(void *); void up_log_raw(const char *); void up_crash(void);
 *   void up_progress(const char *) (UI text, may do nothing); int g_up_quiet (--quiet: no message boxes / windows);
 *   int up_http_get(const char *url, int api, int (*sink)(void *, const u8 *, u32), void *sctx, i64 *content_len,
 *                   char *err, int errn) (0 ok; HTTPS GET with the host's timeouts; *content_len -1 if unknown).
 * Paths use '/' (Windows accepts it).
 */
#pragma once

/* ------------------------------------------------------------------ fixed facts (pinned) */
#define UP_REPO          "SuperCatElvis/UntilDawn_PC"
#define UP_API_URL       "https://api.github.com/repos/" UP_REPO "/releases/latest"
#define UP_DL_PREFIX     "https://github.com/" UP_REPO "/releases/download/"
#define UP_ZIP_PREFIX    "UntilDawn_PC_v"
#define UP_MAX_JSON      (1u << 20)          /* the API answer: at most 1 MB */
#define UP_MAX_ZIP       (256ull << 20)      /* the release zip: at most 256 MB */
#define UP_MAX_UNPACKED  (512ull << 20)      /* all installed files together */
#define UP_MAX_ENTRIES   1000

/* ------------------------------------------------------------------ small string helpers */
static int up_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int up_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int up_lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int up_ieq(const char *a, const char *b) { while (*a && up_lc(*a) == up_lc(*b)) { a++; b++; } return *a == *b; }
static int up_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int up_ends_i(const char *s, const char *e) { int a = up_len(s), b = up_len(e); return a >= b && up_ieq(s + a - b, e); }
static void up_cpy(char *d, const char *s, int n) { int k = 0; while (s[k] && k < n - 1) { d[k] = s[k]; k++; } d[k] = 0; }
static void up_mcpy(void *d, const void *s, u64 n) { u8 *a = (u8 *)d; const u8 *b = (const u8 *)s; while (n--) *a++ = *b++; }
static void up_mset(void *d, int c, u64 n) { u8 *a = (u8 *)d; while (n--) *a++ = (u8)c; }
typedef struct { char *b; int n, cap; } UpSb;
static void sb_put(UpSb *s, const char *t) { while (*t && s->n + 1 < s->cap) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void sb_num(UpSb *s, i64 v) { char t[24]; int i = 23; t[i] = 0; int neg = v < 0; u64 u = neg ? (u64)-v : (u64)v; if (!u) t[--i] = '0'; while (u) { t[--i] = (char)('0' + u % 10); u /= 10; } if (neg) t[--i] = '-'; sb_put(s, t + i); }
static void up_join(char *o, int n, const char *a, const char *b) { UpSb s = {o, 0, n}; o[0] = 0; sb_put(&s, a); if (s.n && o[s.n - 1] != '/' && o[s.n - 1] != '\\') sb_put(&s, "/"); sb_put(&s, b); }
static const char *up_basename(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1; return b; }

/* ------------------------------------------------------------------ log */
static void up_log(const char *a, const char *b, const char *c) { up_log_raw("update: "); up_log_raw(a); if (b) up_log_raw(b); if (c) up_log_raw(c); up_log_raw("\n"); }
static void up_logn(const char *a, i64 v, const char *c) { char t[32]; UpSb s = {t, 0, sizeof t}; sb_num(&s, v); up_log(a, t, c); }

/* ------------------------------------------------------------------ SHA-256 */
typedef struct { u32 h[8]; u64 len; u8 buf[64]; u32 n; } UpSha;
static const u32 k_up_sha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define UP_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void up_sha_block(UpSha *s, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = UP_ROR(w[i - 15], 7) ^ UP_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = UP_ROR(w[i - 2], 17) ^ UP_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (UP_ROR(e, 6) ^ UP_ROR(e, 11) ^ UP_ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_up_sha[i] + w[i];
        u32 t2 = (UP_ROR(a, 2) ^ UP_ROR(a, 13) ^ UP_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void up_sha_init(UpSha *s) {
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0; s->n = 0;
}
static void up_sha_add(UpSha *s, const u8 *p, u64 n) {
    s->len += n;
    while (n) {
        if (!s->n && n >= 64) { up_sha_block(s, p); p += 64; n -= 64; continue; }
        u32 c = 64 - s->n; if (c > n) c = (u32)n;
        up_mcpy(s->buf + s->n, p, c); s->n += c; p += c; n -= c;
        if (s->n == 64) { up_sha_block(s, s->buf); s->n = 0; }
    }
}
static void up_sha_hex(UpSha *s, char out[65]) {
    u64 bits = s->len * 8; u8 pad = 0x80, z = 0;
    up_sha_add(s, &pad, 1);
    while (s->n != 56) up_sha_add(s, &z, 1);
    u8 L[8]; for (int i = 0; i < 8; i++) L[i] = (u8)(bits >> (56 - 8 * i));
    up_sha_add(s, L, 8);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) { u8 b = (u8)(s->h[i] >> (24 - 8 * j)); out[i * 8 + j * 2] = hx[b >> 4]; out[i * 8 + j * 2 + 1] = hx[b & 15]; }
    out[64] = 0;
}
static int up_sha_file(const char *path, char out[65], i64 *size) {
    void *f = up_fopen_r(path); if (!f) return 0;
    static u8 buf[1 << 16]; UpSha s; up_sha_init(&s); i64 n, tot = 0;
    while ((n = up_fread(f, buf, sizeof buf)) > 0) { up_sha_add(&s, buf, (u64)n); tot += n; }
    up_fclose(f, 0); if (n < 0) return 0;
    up_sha_hex(&s, out); if (size) *size = tot; return 1;
}

/* ------------------------------------------------------------------ CRC-32 (ZIP) */
static u32 up_crc32(const u8 *p, u64 n) {
    static u32 t[256]; static int init;
    if (!init) { for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
    u32 c = 0xFFFFFFFFu; while (n--) c = t[(c ^ *p++) & 255] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ inflate (RFC 1951), whole buffer to whole buffer */
typedef struct { const u8 *src; u64 slen, spos; u32 bb; int bc; u8 *dst; u64 dlen, dpos; int err; } UpInf;
typedef struct { short count[16]; short sym[288]; } UpHuff;
static u32 inf_bits(UpInf *s, int n) {
    while (s->bc < n) { if (s->spos >= s->slen) { s->err = 1; return 0; } s->bb |= (u32)s->src[s->spos++] << s->bc; s->bc += 8; }
    u32 v = s->bb & ((1u << n) - 1); s->bb >>= n; s->bc -= n; return v;
}
static int inf_build(UpHuff *h, const u8 *len, int n) {   /* <0: over-subscribed */
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1; for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return -1; }
    offs[1] = 0; for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return left;
}
static int inf_decode(UpInf *s, const UpHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)inf_bits(s, 1); if (s->err) return -1;
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int inf_codes(UpInf *s, const UpHuff *lc, const UpHuff *dc) {
    static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = inf_decode(s, lc); if (sym < 0) return -1;
        if (sym < 256) { if (s->dpos >= s->dlen) return -2; s->dst[s->dpos++] = (u8)sym; continue; }
        if (sym == 256) return 0;
        sym -= 257; if (sym >= 29) return -3;
        u64 len = (u64)lbase[sym] + inf_bits(s, lext[sym]);
        int ds = inf_decode(s, dc); if (ds < 0 || ds >= 30) return -4;
        u64 dist = (u64)dbase[ds] + inf_bits(s, dext[ds]);
        if (s->err) return -1;
        if (dist > s->dpos) return -5;
        if (s->dpos + len > s->dlen) return -2;
        for (u64 k = 0; k < len; k++) { s->dst[s->dpos] = s->dst[s->dpos - dist]; s->dpos++; }
    }
}
static int up_inflate(const u8 *src, u64 slen, u8 *dst, u64 dlen, u64 *out_n) {
    UpInf s; up_mset(&s, 0, sizeof s); s.src = src; s.slen = slen; s.dst = dst; s.dlen = dlen;
    static UpHuff lc, dc; u8 lens[320];
    int last, r;
    do {
        last = (int)inf_bits(&s, 1); int type = (int)inf_bits(&s, 2); if (s.err) return -10;
        if (type == 0) {
            s.bb = 0; s.bc = 0;
            if (s.spos + 4 > s.slen) return -11;
            u32 len = s.src[s.spos] | (u32)s.src[s.spos + 1] << 8, nlen = s.src[s.spos + 2] | (u32)s.src[s.spos + 3] << 8; s.spos += 4;
            if ((len ^ 0xFFFF) != nlen) return -12;
            if (s.spos + len > s.slen) return -13;
            if (s.dpos + len > s.dlen) return -2;
            up_mcpy(s.dst + s.dpos, s.src + s.spos, len); s.dpos += len; s.spos += len; r = 0;
        } else if (type == 1) {
            int i = 0; for (; i < 144; i++) lens[i] = 8; for (; i < 256; i++) lens[i] = 9; for (; i < 280; i++) lens[i] = 7; for (; i < 288; i++) lens[i] = 8;
            inf_build(&lc, lens, 288); for (i = 0; i < 30; i++) lens[i] = 5; inf_build(&dc, lens, 30);
            r = inf_codes(&s, &lc, &dc);
        } else if (type == 2) {
            static const u8 order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            int nlen = (int)inf_bits(&s, 5) + 257, ndist = (int)inf_bits(&s, 5) + 1, ncode = (int)inf_bits(&s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return -14;
            int i = 0; for (; i < ncode; i++) lens[order[i]] = (u8)inf_bits(&s, 3); for (; i < 19; i++) lens[order[i]] = 0;
            if (s.err || inf_build(&lc, lens, 19) != 0) return -15;
            i = 0;
            while (i < nlen + ndist) {
                int sym = inf_decode(&s, &lc); if (sym < 0) return -16;
                if (sym < 16) { lens[i++] = (u8)sym; continue; }
                int len = 0, rep;
                if (sym == 16) { if (!i) return -17; len = lens[i - 1]; rep = 3 + (int)inf_bits(&s, 2); }
                else if (sym == 17) rep = 3 + (int)inf_bits(&s, 3);
                else rep = 11 + (int)inf_bits(&s, 7);
                if (s.err || i + rep > nlen + ndist) return -18;
                while (rep--) lens[i++] = (u8)len;
            }
            if (!lens[256]) return -19;
            if (inf_build(&lc, lens, nlen) < 0) return -20;
            if (inf_build(&dc, lens + nlen, ndist) < 0) return -21;
            r = inf_codes(&s, &lc, &dc);
        } else return -22;
        if (r) return r;
    } while (!last);
    *out_n = s.dpos; return 0;
}

/* ------------------------------------------------------------------ versions: "v0.4.2" / "0.4.2" (1-4 numbers); anything else (e.g. "-rc1") is not a release */
typedef struct { int n; u32 v[4]; } UpVer;
static int up_ver_parse(const char *s, UpVer *o) {
    o->n = 0; if (*s == 'v' || *s == 'V') s++;
    for (;;) {
        if (*s < '0' || *s > '9' || o->n >= 4) return 0;
        u32 x = 0; int d = 0; while (*s >= '0' && *s <= '9') { x = x * 10 + (u32)(*s++ - '0'); if (++d > 6) return 0; }
        o->v[o->n++] = x;
        if (!*s) return 1;
        if (*s++ != '.') return 0;
    }
}
static int up_ver_cmp(const UpVer *a, const UpVer *b) {   /* -1 a<b, 0 equal (missing parts are 0), 1 a>b */
    for (int i = 0; i < 4; i++) { u32 x = i < a->n ? a->v[i] : 0, y = i < b->n ? b->v[i] : 0; if (x != y) return x < y ? -1 : 1; }
    return 0;
}
static void up_ver_str(const UpVer *v, char *o, int n) { UpSb s = {o, 0, n}; o[0] = 0; for (int i = 0; i < v->n; i++) { if (i) sb_put(&s, "."); sb_num(&s, v->v[i]); } }

/* ------------------------------------------------------------------ JSON: the fields of GitHub's release object we use */
typedef struct {
    char tag[64], ver[32], body[1024], zip_url[512], sha_url[512], zip_name[128], seen[600];
    char zip_digest[65];     /* G12: the SHA-256 GitHub reports for the zip asset ("digest": "sha256:<hex>"); "" = none */
    int draft, prerelease, have_tag; i64 zip_size;
    int nas; struct { char url[512]; char digest[65]; i64 size; } as[16];   /* the assets as listed (chosen once the tag is known) */
} UpRel;
#ifndef UP_ACCEPT_LEGACY_NAME
#define UP_ACCEPT_LEGACY_NAME 1   /* coordinator 16 Nov: also accept UntilDawn_<ver>.zip (the name v0.4.1 is published under); 0 = only UntilDawn_v<ver>.zip / UntilDawn_PC_v<ver>.zip */
#endif
static int g_up_legacy_name = UP_ACCEPT_LEGACY_NAME;
typedef struct { const char *p, *e; int err; } UpJs;
static void js_ws(UpJs *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++; }
static int js_str(UpJs *j, char *o, int on) {   /* at '"': decodes (non-ASCII and controls -> '?'); o may be 0 */
    js_ws(j); if (j->p >= j->e || *j->p != '"') { j->err = 1; return 0; }
    j->p++; int k = 0;
    while (j->p < j->e && *j->p != '"') {
        int c = (u8)*j->p++;
        if (c == '\\') {
            if (j->p >= j->e) break;
            c = *j->p++;
            if (c == 'n') c = '\n'; else if (c == 't') c = ' '; else if (c == 'r') c = '\r'; else if (c == 'b' || c == 'f') c = ' ';
            else if (c == 'u') { u32 v = 0; for (int i = 0; i < 4; i++) { if (j->p >= j->e) { j->err = 1; return 0; } int h = *j->p++; v = v * 16 + (u32)(h >= '0' && h <= '9' ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : 0); } c = v >= 0x20 && v < 0x7F ? (int)v : '?'; }
        } else if (c >= 0x80) c = '?';
        if (c < 0x20 && c != '\n' && c != '\r') c = ' ';
        if (o && k < on - 1) o[k++] = (char)c;
    }
    if (j->p >= j->e) { j->err = 1; return 0; }
    j->p++; if (o) o[k] = 0; return 1;
}
static void js_skip(UpJs *j, int depth) {
    js_ws(j); if (j->p >= j->e || depth > 64) { j->err = 1; return; }
    char c = *j->p;
    if (c == '"') { js_str(j, 0, 0); return; }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']'; j->p++; js_ws(j);
        if (j->p < j->e && *j->p == close) { j->p++; return; }
        for (;;) {
            if (c == '{') { js_str(j, 0, 0); js_ws(j); if (j->p >= j->e || *j->p != ':') { j->err = 1; return; } j->p++; }
            js_skip(j, depth + 1); if (j->err) return;
            js_ws(j); if (j->p >= j->e) { j->err = 1; return; }
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == close) { j->p++; return; }
            j->err = 1; return;
        }
    }
    while (j->p < j->e && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\n' && *j->p != '\r' && *j->p != '\t') j->p++;   /* number / true / false / null */
}
static int js_bool(UpJs *j) { js_ws(j); int t = j->e - j->p >= 4 && up_starts(j->p, "true"); js_skip(j, 0); return t; }
static void js_asset(UpJs *j, UpRel *r) {   /* one element of "assets" */
    js_ws(j); if (j->p >= j->e || *j->p != '{') { js_skip(j, 1); return; }
    j->p++; js_ws(j); if (j->p < j->e && *j->p == '}') { j->p++; return; }
    char url[512] = "", key[64], dig[96] = ""; i64 size = -1;
    for (;;) {
        if (!js_str(j, key, sizeof key)) return;
        js_ws(j); if (j->p >= j->e || *j->p != ':') { j->err = 1; return; } j->p++;
        if (up_eq(key, "browser_download_url")) js_str(j, url, sizeof url);
        else if (up_eq(key, "digest")) { js_ws(j); if (j->p < j->e && *j->p == '"') js_str(j, dig, sizeof dig); else js_skip(j, 2); }
        else if (up_eq(key, "size")) { js_ws(j); i64 v = 0; while (j->p < j->e && *j->p >= '0' && *j->p <= '9') v = v * 10 + (*j->p++ - '0'); size = v; js_skip(j, 2); }
        else js_skip(j, 2);
        if (j->err) return;
        js_ws(j); if (j->p < j->e && *j->p == ',') { j->p++; continue; }
        if (j->p < j->e && *j->p == '}') { j->p++; break; }
        j->err = 1; return;
    }
    { UpSb sb = {r->seen, up_len(r->seen), sizeof r->seen}; if (sb.n) sb_put(&sb, ", "); sb_put(&sb, up_starts(url, UP_DL_PREFIX) ? url + sizeof UP_DL_PREFIX - 1 : url); if (dig[0]) sb_put(&sb, " (digest)"); }   /* (for the log) */
    if (r->nas >= 16 || !url[0]) return;
    up_cpy(r->as[r->nas].url, url, sizeof r->as[0].url); r->as[r->nas].size = size; r->as[r->nas].digest[0] = 0;
    /* "sha256:" + exactly 64 hex digits; anything else counts as no digest */
    if (up_starts(dig, "sha256:") && up_len(dig) == 7 + 64) { int ok = 1; char h[65]; for (int k = 0; k < 64; k++) { int c = up_lc(dig[7 + k]); if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) ok = 0; h[k] = (char)c; } h[64] = 0;
        if (ok) up_cpy(r->as[r->nas].digest, h, 65); }
    r->nas++;
}
/* 0 ok; otherwise err says why */
static int up_parse_release(const char *t, u64 n, UpRel *r, char *err, int errn) {
    up_mset(r, 0, sizeof *r); r->zip_size = -1;
    UpJs j = {t, t + n, 0}; js_ws(&j);
    if (j.p >= j.e || *j.p != '{') { up_cpy(err, "the answer is not a JSON object", errn); return 1; }
    j.p++; js_ws(&j);
    char key[64];
    if (j.p < j.e && *j.p != '}') for (;;) {
        if (!js_str(&j, key, sizeof key)) break;
        js_ws(&j); if (j.p >= j.e || *j.p != ':') { j.err = 1; break; } j.p++;
        if (up_eq(key, "tag_name")) { js_str(&j, r->tag, sizeof r->tag); r->have_tag = 1; }
        else if (up_eq(key, "body")) { js_ws(&j); if (j.p < j.e && *j.p == '"') js_str(&j, r->body, sizeof r->body); else js_skip(&j, 1); }
        else if (up_eq(key, "draft")) r->draft = js_bool(&j);
        else if (up_eq(key, "prerelease")) r->prerelease = js_bool(&j);
        else if (up_eq(key, "assets")) {
            js_ws(&j); if (j.p >= j.e || *j.p != '[') { js_skip(&j, 1); }
            else { j.p++; js_ws(&j);
                if (j.p < j.e && *j.p == ']') j.p++;
                else for (;;) { js_asset(&j, r); if (j.err) break; js_ws(&j); if (j.p < j.e && *j.p == ',') { j.p++; continue; } if (j.p < j.e && *j.p == ']') { j.p++; break; } j.err = 1; break; } }
        } else js_skip(&j, 1);
        if (j.err) break;
        js_ws(&j); if (j.p < j.e && *j.p == ',') { j.p++; continue; }
        if (j.p < j.e && *j.p == '}') break;
        j.err = 1; break;
    }
    if (j.err) { up_cpy(err, "the answer is not valid JSON", errn); return 2; }
    if (!r->have_tag || !r->tag[0]) { up_cpy(err, "the answer has no tag_name (no release, or a GitHub error message)", errn); return 3; }
    UpVer v; if (!up_ver_parse(r->tag, &v)) { up_cpy(err, "the release tag is not a version number", errn); return 4; }
    up_ver_str(&v, r->ver, sizeof r->ver);
    if (r->draft || r->prerelease) { up_cpy(err, "the latest release is a draft or pre-release", errn); return 5; }
    /* the package, in this order of preference (file name case-insensitive, exactly this tag's version, only in this tag's
       download folder): UntilDawn_v<ver>.zip (the user's convention), UntilDawn_<ver>.zip (v0.4.1 as published),
       UntilDawn_PC_v<ver>.zip. Optional: <that name>.sha256.txt in the same folder. */
    {
        static const char *pre_names[3] = {"UntilDawn_v", "UntilDawn_", UP_ZIP_PREFIX};
        char folder[300]; UpSb f = {folder, 0, sizeof folder}; folder[0] = 0; sb_put(&f, UP_DL_PREFIX); sb_put(&f, r->tag); sb_put(&f, "/");
        for (int pass = 0; pass < 3 && !r->zip_url[0]; pass++) {
            if (pass == 1 && !g_up_legacy_name) continue;
            char want[160]; UpSb s = {want, 0, sizeof want}; want[0] = 0; sb_put(&s, pre_names[pass]); sb_put(&s, r->ver); sb_put(&s, ".zip");
            for (int i = 0; i < r->nas && !r->zip_url[0]; i++) {
                const char *u = r->as[i].url;
                if (!up_starts(u, folder) || !up_ieq(u + f.n, want)) continue;
                up_cpy(r->zip_url, u, sizeof r->zip_url); up_cpy(r->zip_name, u + f.n, sizeof r->zip_name); r->zip_size = r->as[i].size; up_cpy(r->zip_digest, r->as[i].digest, 65);
            }
        }
        if (r->zip_url[0]) {
            char sw[200]; UpSb s = {sw, 0, sizeof sw}; sw[0] = 0; sb_put(&s, r->zip_name); sb_put(&s, ".sha256.txt");
            for (int i = 0; i < r->nas; i++) if (up_starts(r->as[i].url, folder) && up_ieq(r->as[i].url + f.n, sw)) { up_cpy(r->sha_url, r->as[i].url, sizeof r->sha_url); break; }
        }
    }
    if (!r->zip_url[0]) { up_cpy(err, "the release has no program package asset of the expected name", errn); return 6; }
    if (!r->zip_digest[0] && !r->sha_url[0]) { up_cpy(err, "the release publishes no SHA-256 for its package (no asset digest, no .sha256.txt) - not installed unverified", errn); return 7; }
    if (r->zip_size > (i64)UP_MAX_ZIP) { up_cpy(err, "the package is larger than the updater accepts", errn); return 8; }
    return 0;
}
/* "<64 hex>  [*]name" -> lower-case hex; 0 if malformed or for another file */
static int up_parse_sha_txt(const char *t, u64 n, const char *name, char out[65]) {
    u64 i = 0; while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n' || (u8)t[i] == 0xEF || (u8)t[i] == 0xBB || (u8)t[i] == 0xBF)) i++;
    for (int k = 0; k < 64; k++, i++) { if (i >= n) return 0; int c = up_lc(t[i]); if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; out[k] = (char)c; }
    out[64] = 0;
    if (i < n && t[i] != ' ' && t[i] != '\t' && t[i] != '\r' && t[i] != '\n') return 0;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    if (i < n && t[i] == '*') i++;
    if (i >= n || t[i] == '\r' || t[i] == '\n') return 1;   /* hash only */
    char nm[200]; int k = 0; while (i < n && t[i] != '\r' && t[i] != '\n' && k < 199) nm[k++] = t[i++]; nm[k] = 0;
    while (k && (nm[k - 1] == ' ' || nm[k - 1] == '\t')) nm[--k] = 0;
    return up_eq(nm, name);
}

/* ------------------------------------------------------------------ install policy */
/* zip entry name (top folder already removed) -> 1 install (program file), 0 skip, -1 unsafe (the whole package is refused) */
static int up_policy(const char *rel, char *why, int whyn) {
    int n = up_len(rel);
    if (!n || n > 200 || rel[0] == '/' || rel[0] == '\\') { up_cpy(why, "unsafe path", whyn); return -1; }
    const char *c = rel; int comp_len = 0, depth = 0;
    for (const char *q = rel;; q++) {
        if (*q == '/' || !*q) {
            if (!*q && comp_len == 0 && q > rel && q[-1] == '/') break;   /* "dir/" */
            if (comp_len == 0 || (comp_len == 1 && c[0] == '.') || (comp_len == 2 && c[0] == '.' && c[1] == '.')) { up_cpy(why, "unsafe path", whyn); return -1; }
            if (!*q) break;
            depth++; c = q + 1; comp_len = 0; continue;
        }
        u8 ch = (u8)*q;
        if (ch < 0x20 || ch == ':' || ch == '\\' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch >= 0x7F) { up_cpy(why, "unsafe path", whyn); return -1; }
        comp_len++;
    }
    if (rel[n - 1] == '/') { up_cpy(why, "folder", whyn); return 0; }
    static const char *prot[] = {"game/", "savedata/", "config/", "cache/", "logs/"};
    for (int i = 0; i < 5; i++) { int pl = up_len(prot[i]), m = 1; for (int k = 0; k < pl; k++) if (up_lc(rel[k]) != prot[i][k]) { m = 0; break; } if (m) { up_cpy(why, "player data (never touched)", whyn); return 0; } }
    if (depth == 0) {
        if (up_ends_i(rel, ".exe") || up_ends_i(rel, ".udm") || up_ends_i(rel, ".txt") || up_ends_i(rel, ".md")) return 1;
        up_cpy(why, "not a program file", whyn); return 0;
    }
    if (depth == 1) {
        const char *cp = "compat/"; int m = 1; for (int k = 0; k < 7; k++) if (up_lc(rel[k]) != cp[k]) { m = 0; break; }
        if (m) { if (up_ends_i(rel, ".exe") || up_ends_i(rel, ".txt") || up_ends_i(rel, ".md") || up_ends_i(rel, ".bat")) return 1;
                 up_cpy(why, "not a program file", whyn); return 0; }
    }
    up_cpy(why, "not in a program folder", whyn); return 0;
}

/* ------------------------------------------------------------------ ZIP reader (no ZIP64, no encryption) */
typedef struct { char name[220]; char rel[220]; u32 crc, csize, usize, lofs; u16 method, flags; } UpZEnt;
static u32 rd16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
/* reads the central directory into e[] (at most maxn); returns the count, or -1 (err says why) */
static int up_zip_list(const u8 *z, u64 n, UpZEnt *e, int maxn, char *err, int errn) {
    if (n < 22) { up_cpy(err, "not a ZIP file (too short)", errn); return -1; }
    i64 eo = -1; for (i64 i = (i64)n - 22; i >= 0 && i >= (i64)n - 22 - 65535; i--) if (rd32(z + i) == 0x06054b50) { eo = i; break; }
    if (eo < 0) { up_cpy(err, "not a ZIP file (no end record: incomplete download?)", errn); return -1; }
    u32 cnt = rd16(z + eo + 10), cdsz = rd32(z + eo + 12), cdo = rd32(z + eo + 16);
    if (cnt == 0xFFFF || cdo == 0xFFFFFFFFu || (u64)cdo + cdsz > (u64)eo || cnt > (u32)maxn) { up_cpy(err, "ZIP directory unsupported or damaged", errn); return -1; }
    u64 p = cdo; int k = 0;
    for (u32 i = 0; i < cnt; i++) {
        if (p + 46 > (u64)eo || rd32(z + p) != 0x02014b50) { up_cpy(err, "ZIP directory damaged", errn); return -1; }
        UpZEnt *x = &e[k]; up_mset(x, 0, sizeof *x);
        x->flags = (u16)rd16(z + p + 8); x->method = (u16)rd16(z + p + 10); x->crc = rd32(z + p + 16); x->csize = rd32(z + p + 20); x->usize = rd32(z + p + 24);
        u32 nl = rd16(z + p + 28), xl = rd16(z + p + 30), cl = rd16(z + p + 32); x->lofs = rd32(z + p + 42);
        if (p + 46 + nl + xl + cl > (u64)eo || nl >= sizeof x->name || nl == 0) { up_cpy(err, "ZIP entry name damaged or too long", errn); return -1; }
        for (u32 c = 0; c < nl; c++) { char ch = (char)z[p + 46 + c]; x->name[c] = ch == '\\' ? '/' : ch; }
        x->name[nl] = 0;
        if (x->csize == 0xFFFFFFFFu || x->usize == 0xFFFFFFFFu || x->lofs == 0xFFFFFFFFu) { up_cpy(err, "ZIP64 entries are not supported", errn); return -1; }
        if (x->flags & 1) { up_cpy(err, "encrypted ZIP entries are not supported", errn); return -1; }
        p += 46 + nl + xl + cl; k++;
    }
    /* one common top folder ("UntilDawn_0.4.2/") is removed */
    int top = -1; char tn[220] = "";
    for (int i = 0; i < k; i++) { const char *s = e[i].name; int m = 0; while (s[m] && s[m] != '/') m++;
        if (!s[m]) { top = 0; break; }
        if (top < 0) { up_mcpy(tn, s, (u64)m + 1); tn[m + 1] = 0; top = m + 1; }
        else if (m + 1 != top || !up_starts(s, tn)) { top = 0; break; } }
    for (int i = 0; i < k; i++) up_cpy(e[i].rel, e[i].name + (top > 0 ? top : 0), sizeof e[i].rel);
    return k;
}
/* the entry's bytes, inflated and CRC-checked, into out (usize bytes); 0 ok */
static int up_zip_extract(const u8 *z, u64 n, const UpZEnt *x, u8 *out, char *err, int errn) {
    u64 lo = x->lofs;
    if (lo + 30 > n || rd32(z + lo) != 0x04034b50) { up_cpy(err, "ZIP entry header damaged", errn); return 1; }
    u64 d = lo + 30 + rd16(z + lo + 26) + rd16(z + lo + 28);
    if (d + x->csize > n) { up_cpy(err, "ZIP entry data cut off", errn); return 2; }
    if (x->method == 0) { if (x->csize != x->usize) { up_cpy(err, "ZIP stored entry size mismatch", errn); return 3; } up_mcpy(out, z + d, x->usize); }
    else if (x->method == 8) { u64 got = 0; int r = up_inflate(z + d, x->csize, out, x->usize, &got); if (r || got != x->usize) { up_cpy(err, "ZIP entry data damaged (inflate)", errn); return 4; } }
    else { up_cpy(err, "ZIP compression method not supported", errn); return 5; }
    if (up_crc32(out, x->usize) != x->crc) { up_cpy(err, "ZIP entry CRC mismatch (damaged)", errn); return 6; }
    return 0;
}

/* ------------------------------------------------------------------ files */
static int up_mkdirs(const char *path) {   /* every folder of path (the path itself is a folder) */
    char t[1200]; up_cpy(t, path, sizeof t); int n = up_len(t);
    for (int i = 1; i <= n; i++) if (t[i] == '/' || t[i] == '\\' || !t[i]) { char c = t[i]; t[i] = 0; if (!(i == 2 && t[1] == ':') && up_exists(t) != 2 && !up_mkdir1(t)) return 0; t[i] = c; }
    return 1;
}
static int up_mkparent(const char *file) { char t[1200]; up_cpy(t, file, sizeof t); int n = up_len(t); while (n && t[n - 1] != '/' && t[n - 1] != '\\') n--; if (!n) return 1; t[n - 1] = 0; return up_mkdirs(t); }
static int up_write_file(const char *p, const void *d, u64 n) { void *f = up_fopen_w(p); if (!f) return 0; int ok = up_fwrite(f, d, n); ok &= up_fclose(f, 1); return ok; }
static char *up_read_file(const char *p, u64 max, u64 *n) {
    void *f = up_fopen_r(p); if (!f) return 0;
    u64 cap = 1 << 16, k = 0; char *b = (char *)up_alloc(cap + 1); if (!b) { up_fclose(f, 0); return 0; }
    for (;;) {
        if (k == cap) { if (cap >= max) { up_free(b); up_fclose(f, 0); return 0; } u64 nc = cap * 2; char *nb = (char *)up_alloc(nc + 1); if (!nb) { up_free(b); up_fclose(f, 0); return 0; } up_mcpy(nb, b, k); up_free(b); b = nb; cap = nc; }
        i64 r = up_fread(f, b + k, cap - k); if (r < 0) { up_free(b); up_fclose(f, 0); return 0; } if (!r) break; k += (u64)r;
    }
    up_fclose(f, 0); b[k] = 0; *n = k; return b;
}
typedef struct { const char *dir; int ok; } UpRm;
static int up_rmtree_cb(void *ctx, const char *name, int isdir);
static int up_rmtree(const char *dir) {   /* everything below dir and dir itself; 1 if all went */
    if (!up_exists(dir)) return 1;
    UpRm r = {dir, 1}; up_list(dir, up_rmtree_cb, &r);
    if (!up_rmdir(dir)) r.ok = 0;
    return r.ok;
}
static int up_rmtree_cb(void *ctx, const char *name, int isdir) {
    UpRm *r = (UpRm *)ctx; char p[1200]; up_join(p, sizeof p, r->dir, name);
    if (isdir) { if (!up_rmtree(p)) r->ok = 0; } else if (!up_delete(p)) r->ok = 0;
    return 1;
}

/* ------------------------------------------------------------------ the updater's state */
enum { UP_MAXPLAN = 128 };
typedef struct { char rel[220]; int existed; char sha[65]; i64 size; } UpItem;
typedef struct {
    char base[1024];           /* the program folder (UntilDawn.exe) */
    char dir[1100];            /* base/Cache/update */
    char source[1024];         /* --update-source: a local folder in place of GitHub (tests) */
    char current[32];          /* the installed version */
    char skip[32];             /* update_skip_version */
    i64 sim_cut, sim_fail_replace, sim_crash_after;   /* fault injection (tests) */
    int nplan; UpItem plan[UP_MAXPLAN];
    char err[512];
} UpCtx;
static void up_ctx_dirs(UpCtx *c) { char t[1100]; up_join(t, sizeof t, c->base, "Cache/update"); up_cpy(c->dir, t, sizeof c->dir); }
static void up_sub(const UpCtx *c, const char *rel, char *o, int n) { up_join(o, n, c->dir, rel); }
static void up_err(UpCtx *c, const char *a, const char *b) { UpSb s = {c->err, 0, sizeof c->err}; c->err[0] = 0; sb_put(&s, a); if (b) sb_put(&s, b); up_log("ERROR: ", c->err, 0); }

/* ------------------------------------------------------------------ transport: GitHub over HTTPS, or a local folder (--update-source) */
typedef struct { UpCtx *c; u8 *mem; u64 n, cap; void *f; UpSha sha; i64 cut; int cut_hit, fail; u64 last_ui; const char *what; i64 total; } UpSink;
static int up_sink(void *ctx, const u8 *p, u32 n) {
    UpSink *s = (UpSink *)ctx;
    if (s->cut >= 0 && (i64)(s->n + n) > s->cut) { n = (u32)(s->cut - (i64)s->n); s->cut_hit = 1; }
    if (s->mem) { if (s->n + n > s->cap) { s->fail = 1; return 0; } up_mcpy(s->mem + s->n, p, n); }
    if (s->f && n && !up_fwrite(s->f, p, n)) { s->fail = 1; return 0; }
    up_sha_add(&s->sha, p, n); s->n += n;
    if (s->what && s->n - s->last_ui >= (256u << 10)) { s->last_ui = s->n; char t[160]; UpSb b = {t, 0, sizeof t}; t[0] = 0; sb_put(&b, s->what); sb_put(&b, " "); sb_num(&b, (i64)(s->n >> 10));
        if (s->total > 0) { sb_put(&b, " / "); sb_num(&b, s->total >> 10); } sb_put(&b, " KB"); up_progress(t); }
    return !s->cut_hit;
}
/* GET url into the sink; 0 ok. api: the release query (local source: latest.json) */
static int up_fetch(UpCtx *c, const char *url, int api, UpSink *s) {
    i64 clen = -1; char e[256] = "";
    up_sha_init(&s->sha); s->n = 0; s->cut_hit = 0; s->fail = 0;
    if (!up_starts(url, "https://")) { up_err(c, "refused (not HTTPS): ", url); return 1; }
    if (c->source[0]) {
        char p[1200]; up_join(p, sizeof p, c->source, api ? "latest.json" : up_basename(url));
        void *f = up_fopen_r(p); if (!f) { up_err(c, "local update source: no file ", p); return 2; }
        static u8 buf[1 << 16]; i64 r;
        while ((r = up_fread(f, buf, sizeof buf)) > 0) if (!up_sink(s, buf, (u32)r)) break;
        up_fclose(f, 0); clen = -1;
        if (s->cut_hit) { up_err(c, "download interrupted (simulated connection drop) at ", 0); up_logn("  bytes received: ", (i64)s->n, 0); return 3; }
    } else {
        int r = up_http_get(url, api, up_sink, s, &clen, e, sizeof e);
        if (s->cut_hit) { up_err(c, "download interrupted (simulated connection drop)", 0); return 3; }
        if (r) { up_err(c, "network: ", e); return 4; }
    }
    if (s->fail) { up_err(c, "could not store the downloaded data (disk full or too large)", 0); return 5; }
    if (clen >= 0 && (u64)clen != s->n) { up_err(c, "download incomplete (fewer bytes than announced)", 0); return 6; }
    return 0;
}

/* ------------------------------------------------------------------ step 1: what is the latest release? 0 = a newer one in *r, 1 = nothing to do, 2 = error */
static int up_check(UpCtx *c, UpRel *r) {
    UpSink s; up_mset(&s, 0, sizeof s); s.c = c; s.cut = -1;
    s.cap = UP_MAX_JSON; s.mem = (u8 *)up_alloc(s.cap + 1); if (!s.mem) { up_err(c, "out of memory", 0); return 2; }
    up_log("checking ", c->source[0] ? "the local update source " : UP_API_URL, c->source[0] ? c->source : 0);
    if (up_fetch(c, UP_API_URL, 1, &s)) { up_free(s.mem); return 2; }
    char e[160]; int pr = up_parse_release((const char *)s.mem, s.n, r, e, sizeof e);
    up_free(s.mem);
    if (pr) { up_err(c, "release information: ", e); up_log("  release tag: ", r->tag[0] ? r->tag : "(none)", 0); up_log("  assets: ", r->seen[0] ? r->seen : "(none)", 0);
              up_log("  expected: tag vX.Y.Z with assets " UP_ZIP_PREFIX "X.Y.Z.zip + .zip.sha256.txt under " UP_DL_PREFIX "<tag>/", 0, 0); return 2; }
    UpVer cur, lat; up_ver_parse(r->ver, &lat);
    if (!up_ver_parse(c->current, &cur)) { up_err(c, "the installed version is not known: ", c->current); return 2; }
    up_log("latest release ", r->tag, 0); up_log("installed version ", c->current, 0);
    if (up_ver_cmp(&lat, &cur) <= 0) { up_log("up to date", 0, 0); return 1; }
    UpVer sk; if (c->skip[0] && up_ver_parse(c->skip, &sk) && up_ver_cmp(&sk, &lat) == 0) { up_log("version ", r->ver, " skipped by the player (update_skip_version)"); return 1; }
    { /* loop guard: this version was installed by the updater before, but the program still reports the old one */
      char p[1200]; up_sub(c, "installed.txt", p, sizeof p); u64 n = 0; char *t = up_read_file(p, 4096, &n);
      if (t) { int same = up_starts(t, r->ver) && (t[up_len(r->ver)] == '\n' || t[up_len(r->ver)] == '\r' || !t[up_len(r->ver)]); up_free(t);
          if (same) { up_log("version ", r->ver, " was installed already but the program reports another version - not offered again (see NOTES)"); return 1; } } }
    up_log("a newer version is available: ", r->ver, 0);
    return 0;
}

/* ------------------------------------------------------------------ step 2: download + SHA-256; the zip in memory (*zip, *zn). 0 ok */
static int up_download(UpCtx *c, const UpRel *r, u8 **zip, u64 *zn) {
    *zip = 0; *zn = 0;
    char dl[1200], part[1200], fin[1200]; up_sub(c, "download", dl, sizeof dl);
    if (!up_mkdirs(dl)) { up_err(c, "cannot create ", dl); return 1; }
    up_join(part, sizeof part, dl, r->zip_name); up_join(fin, sizeof fin, dl, r->zip_name);
    { int k = up_len(part); up_cpy(part + k, ".part", (int)sizeof part - k); }
    /* the expected hash: GitHub's asset digest and/or the release's .sha256.txt; both present -> they must agree; none -> refused */
    char want[65] = "";
    if (r->zip_digest[0]) { up_cpy(want, r->zip_digest, sizeof want); up_log("expected SHA-256 (GitHub asset digest) ", want, 0); }
    if (r->sha_url[0]) {
        UpSink s; up_mset(&s, 0, sizeof s); s.c = c; s.cut = -1; s.cap = 4096; s.mem = (u8 *)up_alloc(s.cap + 1);
        if (!s.mem) { up_err(c, "out of memory", 0); return 1; }
        up_progress("Downloading the checksum ...");
        if (up_fetch(c, r->sha_url, 0, &s)) { up_free(s.mem); up_err(c, "the release lists a .sha256.txt that could not be downloaded - not installed", 0); return 2; }
        char txt[65]; int ok = up_parse_sha_txt((const char *)s.mem, s.n, r->zip_name, txt); up_free(s.mem);
        if (!ok) { up_err(c, "the .sha256.txt of the release is malformed or names another file", 0); return 3; }
        up_log("expected SHA-256 (.sha256.txt) ", txt, 0);
        if (want[0] && !up_eq(want, txt)) { up_err(c, "the asset digest and the .sha256.txt of the release disagree - not installed", 0); return 3; }
        up_cpy(want, txt, sizeof want);
    }
    if (!want[0]) { up_err(c, "no published SHA-256 for the package - not installed unverified", 0); return 3; }
    /* an earlier complete download with the right hash is used again */
    char have[65]; i64 hs = 0;
    if (up_exists(fin) == 1 && up_sha_file(fin, have, &hs) && up_eq(have, want)) up_log("using the earlier download ", fin, 0);
    else {
        if (up_exists(fin)) up_delete(fin);
        UpSink s; up_mset(&s, 0, sizeof s); s.c = c; s.cut = c->sim_cut > 0 ? c->sim_cut : -1; s.what = "Downloading the update:"; s.total = r->zip_size;
        s.f = up_fopen_w(part); if (!s.f) { up_err(c, "cannot write ", part); return 4; }
        int fr = up_fetch(c, r->zip_url, 0, &s);
        int cl = up_fclose(s.f, 1); s.f = 0;
        if (fr || !cl) { if (!fr) up_err(c, "cannot write ", part); up_delete(part); return 5; }
        up_sha_hex(&s.sha, have); up_log("received SHA-256 ", have, 0); up_logn("received bytes ", (i64)s.n, 0);
        if (r->zip_size >= 0 && (i64)s.n != r->zip_size) { up_err(c, "download size differs from the release's asset size (incomplete download)", 0); up_delete(part); return 6; }
        if (!up_eq(have, want)) { up_err(c, "SHA-256 mismatch: the download is damaged or not the published file", 0); up_delete(part); return 7; }
        if (!up_move(part, fin)) { up_err(c, "cannot rename ", part); up_delete(part); return 8; }
    }
    u64 n = 0; char *z = up_read_file(fin, UP_MAX_ZIP, &n);
    if (!z) { up_err(c, "cannot read ", fin); return 9; }
    /* hashed again in memory: what is unpacked is exactly what was checked */
    UpSha h; up_sha_init(&h); up_sha_add(&h, (u8 *)z, n); up_sha_hex(&h, have);
    if (!up_eq(have, want)) { up_free(z); up_delete(fin); up_err(c, "SHA-256 mismatch on re-read (disk problem?)", 0); return 10; }
    *zip = (u8 *)z; *zn = n; return 0;
}

/* ------------------------------------------------------------------ step 3: unpack the program files into Cache/update/stage (CRC-checked). 0 ok */
static int up_stage(UpCtx *c, const u8 *z, u64 zn) {
    static UpZEnt ents[UP_MAX_ENTRIES];
    char e[160];
    int k = up_zip_list(z, zn, ents, UP_MAX_ENTRIES, e, sizeof e);
    if (k < 0) { up_err(c, "package: ", e); return 1; }
    char st[1200]; up_sub(c, "stage", st, sizeof st);
    if (!up_rmtree(st) || !up_mkdirs(st)) { up_err(c, "cannot prepare ", st); return 2; }
    c->nplan = 0; u64 total = 0; int have_exe = 0;
    for (int i = 0; i < k; i++) {
        if (!ents[i].rel[0]) continue;   /* the top folder's own entry ("UntilDawn_0.4.1/": real release zips have folder entries) */
        char why[64] = ""; int pol = up_policy(ents[i].rel, why, sizeof why);
        if (pol < 0) { up_err(c, "package refused, unsafe entry name: ", ents[i].name); return 3; }
        if (pol == 0) { if (!up_ends_i(ents[i].rel, "/") && ents[i].rel[0]) up_log("skipped ", ents[i].rel, why[0] ? (up_eq(why, "player data (never touched)") ? "  (player data: never touched)" : "  (not a program file)") : 0); continue; }
        for (int j = 0; j < c->nplan; j++) if (up_ieq(c->plan[j].rel, ents[i].rel)) { up_err(c, "package refused, file twice: ", ents[i].rel); return 4; }
        if (c->nplan >= UP_MAXPLAN) { up_err(c, "package has too many files", 0); return 5; }
        total += ents[i].usize;
        if (total > UP_MAX_UNPACKED) { up_err(c, "package unpacks to more than the updater accepts", 0); return 6; }
        u8 *buf = (u8 *)up_alloc(ents[i].usize ? ents[i].usize : 1); if (!buf) { up_err(c, "out of memory", 0); return 7; }
        if (up_zip_extract(z, zn, &ents[i], buf, e, sizeof e)) { up_free(buf); up_err(c, "package: ", e); return 8; }
        char p[1200]; up_join(p, sizeof p, st, ents[i].rel);
        if (!up_mkparent(p) || !up_write_file(p, buf, ents[i].usize)) { up_free(buf); up_err(c, "cannot write ", p); return 9; }
        UpItem *it = &c->plan[c->nplan++]; up_mset(it, 0, sizeof *it);
        up_cpy(it->rel, ents[i].rel, sizeof it->rel); it->size = ents[i].usize;
        UpSha h; up_sha_init(&h); up_sha_add(&h, buf, ents[i].usize); up_sha_hex(&h, it->sha);
        up_free(buf);
        if (up_ieq(it->rel, "UntilDawn.exe")) have_exe = 1;
    }
    if (!have_exe) { up_err(c, "package refused: it has no UntilDawn.exe", 0); return 10; }
    /* UntilDawn.exe last: until the very end the old program is complete */
    for (int j = 0; j < c->nplan - 1; j++) if (up_ieq(c->plan[j].rel, "UntilDawn.exe")) { UpItem t = c->plan[j]; for (int m = j; m < c->nplan - 1; m++) c->plan[m] = c->plan[m + 1]; c->plan[c->nplan - 1] = t; break; }
    up_logn("staged program files: ", c->nplan, 0);
    return 0;
}

/* ------------------------------------------------------------------ step 4: install with journal; rollback; recovery */
static int g_up_trash_n;
static int up_to_trash(UpCtx *c, const char *path) {   /* a file that cannot simply be deleted (a running exe can be renamed) */
    if (up_delete(path)) return 1;
    char t[1200], nm[260]; up_sub(c, "trash", t, sizeof t); up_mkdirs(t);
    for (int k = 0; k < 1000; k++) { UpSb s = {nm, 0, sizeof nm}; nm[0] = 0; sb_num(&s, ++g_up_trash_n); sb_put(&s, "_"); sb_put(&s, up_basename(path));
        char p[1200]; up_join(p, sizeof p, t, nm); if (up_exists(p)) continue; return up_move(path, p); }
    return 0;
}
/* journal lines: "UDJ1", "from V", "to V", "e REL" (existed: the old file goes to backup/REL) / "n REL" (new), "begin", ..., "done" */
static int up_journal_write(UpCtx *c, const char *path, const char *to, int done) {
    static char t[64 << 10]; UpSb s = {t, 0, sizeof t}; t[0] = 0;
    sb_put(&s, "UDJ1\nfrom "); sb_put(&s, c->current); sb_put(&s, "\nto "); sb_put(&s, to); sb_put(&s, "\n");
    for (int i = 0; i < c->nplan; i++) { sb_put(&s, c->plan[i].existed ? "e " : "n "); sb_put(&s, c->plan[i].rel); sb_put(&s, "\n"); }
    sb_put(&s, "begin\n"); if (done) sb_put(&s, "done\n");
    if (s.n + 1 >= s.cap) return 0;
    return up_write_file(path, t, (u64)s.n);
}
/* puts back every file of the journal at jpath (the old files from backup/, new files moved away); 1 = all restored */
static int up_rollback_journal(UpCtx *c, const char *jpath, const char *bdir) {
    u64 n = 0; char *t = up_read_file(jpath, 1 << 20, &n);
    if (!t || !up_starts(t, "UDJ1")) { if (t) up_free(t); up_err(c, "rollback: journal unreadable ", jpath); return 0; }
    /* the lines in reverse order */
    int ok = 1, cnt = 0; char *lines[UP_MAXPLAN + 8]; char *p = t;
    while (*p && cnt < UP_MAXPLAN + 8) { lines[cnt++] = p; while (*p && *p != '\n') p++; if (*p) *p++ = 0; }
    for (int i = cnt - 1; i >= 0; i--) {
        char *l = lines[i]; int m = up_len(l); if (m && l[m - 1] == '\r') l[m - 1] = 0;
        if (!((l[0] == 'e' || l[0] == 'n') && l[1] == ' ')) continue;
        const char *rel = l + 2; char why[64];
        if (up_policy(rel, why, sizeof why) != 1) { up_err(c, "rollback: journal names a non-program path, ignored: ", rel); ok = 0; continue; }
        char tg[1200], bk[1200]; up_join(tg, sizeof tg, c->base, rel); up_join(bk, sizeof bk, bdir, rel);
        if (l[0] == 'e') {
            if (up_exists(bk) == 1) {
                if (up_exists(tg) && !up_to_trash(c, tg)) { up_err(c, "rollback: cannot move away ", tg); ok = 0; continue; }
                if (!up_mkparent(tg) || !up_move(bk, tg)) { up_err(c, "rollback: cannot restore ", tg); ok = 0; continue; }
                up_log("restored ", rel, 0);
            } else if (up_exists(tg) != 1) { up_err(c, "rollback: neither the old file nor its backup exists: ", rel); ok = 0; }
        } else if (up_exists(tg)) {
            if (!up_to_trash(c, tg)) { up_err(c, "rollback: cannot remove the new file ", tg); ok = 0; } else up_log("removed new file ", rel, 0);
        }
    }
    up_free(t);
    return ok;
}
/* journal left by an interrupted install: roll back. 0 nothing to do, 1 rolled back, -1 rollback incomplete */
static int up_recover(UpCtx *c) {
    char j[1200], b[1200]; up_sub(c, "journal.txt", j, sizeof j); up_sub(c, "backup", b, sizeof b);
    if (up_exists(j) != 1) return 0;
    u64 n = 0; char *t = up_read_file(j, 1 << 20, &n);
    int done = t && n >= 5 && (up_ends_i(t, "\ndone\n") || up_ends_i(t, "\ndone"));
    if (t) up_free(t);
    if (done) {   /* finished install whose journal was not yet renamed */
        char l[1200]; up_sub(c, "last_install.txt", l, sizeof l); up_move(j, l); return 0; }
    up_log("an interrupted install was found (Cache/update/journal.txt): rolling back", 0, 0);
    int ok = up_rollback_journal(c, j, b);
    if (ok) { char f[1200]; up_sub(c, "failed_install.txt", f, sizeof f); up_move(j, f); up_log("rollback complete: the previous version is restored", 0, 0); return 1; }
    up_log("ROLLBACK INCOMPLETE - the journal is kept; see Logs/update.log", 0, 0);
    return -1;
}
/* the staged files into the program folder. 0 = installed; 1 = failed and rolled back; 2 = failed, rollback incomplete */
static int up_apply(UpCtx *c, const char *to_ver) {
    char st[1200], bk[1200], jr[1200]; up_sub(c, "stage", st, sizeof st); up_sub(c, "backup", bk, sizeof bk); up_sub(c, "journal.txt", jr, sizeof jr);
    if (up_recover(c) < 0) { up_err(c, "an earlier install could not be rolled back; not installing", 0); return 2; }
    { char tr[1200]; up_sub(c, "trash", tr, sizeof tr); up_rmtree(tr); }   /* (files of running programs may stay; harmless) */
    if (!up_rmtree(bk) || !up_mkdirs(bk)) { up_err(c, "cannot empty the backup folder ", bk); return 1; }
    for (int i = 0; i < c->nplan; i++) {
        char tg[1200], sp[1200]; up_join(tg, sizeof tg, c->base, c->plan[i].rel); up_join(sp, sizeof sp, st, c->plan[i].rel);
        int ex = up_exists(tg); if (ex == 2) { up_err(c, "a folder is in the way of ", tg); return 1; }
        c->plan[i].existed = ex == 1;
        if (up_exists(sp) != 1) { up_err(c, "staged file missing ", sp); return 1; }
    }
    if (!up_journal_write(c, jr, to_ver, 0)) { up_err(c, "cannot write the journal ", jr); up_delete(jr); return 1; }
    up_progress("Installing the update ...");
    int fail = 0;
    for (int i = 0; i < c->nplan && !fail; i++) {
        UpItem *it = &c->plan[i];
        char tg[1200], sp[1200], bp[1200]; up_join(tg, sizeof tg, c->base, it->rel); up_join(sp, sizeof sp, st, it->rel); up_join(bp, sizeof bp, bk, it->rel);
        if (c->sim_fail_replace > 0 && i + 1 == c->sim_fail_replace) { up_err(c, "replace failed (simulated) for ", it->rel); fail = 1; break; }
        if (it->existed) { if (!up_mkparent(bp) || !up_move(tg, bp)) { up_err(c, "cannot move the old file to the backup: ", tg); fail = 1; break; } }
        if (!up_mkparent(tg) || !up_move(sp, tg)) { up_err(c, "cannot put the new file in place: ", tg); fail = 1; break; }
        char h[65]; i64 sz = -1;
        if (!up_sha_file(tg, h, &sz) || sz != it->size || !up_eq(h, it->sha)) { up_err(c, "the installed file does not match the package: ", tg); fail = 1; break; }
        up_log("installed ", it->rel, 0);
        if (c->sim_crash_after > 0 && i + 1 == c->sim_crash_after) { up_log("SIMULATED CRASH during the install", 0, 0); up_crash(); }
    }
    if (fail) {
        up_log("install failed - rolling back", 0, 0);
        int ok = up_rollback_journal(c, jr, bk);
        if (ok) { char f[1200]; up_sub(c, "failed_install.txt", f, sizeof f); up_move(jr, f); up_log("rollback complete: the previous version is restored", 0, 0); return 1; }
        up_log("ROLLBACK INCOMPLETE - the journal is kept for the next start", 0, 0); return 2;
    }
    {   /* done: the complete journal with "done" written aside, then renamed over (never a half-written journal) */
        char l[1200], tmp[1200]; up_sub(c, "last_install.txt", l, sizeof l); up_sub(c, "journal.tmp", tmp, sizeof tmp);
        if (!(up_journal_write(c, tmp, to_ver, 1) && up_move(tmp, jr))) up_log("warning: could not mark the journal done", 0, 0);
        else if (!up_move(jr, l)) up_log("warning: could not rename the finished journal (the next start does it)", 0, 0); }
    { char p[1200]; up_sub(c, "installed.txt", p, sizeof p); char t[64]; UpSb s = {t, 0, sizeof t}; t[0] = 0; sb_put(&s, to_ver); sb_put(&s, "\n"); up_write_file(p, t, (u64)s.n); }
    up_rmtree(st);
    up_log("installed version ", to_ver, " (the previous files are in Cache/update/backup)");
    return 0;
}
/* --restore-previous: the files of the last install back from Cache/update/backup (the player's own undo) */
static int up_restore_previous(UpCtx *c) {
    char l[1200], b[1200]; up_sub(c, "last_install.txt", l, sizeof l); up_sub(c, "backup", b, sizeof b);
    if (up_exists(l) != 1) { up_err(c, "no earlier install to undo", 0); return 1; }
    if (!up_rollback_journal(c, l, b)) return 2;
    char f[1200]; up_sub(c, "undone_install.txt", f, sizeof f); up_move(l, f);
    char p[1200]; up_sub(c, "installed.txt", p, sizeof p); up_delete(p);
    up_log("the previous version is restored", 0, 0); return 0;
}
