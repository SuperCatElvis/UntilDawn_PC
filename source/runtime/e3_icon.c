/* e3_icon.c - w38_rel: the window / taskbar icon from the PLAYER'S OWN game files (sce_sys/icon0.png), made at run time.
 * No artwork is distributed with the program and UntilDawn.exe is never changed (same bytes as shipped: the updater's checks
 * and SHA-256 sums stay valid). The icon0.png of the player's game folder (512 x 512, 8-bit RGB or RGBA, as Sony's format
 * requires) is decoded, scaled to 256 / 48 / 32 / 16 pixels and handed to Windows as icons; a copy as an .ico file is kept in
 * Cache\UntilDawn.ico (player data: never touched by the updater) for desktop shortcuts. If anything fails, Windows' default
 * icon is used and the log says why - the game is never affected. Inflate: the updater's RFC 1951 decoder (ud_update_core.h). */
static void ic_mcpy(void *d, const void *s, u64 n) { u8 *a = (u8 *)d; const u8 *b = (const u8 *)s; for (u64 i = 0; i < n; i++) a[i] = b[i]; }
static void ic_mset(void *d, int v, u64 n) { u8 *p = (u8 *)d; for (u64 i = 0; i < n; i++) p[i] = (u8)v; }
/* ------------------------------------------------------------------ inflate (RFC 1951), whole buffer to whole buffer */
typedef struct { const u8 *src; u64 slen, spos; u32 bb; int bc; u8 *dst; u64 dlen, dpos; int err; } IcInf;
typedef struct { short count[16]; short sym[288]; } IcHuff;
static u32 icinf_bits(IcInf *s, int n) {
    while (s->bc < n) { if (s->spos >= s->slen) { s->err = 1; return 0; } s->bb |= (u32)s->src[s->spos++] << s->bc; s->bc += 8; }
    u32 v = s->bb & ((1u << n) - 1); s->bb >>= n; s->bc -= n; return v;
}
static int icinf_build(IcHuff *h, const u8 *len, int n) {   /* <0: over-subscribed */
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1; for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return -1; }
    offs[1] = 0; for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return left;
}
static int icinf_decode(IcInf *s, const IcHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)icinf_bits(s, 1); if (s->err) return -1;
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int icinf_codes(IcInf *s, const IcHuff *lc, const IcHuff *dc) {
    static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = icinf_decode(s, lc); if (sym < 0) return -1;
        if (sym < 256) { if (s->dpos >= s->dlen) return -2; s->dst[s->dpos++] = (u8)sym; continue; }
        if (sym == 256) return 0;
        sym -= 257; if (sym >= 29) return -3;
        u64 len = (u64)lbase[sym] + icinf_bits(s, lext[sym]);
        int ds = icinf_decode(s, dc); if (ds < 0 || ds >= 30) return -4;
        u64 dist = (u64)dbase[ds] + icinf_bits(s, dext[ds]);
        if (s->err) return -1;
        if (dist > s->dpos) return -5;
        if (s->dpos + len > s->dlen) return -2;
        for (u64 k = 0; k < len; k++) { s->dst[s->dpos] = s->dst[s->dpos - dist]; s->dpos++; }
    }
}
static int ic_inflate(const u8 *src, u64 slen, u8 *dst, u64 dlen, u64 *out_n) {
    IcInf s; ic_mset(&s, 0, sizeof s); s.src = src; s.slen = slen; s.dst = dst; s.dlen = dlen;
    static IcHuff lc, dc; u8 lens[320];
    int last, r;
    do {
        last = (int)icinf_bits(&s, 1); int type = (int)icinf_bits(&s, 2); if (s.err) return -10;
        if (type == 0) {
            s.bb = 0; s.bc = 0;
            if (s.spos + 4 > s.slen) return -11;
            u32 len = s.src[s.spos] | (u32)s.src[s.spos + 1] << 8, nlen = s.src[s.spos + 2] | (u32)s.src[s.spos + 3] << 8; s.spos += 4;
            if ((len ^ 0xFFFF) != nlen) return -12;
            if (s.spos + len > s.slen) return -13;
            if (s.dpos + len > s.dlen) return -2;
            ic_mcpy(s.dst + s.dpos, s.src + s.spos, len); s.dpos += len; s.spos += len; r = 0;
        } else if (type == 1) {
            int i = 0; for (; i < 144; i++) lens[i] = 8; for (; i < 256; i++) lens[i] = 9; for (; i < 280; i++) lens[i] = 7; for (; i < 288; i++) lens[i] = 8;
            icinf_build(&lc, lens, 288); for (i = 0; i < 30; i++) lens[i] = 5; icinf_build(&dc, lens, 30);
            r = icinf_codes(&s, &lc, &dc);
        } else if (type == 2) {
            static const u8 order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            int nlen = (int)icinf_bits(&s, 5) + 257, ndist = (int)icinf_bits(&s, 5) + 1, ncode = (int)icinf_bits(&s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return -14;
            int i = 0; for (; i < ncode; i++) lens[order[i]] = (u8)icinf_bits(&s, 3); for (; i < 19; i++) lens[order[i]] = 0;
            if (s.err || icinf_build(&lc, lens, 19) != 0) return -15;
            i = 0;
            while (i < nlen + ndist) {
                int sym = icinf_decode(&s, &lc); if (sym < 0) return -16;
                if (sym < 16) { lens[i++] = (u8)sym; continue; }
                int len = 0, rep;
                if (sym == 16) { if (!i) return -17; len = lens[i - 1]; rep = 3 + (int)icinf_bits(&s, 2); }
                else if (sym == 17) rep = 3 + (int)icinf_bits(&s, 3);
                else rep = 11 + (int)icinf_bits(&s, 7);
                if (s.err || i + rep > nlen + ndist) return -18;
                while (rep--) lens[i++] = (u8)len;
            }
            if (!lens[256]) return -19;
            if (icinf_build(&lc, lens, nlen) < 0) return -20;
            if (icinf_build(&dc, lens + nlen, ndist) < 0) return -21;
            r = icinf_codes(&s, &lc, &dc);
        } else return -22;
        if (r) return r;
    } while (!last);
    *out_n = s.dpos; return 0;
}

/* PNG (8-bit RGB / RGBA, not interlaced) -> RGBA. Returns a malloc'd buffer or 0. */
static u8 *ic_png_rgba(const u8 *d, u64 n, u32 *pw, u32 *ph, const char **why) {
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    for (int i = 0; i < 8; i++) if (n < 33 || d[i] != sig[i]) { *why = "not a PNG file"; return 0; }
    u32 w = 0, h = 0; int ct = -1, bd = 0, il = 0; u64 zl = 0; u8 *z = 0; u64 zcap = 0;
    for (u64 p = 8; p + 12 <= n; ) {
        u32 L = (u32)d[p] << 24 | (u32)d[p + 1] << 16 | (u32)d[p + 2] << 8 | d[p + 3]; const u8 *t = d + p + 4, *c = d + p + 8;
        if (p + 12 + L > n) break;
        if (t[0] == 'I' && t[1] == 'H' && t[2] == 'D' && t[3] == 'R' && L >= 13) { w = (u32)c[0] << 24 | (u32)c[1] << 16 | (u32)c[2] << 8 | c[3]; h = (u32)c[4] << 24 | (u32)c[5] << 16 | (u32)c[6] << 8 | c[7]; bd = c[8]; ct = c[9]; il = c[12]; }
        else if (t[0] == 'I' && t[1] == 'D' && t[2] == 'A' && t[3] == 'T') { if (zl + L > zcap) { u64 nc = (zl + L) * 2; u8 *q = (u8 *)malloc(nc); if (!q) { free(z); *why = "out of memory"; return 0; } for (u64 i = 0; i < zl; i++) q[i] = z[i]; free(z); z = q; zcap = nc; } for (u32 i = 0; i < L; i++) z[zl++] = c[i]; }
        else if (t[0] == 'I' && t[1] == 'E' && t[2] == 'N' && t[3] == 'D') break;
        p += 12 + L;
    }
    if (!w || !h || w > 4096 || h > 4096 || bd != 8 || (ct != 2 && ct != 6) || il || zl < 6) { free(z); *why = "unsupported PNG layout"; return 0; }
    u32 bpp = ct == 6 ? 4 : 3; u64 stride = (u64)w * bpp, raw_n = (stride + 1) * h, got = 0;
    u8 *raw = (u8 *)malloc(raw_n); if (!raw) { free(z); *why = "out of memory"; return 0; }
    int r = ic_inflate(z + 2, zl - 6, raw, raw_n, &got); free(z);   /* zlib: 2-byte header, deflate data, 4-byte Adler-32 */
    if (r || got != raw_n) { free(raw); *why = "PNG data could not be decoded"; return 0; }
    u8 *out = (u8 *)malloc((u64)w * h * 4); if (!out) { free(raw); *why = "out of memory"; return 0; }
    for (u32 y = 0; y < h; y++) {   /* undo the per-row filters (in place, on the raw rows) */
        u8 *row = raw + y * (stride + 1) + 1, *prv = y ? raw + (y - 1) * (stride + 1) + 1 : 0; int f = row[-1];
        for (u64 x = 0; x < stride; x++) {
            int a = x >= bpp ? row[x - bpp] : 0, b = prv ? prv[x] : 0, c = (prv && x >= bpp) ? prv[x - bpp] : 0, v = row[x];
            if (f == 1) v += a; else if (f == 2) v += b; else if (f == 3) v += (a + b) / 2;
            else if (f == 4) { int pp = a + b - c, pa = pp > a ? pp - a : a - pp, pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp; v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c; }
            else if (f != 0) { free(raw); free(out); *why = "bad PNG filter"; return 0; }
            row[x] = (u8)v;
        }
        for (u32 x = 0; x < w; x++) { u8 *o = out + ((u64)y * w + x) * 4; const u8 *s = row + (u64)x * bpp; o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = bpp == 4 ? s[3] : 255; }
    }
    free(raw); *pw = w; *ph = h; return out;
}
/* one icon image: BITMAPINFOHEADER + 32-bit BGRA rows bottom-up + an all-zero AND mask (box-filtered from the RGBA source) */
static u8 *ic_dib(const u8 *rgba, u32 w, u32 h, u32 s, u32 *len) {
    u32 mask_row = ((s + 31) / 32) * 4; *len = 40 + s * s * 4 + mask_row * s;
    u8 *b = (u8 *)malloc(*len); if (!b) return 0; ic_mset(b, 0, *len);
    u32 *hd = (u32 *)b; hd[0] = 40; hd[1] = s; hd[2] = s * 2; b[12] = 1; b[14] = 32; hd[5] = s * s * 4;
    for (u32 y = 0; y < s; y++) for (u32 x = 0; x < s; x++) {
        u32 x0 = x * w / s, x1 = (x + 1) * w / s, y0 = y * h / s, y1 = (y + 1) * h / s; if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
        u64 acc[4] = {0, 0, 0, 0}, cnt = 0;
        for (u32 yy = y0; yy < y1; yy++) for (u32 xx = x0; xx < x1; xx++) { const u8 *p = rgba + ((u64)yy * w + xx) * 4; for (int k = 0; k < 4; k++) acc[k] += p[k]; cnt++; }
        u8 *o = b + 40 + ((u64)(s - 1 - y) * s + x) * 4; o[0] = (u8)(acc[2] / cnt); o[1] = (u8)(acc[1] / cnt); o[2] = (u8)(acc[0] / cnt); o[3] = (u8)(acc[3] / cnt);
    }
    return b;
}
static void *g_ic_big, *g_ic_small;
static const char *g_ic_state = "not tried";
/* build the icons from <game>/sce_sys/icon0.png; ico_path: where to keep the .ico copy (0 = none) */
static void ic_from_game(const char *game, const char *ico_path) {
    char p[1200]; u64 n = 0; path_join(p, game, "sce_sys/icon0.png");
    u8 *d = (u8 *)plat_read_file(p, &n); if (!d) { g_ic_state = "no sce_sys\\icon0.png in the game folder - Windows' default icon"; return; }
    const char *why = 0; u32 w = 0, h = 0; u8 *rgba = ic_png_rgba(d, n, &w, &h, &why);
    if (!rgba) { g_ic_state = why; return; }
    static const u32 sz[4] = {256, 48, 32, 16}; u8 *img[4]; u32 len[4]; int ok = 1;
    for (int i = 0; i < 4; i++) { img[i] = ic_dib(rgba, w, h, sz[i], &len[i]); if (!img[i]) ok = 0; }
    free(rgba);
    if (!ok) { g_ic_state = "out of memory"; return; }
    g_ic_big = CreateIconFromResourceEx(img[2], len[2], 1, 0x00030000, 32, 32, 0);
    g_ic_small = CreateIconFromResourceEx(img[3], len[3], 1, 0x00030000, 16, 16, 0);
    g_ic_state = g_ic_big ? "from the game folder's own icon (sce_sys\\icon0.png)" : "Windows refused the icon - default icon";
    if (ico_path && g_ic_big && plat_file_size(ico_path) < 0) {   /* the .ico copy for shortcuts, written once */
        u64 tot = 6 + 16 * 4; for (int i = 0; i < 4; i++) tot += len[i];
        u8 *f = (u8 *)malloc(tot);
        if (f) { ic_mset(f, 0, 6 + 16 * 4); f[2] = 1; f[4] = 4; u32 off = 6 + 16 * 4;
            for (int i = 0; i < 4; i++) { u8 *e = f + 6 + 16 * i; e[0] = (u8)(sz[i] & 255); e[1] = (u8)(sz[i] & 255); e[4] = 1; e[6] = 32; *(u32 *)(e + 8) = len[i]; *(u32 *)(e + 12) = off;
                for (u32 k = 0; k < len[i]; k++) f[off + k] = img[i][k]; off += len[i]; }
            void *o = plat_open_write(ico_path); if (o) { plat_write_bin(o, f, tot); plat_close(o); } free(f); }
    }
    for (int i = 0; i < 4; i++) free(img[i]);
    g_win_icon_big = g_ic_big; g_win_icon_small = g_ic_small;
}
