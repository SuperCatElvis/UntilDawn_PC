/* e6_skipprompt.c - the movie skip prompt in the game's own UI (STATUS 86). Ours.
   The game's end-credits screen has a "Skip" prompt (Credits.swf: the Cross button pictures credits_i8 / credits_i44,
   the text SMG013_66585 in MelbourneUD). The credits movie is not loaded during the intro, so the same prompt is added
   to the movie that shows the movie legal line (scaleform/HUD/Screens/MovieLegal.swf, 4675 bytes), which is on screen
   over the intro and recap movies:
   - our three ActionScript classes (skipprompt_as\*.as, compiled to udskip_doabc.h): the prompt and its two pictures;
   - the two pictures as bitmaps, decoded at start-up from the player's own game files (the frontend movie's copies
     frontend_i8.gnf / frontend_if9.gnf, byte-identical to the credits ones: BC7, 1D-tiled);
   - an instance of the prompt on the movie's timeline.
   Nothing of the game is shipped: the movie is changed in memory, as the Scaleform loader reads it (eboot+0x189b20,
   its file object holds the data pointer at +0x18 and the size at +0x20). */
#include "e6_bc7.c"
#include "udskip_doabc.h"

#define SKP_LUMP "localcachepink/lumps/levels.game.thegame.leveldescription_lump_719c2cf3-0d42-686a-e2ad-5a6177add2eb.core"
#define SKP_LEGAL_LEN 4675u
#define SKP_LEGAL_FNV 0xf61a5a665d1d80b5ull

static u64 skp_fnv(const u8 *p, u64 n) { u64 h = 0xcbf29ce484222325ull; for (u64 i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; } return h; }

static u64 skp_lz4(const u8 *s, u64 n, u8 *d, u64 dn) {
    u64 i = 0, o = 0;
    while (i < n) {
        u32 tok = s[i++]; u64 ll = tok >> 4;
        if (ll == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ll += b; } while (b == 255); }
        if (o + ll > dn || i + ll > n) return o;
        for (u64 k = 0; k < ll; k++) d[o + k] = s[i + k];
        o += ll; i += ll;
        if (i >= n || o >= dn) break;
        if (i + 2 > n) return o;
        u64 off = (u64)s[i] | (u64)s[i + 1] << 8; i += 2;
        u64 ml = tok & 15;
        if (ml == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > o || o + ml > dn) return o;
        for (u64 k = 0; k < ml; k++) d[o + k] = d[o + k - off];
        o += ml;
    }
    return o;
}
/* read [off, off+n) of the lump's decompressed data. Layout (as tools\ud_assets\corescan.c reads it): a 32-byte header
   (u32 magic CB10C183, u32 block size, u64 total size, 16 bytes), a u32 packed size per block, then the blocks (LZ4, or
   stored as is when the packed size equals the block size) */
static int skp_lump_read(void *f, u64 off, u64 n, u8 *out) {
    u8 h[32]; if (hf_seek(f, 0, 0) < 0 || hf_read(f, h, 32) != 32) return 0;
    u32 magic, bsz; u64 total; mem_copy(&magic, h, 4); mem_copy(&bsz, h + 4, 4); mem_copy(&total, h + 8, 8);
    if (magic != 0xCB10C183u || !bsz || bsz > (4u << 20) || off + n > total) return 0;
    u32 nb = (u32)((total + bsz - 1) / bsz);
    u32 *cs = (u32 *)plat_alloc_rw((u64)nb * 4); if (!cs || hf_read(f, cs, (u64)nb * 4) != (u64)nb * 4) return 0;
    u8 *cbuf = (u8 *)plat_alloc_rw(bsz), *dbuf = (u8 *)plat_alloc_rw(bsz); if (!cbuf || !dbuf) return 0;
    u64 pos = 32 + (u64)nb * 4;
    for (u32 b = 0; b < nb; b++) {
        u64 dpos = (u64)b * bsz, want = b + 1 < nb ? bsz : total - dpos;
        if (cs[b] > bsz) return 0;
        if (dpos + want > off && dpos < off + n) {
            if (hf_seek(f, (i64)pos, 0) < 0 || hf_read(f, cbuf, cs[b]) != cs[b]) return 0;
            if (cs[b] == want) mem_copy(dbuf, cbuf, want); else if (skp_lz4(cbuf, cs[b], dbuf, want) != want) return 0;
            u64 a = off > dpos ? off - dpos : 0, e = (off + n < dpos + want ? off + n : dpos + want) - dpos;
            mem_copy(out + (dpos + a - off), dbuf + a, e - a);
        }
        if (dpos >= off + n) break;
        pos += cs[b];
    }
    return 1;
}
/* one texture entry of the frontend movie resource: 256-byte name, u64 size, then the GNF file: header (16), T# (32),
   BC7 data. e = the whole entry. */
static u8 *skp_decode_entry(const u8 *e, u64 elen, const char *name, u32 *w, u32 *h) {
    if (elen < 0x108 + 48) return 0;
    for (int i = 0; name[i]; i++) if (e[i] != (u8)name[i]) return 0;
    if (e[0x108] != 'G' || e[0x109] != 'N' || e[0x10a] != 'F') return 0;
    u32 T[8]; mem_copy(T, e + 0x108 + 16, 32);
    u32 fmt = (T[1] >> 20) & 0x3f, W = (T[2] & 0x3fff) + 1, H = ((T[2] >> 14) & 0x3fff) + 1, tm = (T[3] >> 20) & 0x1f, pitch = ((T[4] >> 13) & 0x3fff) + 1;
    u64 dsize; mem_copy(&dsize, e + 0x100, 8);
    if (fmt != 41 || W > 512 || H > 512 || dsize < 48 || 0x108 + dsize > elen) return 0;   /* BC7 */
    u32 bw = (W + 3) / 4, bh = (H + 3) / 4;
    u8 *lin = (u8 *)plat_alloc_rw((u64)bw * bh * 16), *rgba = (u8 *)plat_alloc_rw((u64)W * H * 4);
    if (!lin || !rgba) return 0;
    detile(e + 0x108 + 48, dsize - 48, lin, bw, bh, 1, 128, tm, pitch / 4);
    for (u32 by = 0; by < bh; by++) for (u32 bx = 0; bx < bw; bx++) {
        u8 px[64]; bc7_decode_block(lin + ((u64)by * bw + bx) * 16, px);
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) { u32 X = bx * 4 + x, Y = by * 4 + y; if (X < W && Y < H) mem_copy(rgba + ((u64)Y * W + X) * 4, px + (y * 4 + x) * 4, 4); }
    }
    *w = W; *h = H; return rgba;
}
static u8 *skp_icon(void *f, u64 off, const char *name, u32 *w, u32 *h) {
    u8 hd[0x108]; if (!skp_lump_read(f, off, sizeof hd, hd)) return 0;
    u64 dsize; mem_copy(&dsize, hd + 0x100, 8); if (dsize < 48 || dsize > (1u << 20)) return 0;
    u8 *e = (u8 *)plat_alloc_rw(0x108 + dsize); if (!e || !skp_lump_read(f, off, 0x108 + dsize, e)) return 0;
    return skp_decode_entry(e, 0x108 + dsize, name, w, h);
}
/* --- SWF tag writer */
typedef struct { u8 *p; u64 n, cap; } SkpBuf;
static void skp_put(SkpBuf *b, const void *s, u64 n) { if (b->n + n <= b->cap) mem_copy(b->p + b->n, s, n); b->n += n; }
static void skp_u8(SkpBuf *b, u32 v) { u8 c = (u8)v; skp_put(b, &c, 1); }
static void skp_u16(SkpBuf *b, u32 v) { skp_u8(b, v & 255); skp_u8(b, v >> 8); }
static void skp_u32(SkpBuf *b, u32 v) { skp_u16(b, v & 0xffff); skp_u16(b, v >> 16); }
static void skp_taghdr(SkpBuf *b, u32 code, u64 len) { skp_u16(b, code << 6 | 63); skp_u32(b, (u32)len); }
static u64 skp_zlib_size(u64 n) { return 2 + (n + 65534) / 65535 * 5 + n + 4; }
static void skp_zlib_stored(SkpBuf *b, const u8 *s, u64 n) {   /* zlib stream of stored (uncompressed) deflate blocks */
    skp_u8(b, 0x78); skp_u8(b, 0x01);
    u32 a = 1, c = 0; for (u64 i = 0; i < n; i++) { a = (a + s[i]) % 65521; c = (c + a) % 65521; }
    u64 i = 0; do { u32 k = (u32)(n - i > 65535 ? 65535 : n - i); skp_u8(b, i + k >= n); skp_u16(b, k); skp_u16(b, ~k & 0xffff); skp_put(b, s + i, k); i += k; } while (i < n);
    skp_u8(b, c >> 8); skp_u8(b, c & 255); skp_u8(b, a >> 8); skp_u8(b, a & 255);
}
static void skp_bits_lossless2(SkpBuf *b, u32 id, const u8 *rgba, u32 w, u32 h) {   /* DefineBitsLossless2, 32-bit premultiplied ARGB */
    u64 n = (u64)w * h * 4; u8 *argb = (u8 *)plat_alloc_rw(n); if (!argb) return;
    for (u64 i = 0; i < (u64)w * h; i++) { u32 A = rgba[i * 4 + 3]; argb[i * 4] = (u8)A; for (int c = 0; c < 3; c++) argb[i * 4 + 1 + c] = (u8)((rgba[i * 4 + c] * A + 127) / 255); }
    skp_taghdr(b, 36, 7 + skp_zlib_size(n)); skp_u16(b, id); skp_u8(b, 5); skp_u16(b, w); skp_u16(b, h); skp_zlib_stored(b, argb, n);
}
static int skp_find_tag(const u8 *d, u64 len, u32 want, u64 *at) {   /* first top-level tag of this code */
    u32 nb = d[8] >> 3; u64 p = 8 + (5 + 4 * nb + 7) / 8 + 4;
    while (p + 2 <= len) {
        u32 hh = (u32)d[p] | (u32)d[p + 1] << 8, code = hh >> 6; u64 L = hh & 63, q = p + 2;
        if (L == 63) { if (q + 4 > len) return 0; L = (u64)d[q] | (u64)d[q + 1] << 8 | (u64)d[q + 2] << 16 | (u64)d[q + 3] << 24; q += 4; }
        if (code == want) { *at = p; return 1; }
        if (code == 0) return 0;
        p = q + L;
    }
    return 0;
}
/* --- the prompt's symbols, written the way the credits screen's are (Credits.swf: skip container, button prompt) */
typedef struct { SkpBuf *b; u32 acc, n; } SkpBits;
static void skb_put(SkpBits *w, u32 v, u32 bits) { while (bits--) { w->acc = (w->acc << 1) | ((v >> bits) & 1); if (++w->n == 8) { skp_u8(w->b, w->acc); w->acc = 0; w->n = 0; } } }
static void skb_flush(SkpBits *w) { if (w->n) { skp_u8(w->b, w->acc << (8 - w->n)); w->acc = 0; w->n = 0; } }
static u32 skb_sbits(i32 v) { u32 n = 1; while (n < 31 && (v < -(1 << (n - 1)) || v >= (1 << (n - 1)))) n++; return n; }
static void skb_rect(SkpBits *w, i32 x0, i32 x1, i32 y0, i32 y1) {
    u32 n = skb_sbits(x0); if (skb_sbits(x1) > n) n = skb_sbits(x1); if (skb_sbits(y0) > n) n = skb_sbits(y0); if (skb_sbits(y1) > n) n = skb_sbits(y1);
    skb_put(w, n, 5); skb_put(w, (u32)x0, n); skb_put(w, (u32)x1, n); skb_put(w, (u32)y0, n); skb_put(w, (u32)y1, n); skb_flush(w);
}
static void skb_matrix(SkpBits *w, i32 scale16, i32 tx, i32 ty) {   /* scale16 = 0: no scale */
    if (scale16) { u32 n = skb_sbits(scale16); skb_put(w, 1, 1); skb_put(w, n, 5); skb_put(w, (u32)scale16, n); skb_put(w, (u32)scale16, n); } else skb_put(w, 0, 1);
    skb_put(w, 0, 1);   /* no rotate */
    u32 n = skb_sbits(tx); if (skb_sbits(ty) > n) n = skb_sbits(ty); if (!tx && !ty) n = 0;
    skb_put(w, n, 5); if (n) { skb_put(w, (u32)tx, n); skb_put(w, (u32)ty, n); }
    skb_flush(w);
}
static void skb_moveto(SkpBits *w, i32 x, i32 y, u32 fill1) {   /* style change: move to (x, y), fill style 1 = fill1 (2 fill bits) */
    skb_put(w, 0, 1); skb_put(w, 0, 1); skb_put(w, 0, 1); skb_put(w, 1, 1); skb_put(w, 0, 1); skb_put(w, 1, 1);
    u32 n = skb_sbits(x); if (skb_sbits(y) > n) n = skb_sbits(y);
    skb_put(w, n, 5); skb_put(w, (u32)x, n); skb_put(w, (u32)y, n); skb_put(w, fill1, 2);
}
static void skb_line(SkpBits *w, i32 dx, i32 dy) {   /* straight edge, horizontal or vertical */
    i32 v = dx ? dx : dy; u32 n = skb_sbits(v); if (n < 2) n = 2;
    skb_put(w, 1, 1); skb_put(w, 1, 1); skb_put(w, n - 2, 4); skb_put(w, 0, 1); skb_put(w, dx ? 0 : 1, 1); skb_put(w, (u32)v, n);
}
static void skb_square(SkpBits *w, i32 x, i32 y, i32 s, u32 fill) { skb_moveto(w, x, y, fill); skb_line(w, s, 0); skb_line(w, 0, s); skb_line(w, -s, 0); skb_line(w, 0, -s); }
/* the image names our movie asks the engine for; their bytes are the frontend's own copies of the credits pictures */
#define SKP_IMG_CIRCLE "UDSkip_I8.GNF"
#define SKP_IMG_CROSS "UDSkip_I44.GNF"
static u8 *g_skp_gnf[2]; static u32 g_skp_gnf_len[2];
static void skp_ext_image(SkpBuf *b, u32 id, u32 w, u32 h, const char *name) {   /* GFX DefineExternalImage2 (as Credits.swf's) */
    u32 nl = (u32)str_len(name);
    skp_taghdr(b, 1009, 4 + 2 + 2 + 2 + 1 + 1 + nl); skp_u32(b, id); skp_u16(b, 26); skp_u16(b, w); skp_u16(b, h); skp_u8(b, 0); skp_u8(b, nl); skp_put(b, name, nl);
}
static void skp_symbols(SkpBuf *b) {
    skp_ext_image(b, 900, 200, 200, SKP_IMG_CIRCLE);
    skp_ext_image(b, 901, 150, 150, SKP_IMG_CROSS);
    {   /* 906: the Cross button - the round base (52.25 px) and the X over it, each a square filled with its picture */
        SkpBuf t = {b->p ? b->p + b->n + 6 : 0, 0, b->cap > b->n + 6 ? b->cap - b->n - 6 : 0}; SkpBits w = {&t, 0, 0};
        skp_u16(&t, 906); skb_rect(&w, 0, 1045, -520, 525);
        skp_u8(&t, 1);
        skp_u8(&t, 0x41); skp_u16(&t, 900); skb_matrix(&w, 342420, 0, -520);      /* 5.2249146 twips per texel (16.16) */
        skp_u8(&t, 0); skp_u8(&t, 0x20);                                          /* no line styles; 2 fill bits, 0 line bits */
        skb_square(&w, 0, -520, 1045, 1);
        /* the X in a new style layer (as Credits.swf does): in the same layer its square would cut a hole into the
           round base (t97/t98: the base looked see-through) */
        skb_put(&w, 0, 1); skb_put(&w, 1, 1); skb_put(&w, 0, 4); skb_flush(&w);   /* style change: new styles only */
        skp_u8(&t, 1); skp_u8(&t, 0x41); skp_u16(&t, 901); skb_matrix(&w, 342420, 131, -385);
        skp_u8(&t, 0);
        skb_put(&w, 2, 4); skb_put(&w, 0, 4);                                     /* 2 fill bits, 0 line bits */
        skb_square(&w, 131, -385, 784, 1);
        skb_put(&w, 0, 6); skb_flush(&w);                                         /* end of shape */
        skp_taghdr(b, 22, t.n); b->n += t.n; }
    skp_taghdr(b, 39, 4 + 11 + 2 + 2); skp_u16(b, 907); skp_u16(b, 1);             /* 907: button sprite (UDSkipButton) */
    skp_taghdr(b, 26, 5); skp_u8(b, 0x02); skp_u16(b, 1); skp_u16(b, 906); skp_u16(b, 1 << 6); skp_u16(b, 0);
    skp_taghdr(b, 75, 2 + 1 + 1 + 1 + 12 + 2); skp_u16(b, 903); skp_u8(b, 0x04); skp_u8(b, 1); skp_u8(b, 12); skp_put(b, "MelbourneUD", 12); skp_u16(b, 0);   /* the font, by name (the game's font library has it) */
    {   /* 904: the text field - MelbourneUD 27 white, as the credits prompt's */
        static const char *html = "<p align=\"left\"><font face=\"MelbourneUD\" size=\"27\" color=\"#ffffff\" letterSpacing=\"0.00\" kerning=\"1\">Skip</font></p>";
        SkpBuf t = {b->p ? b->p + b->n + 6 : 0, 0, b->cap > b->n + 6 ? b->cap - b->n - 6 : 0}; SkpBits w = {&t, 0, 0};
        skp_u16(&t, 904); skb_rect(&w, -40, 14459, -40, 1040);
        skp_u8(&t, 0xed); skp_u8(&t, 0x32);                                       /* text, wrap, multiline, read-only, colour, font / layout, no select, html */
        skp_u16(&t, 903); skp_u16(&t, 540); skp_u32(&t, 0xffffffffu);
        skp_u8(&t, 0); skp_u16(&t, 0); skp_u16(&t, 0); skp_u16(&t, 0); skp_u16(&t, 40);
        skp_u8(&t, 0); skp_put(&t, html, str_len(html) + 1);
        skp_taghdr(b, 37, t.n); b->n += t.n; }
    {   /* 905: text sprite (UDSkipText) with the field named textField at (2, -14) px */
        SkpBuf t = {b->p ? b->p + b->n + 6 : 0, 0, b->cap > b->n + 6 ? b->cap - b->n - 6 : 0}; SkpBits w = {&t, 0, 0};
        skp_u16(&t, 905); skp_u16(&t, 1);
        SkpBuf po = {t.p ? t.p + t.n + 6 : 0, 0, t.cap > t.n + 6 ? t.cap - t.n - 6 : 0}; SkpBits pw = {&po, 0, 0}; (void)w;
        skp_u8(&po, 0x26); skp_u16(&po, 1); skp_u16(&po, 904); skb_matrix(&pw, 0, 40, -280); skp_put(&po, "textField", 10);
        skp_taghdr(&t, 26, po.n); t.n += po.n;
        skp_u16(&t, 1 << 6); skp_u16(&t, 0);
        skp_taghdr(b, 39, t.n); b->n += t.n; }
}
/* the host movies: [0] MovieLegal (on screen over the intro movies), [1] VideoOverlayText (over the recap ones), [2] (t107j)
   Subtitles (scaleform/HUD/Screens/Subtitles.swf, 5006 bytes: a MainTimeline that adds its subtitle text fields with
   addChild; nothing on its timeline) - for the movies neither of the first two is drawn over (butterfly_tutorial,
   1_1_Intro_shot, Intro_BusRide). Which one shows the prompt is decided at the time (e3_loader.c sf_update_probe). */
#define SKP_HOSTS 3
static const u32 g_skp_len[SKP_HOSTS] = {SKP_LEGAL_LEN, 36393u, 5006u};
static const u64 g_skp_fnvs[SKP_HOSTS] = {SKP_LEGAL_FNV, 0x30ce43a9f99f862bull, 0x233ae5110a2af565ull};
static const char *const g_skp_host_name[SKP_HOSTS] = {"the legal-line movie (MovieLegal)", "the overlay-caption movie (VideoOverlayText)", "the subtitle movie (Subtitles)"};
static u8 *g_skp_movie[SKP_HOSTS]; static u32 g_skp_movie_len[SKP_HOSTS];   /* g_skp_state (e6_vk.c): 0 not tried, 1 ready, -1 failed */
static int g_skp_pics;   /* 0 not tried, 1 ok, -1 failed */
static u8 *skp_build(const u8 *orig, u32 olen, u32 *outlen);
static int skp_which(const u8 *orig, u32 olen) { for (int i = 0; i < SKP_HOSTS; i++) if (olen == g_skp_len[i] && skp_fnv(orig, olen) == g_skp_fnvs[i]) return i; return -1; }
static int skp_is_host_len(u64 len) { for (int i = 0; i < SKP_HOSTS; i++) if (len == g_skp_len[i]) return 1; return 0; }
/* the changed movie for this original (or 0); built once per host movie */
static const u8 *skp_prepare(const u8 *orig, u32 olen, u32 *outlen) {
    int k = skp_which(orig, olen); if (k < 0) return 0;
    if (g_skp_movie[k]) { *outlen = g_skp_movie_len[k]; return g_skp_movie[k]; }
    if (!g_skp_pics) {
        g_skp_pics = -1;
        char path[1100]; path_join(path, g_game_dir, SKP_LUMP);
        void *f = hf_open_rb(path); if (!f) { say("  skip prompt: cannot open the frontend lump - prompt not added\n"); return 0; }
        static const u64 at[2] = {0x41b0dc5, 0x5036ccd}; static const char *nm[2] = {"frontend_i8.gnf", "frontend_if9.gnf"};
        for (int i = 0; i < 2; i++) {
            u8 hd[0x108 + 4]; if (!skp_lump_read(f, at[i], sizeof hd, hd)) break;
            int ok = 1; for (int k = 0; nm[i][k]; k++) if (hd[k] != (u8)nm[i][k]) ok = 0;
            u64 dsize; mem_copy(&dsize, hd + 0x100, 8);
            if (!ok || hd[0x108] != 'G' || hd[0x109] != 'N' || hd[0x10a] != 'F' || dsize < 48 || dsize > (1u << 20)) break;
            u8 *g = (u8 *)plat_alloc_rw(dsize); if (!g || !skp_lump_read(f, at[i] + 0x108, dsize, g)) break;
            g_skp_gnf[i] = g; g_skp_gnf_len[i] = (u32)dsize;
        }
        hf_close(f);
        if (!g_skp_gnf[0] || !g_skp_gnf[1]) { say("  skip prompt: the Cross button pictures were not found in the game files - prompt not added\n"); return 0; }
        g_skp_pics = 1;
    }
    if (g_skp_pics < 0) return 0;
    u32 n = 0; u8 *m = skp_build(orig, olen, &n); if (!m) return 0;
    g_skp_movie[k] = m; g_skp_movie_len[k] = n; g_skp_state = 1;
    say("  skip prompt: added to "); say(g_skp_host_name[k]); say("\n");
    *outlen = n; return m;
}
static u8 *skp_build(const u8 *orig, u32 olen, u32 *outlen) {
    u64 at; if (!skp_find_tag(orig, olen, 1, &at)) { say("  skip prompt: unexpected movie layout - prompt not added\n"); return 0; }
    u64 cap = olen + sizeof g_udskip_doabc + 2048;
    SkpBuf b = {(u8 *)plat_alloc_rw(cap), 0, cap}; if (!b.p) return 0;
    skp_put(&b, orig, at);                                    /* header and frame 1 up to its ShowFrame */
    skp_symbols(&b);
    skp_taghdr(&b, 82, sizeof g_udskip_doabc); skp_put(&b, g_udskip_doabc, sizeof g_udskip_doabc);
    skp_taghdr(&b, 39, 4 + 2 + 2); skp_u16(&b, 902); skp_u16(&b, 1); skp_u16(&b, 1 << 6); skp_u16(&b, 0);   /* DefineSprite: ShowFrame, End */
    { static const char *nm[3] = {"UDSkipPrompt", "UDSkipButton", "UDSkipText"}; static const u16 id[3] = {902, 907, 905}; u64 L = 2; for (int i = 0; i < 3; i++) L += 2 + str_len(nm[i]) + 1;
      skp_taghdr(&b, 76, L); skp_u16(&b, 3); for (int i = 0; i < 3; i++) { skp_u16(&b, id[i]); skp_put(&b, nm[i], str_len(nm[i]) + 1); } }
    skp_taghdr(&b, 26, 5); skp_u8(&b, 0x02); skp_u16(&b, 900); skp_u16(&b, 902);   /* PlaceObject2: character 902 at depth 900 */
    skp_put(&b, orig + at, olen - at);                        /* the rest, unchanged */
    if (b.n > cap) return 0;
    u32 total = (u32)b.n; mem_copy(b.p + 4, &total, 4);
    *outlen = total; return b.p;
}
