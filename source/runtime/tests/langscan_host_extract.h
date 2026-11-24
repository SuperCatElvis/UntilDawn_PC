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
