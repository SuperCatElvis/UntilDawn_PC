/* e5_nid.c - run-time NID computation (SHA-1 of name + Sony suffix, first 8 bytes reversed, base64 "+-")
 * so sceKernelDlsym can resolve any exported name in all_programs.prx. Included by e3_hle.c. */
typedef struct { u32 h[5]; u64 len; u8 buf[64]; u32 n; } Sha1;
static u32 rol32(u32 x, int r) { return (x << r) | (x >> (32 - r)); }
static void sha1_block(Sha1 *s, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; } else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol32(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) { s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0; s->len = 0; s->n = 0; }
static void sha1_add(Sha1 *s, const u8 *p, u64 n) { while (n--) { s->buf[s->n++] = *p++; s->len++; if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; } } }
static void sha1_done(Sha1 *s, u8 out[20]) {
    u64 bits = s->len * 8; u8 one = 0x80, zero = 0;
    sha1_add(s, &one, 1);
    while (s->n != 56) sha1_add(s, &zero, 1);
    for (int i = 7; i >= 0; i--) { u8 b = (u8)(bits >> (8 * i)); sha1_add(s, &b, 1); }
    for (int i = 0; i < 5; i++) { out[4 * i] = (u8)(s->h[i] >> 24); out[4 * i + 1] = (u8)(s->h[i] >> 16); out[4 * i + 2] = (u8)(s->h[i] >> 8); out[4 * i + 3] = (u8)s->h[i]; }
}
static void nid_of(const char *name, char out[12]) {
    static const u8 suffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
    static const char *al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    Sha1 s; u8 d[20], r[9];
    sha1_init(&s); sha1_add(&s, (const u8 *)name, (u64)str_len(name)); sha1_add(&s, suffix, 16); sha1_done(&s, d);
    for (int i = 0; i < 8; i++) r[i] = d[7 - i];
    r[8] = 0;
    /* base64 of 8 bytes -> 11 chars (last char uses 4 bits + padding) */
    int o = 0;
    for (int i = 0; i < 9 && o < 11; i += 3) {
        u32 v = (u32)r[i] << 16 | (u32)(i + 1 < 9 ? r[i + 1] : 0) << 8 | (u32)(i + 2 < 9 ? r[i + 2] : 0);
        for (int k = 0; k < 4 && o < 11; k++) out[o++] = al[(v >> (18 - 6 * k)) & 63];
    }
    out[11] = 0;
}
