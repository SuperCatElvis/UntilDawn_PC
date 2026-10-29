/* e6_bc7.c - BC7 block decoder (own implementation of the public BC7 format description; the partition and anchor
   tables are the format's fixed constants). Decodes one 16-byte block to 4x4 RGBA8. Portable C, no libc. */
static const u16 bc7_p2[64] = {
    0xcccc,0x8888,0xeeee,0xecc8,0xc880,0xfeec,0xfec8,0xec80,0xc800,0xffec,0xfe80,0xe800,0xffe8,0xff00,0xfff0,0xf000,
    0xf710,0x008e,0x7100,0x08ce,0x008c,0x7310,0x3100,0x8cce,0x088c,0x3110,0x6666,0x366c,0x17e8,0x0ff0,0x718e,0x399c,
    0xaaaa,0xf0f0,0x5a5a,0x33cc,0x3c3c,0x55aa,0x9696,0xa55a,0x73ce,0x13c8,0x324c,0x3bdc,0x6996,0xc33c,0x9966,0x0660,
    0x0272,0x04e4,0x4e40,0x2720,0xc936,0x936c,0x39c6,0x639c,0x9336,0x9cc6,0x817e,0xe718,0xccf0,0x0fcc,0x7744,0xee22};
static const u32 bc7_p3[64] = {
    0xaa685050,0x6a5a5040,0x5a5a4200,0x5450a0a8,0xa5a50000,0xa0a05050,0x5555a0a0,0x5a5a5050,0xaa550000,0xaa555500,0xaaaa5500,0x90909090,0x94949494,0xa4a4a4a4,0xa9a59450,0x2a0a4250,
    0xa5945040,0x0a425054,0xa5a5a500,0x55a0a0a0,0xa8a85454,0x6a6a4040,0xa4a45000,0x1a1a0500,0x0050a4a4,0xaaa59090,0x14696914,0x69691400,0xa08585a0,0xaa821414,0x50a4a450,0x6a5a0200,
    0xa9a58000,0x5090a0a8,0xa8a09050,0x24242424,0x00aa5500,0x24924924,0x24499224,0x50a50a50,0x500aa550,0xaaaa4444,0x66660000,0xa5a0a5a0,0x50a050a0,0x69286928,0x44aaaa44,0x66666600,
    0xaa444444,0x54a854a8,0x95809580,0x96969600,0xa85454a8,0x80959580,0xaa141414,0x96960000,0xaaaa1414,0xa05050a0,0xa0a5a5a0,0x96000000,0x40804080,0xa9a8a9a8,0xaaaaaa44,0x2a4a5254};
static const u8 bc7_a2[64] = {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};
static const u8 bc7_a3a[64] = {3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3};
static const u8 bc7_a3b[64] = {15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8};
static const u8 bc7_w2[4] = {0, 21, 43, 64}, bc7_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64}, bc7_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
typedef struct { const u8 *b; u32 pos; } Bc7Bits;
static u32 bc7_get(Bc7Bits *s, u32 n) { u32 v = 0; for (u32 i = 0; i < n; i++, s->pos++) v |= (u32)((s->b[s->pos >> 3] >> (s->pos & 7)) & 1) << i; return v; }
static u8 bc7_interp(u32 a, u32 b, u32 idx, u32 bits) {
    u32 w = bits == 2 ? bc7_w2[idx] : bits == 3 ? bc7_w3[idx] : bc7_w4[idx];
    return (u8)(((64 - w) * a + w * b + 32) >> 6);
}
static void bc7_decode_block(const u8 *blk, u8 *out /* 64 bytes RGBA, row-major 4x4 */) {
    static const u8 NS[8] = {3, 2, 3, 2, 1, 1, 1, 2}, PB[8] = {4, 6, 6, 6, 0, 0, 0, 6}, RB[8] = {0, 0, 0, 0, 2, 2, 0, 0}, ISB[8] = {0, 0, 0, 0, 1, 0, 0, 0};
    static const u8 CB[8] = {4, 6, 5, 7, 5, 7, 7, 5}, AB[8] = {0, 0, 0, 0, 6, 8, 7, 5}, EPB[8] = {1, 0, 0, 1, 0, 0, 1, 1}, SPB[8] = {0, 1, 0, 0, 0, 0, 0, 0};
    static const u8 IB[8] = {3, 3, 2, 2, 2, 2, 4, 2}, IB2[8] = {0, 0, 0, 0, 3, 2, 0, 0};
    u32 mode = 0; while (mode < 8 && !(blk[0] & (1u << mode))) mode++;
    if (mode >= 8) { for (int i = 0; i < 64; i++) out[i] = 0; return; }
    Bc7Bits s = {blk, mode + 1};
    u32 part = bc7_get(&s, PB[mode]), rot = bc7_get(&s, RB[mode]), isel = bc7_get(&s, ISB[mode]);
    u32 ns = NS[mode], cb = CB[mode], ab = AB[mode];
    u32 ep[6][4];   /* endpoints [subset*2 + 0/1][rgba] */
    for (u32 c = 0; c < 3; c++) for (u32 e = 0; e < ns * 2; e++) ep[e][c] = bc7_get(&s, cb);
    for (u32 e = 0; e < ns * 2; e++) ep[e][3] = ab ? bc7_get(&s, ab) : 255;
    u32 pbits[6] = {0};
    if (EPB[mode]) for (u32 e = 0; e < ns * 2; e++) pbits[e] = bc7_get(&s, 1);
    if (SPB[mode]) for (u32 k = 0; k < ns; k++) { u32 p = bc7_get(&s, 1); pbits[k * 2] = pbits[k * 2 + 1] = p; }
    int haspb = EPB[mode] || SPB[mode];
    for (u32 e = 0; e < ns * 2; e++) for (u32 c = 0; c < 4; c++) {
        u32 bits = c < 3 ? cb : ab; if (!bits) continue;
        u32 v = ep[e][c];
        if (haspb) { v = (v << 1) | pbits[e]; bits++; }
        v <<= (8 - bits); v |= v >> bits;
        ep[e][c] = v & 255;
    }
    u32 sub[16];
    for (u32 i = 0; i < 16; i++) sub[i] = ns == 1 ? 0 : ns == 2 ? (bc7_p2[part] >> i) & 1 : (bc7_p3[part] >> (2 * i)) & 3;
    u32 idx[16], idx2[16];
    for (u32 i = 0; i < 16; i++) {
        int anchor = i == 0 || (ns == 2 && i == bc7_a2[part]) || (ns == 3 && (i == bc7_a3a[part] || i == bc7_a3b[part]));
        idx[i] = bc7_get(&s, IB[mode] - (anchor ? 1 : 0));
    }
    if (IB2[mode]) for (u32 i = 0; i < 16; i++) idx2[i] = bc7_get(&s, IB2[mode] - (i == 0 ? 1 : 0));
    for (u32 i = 0; i < 16; i++) {
        u32 k = sub[i]; u8 px[4];
        if (!IB2[mode]) {
            for (u32 c = 0; c < 4; c++) px[c] = bc7_interp(ep[k * 2][c], ep[k * 2 + 1][c], idx[i], IB[mode]);
        } else {
            u32 ci = isel ? idx2[i] : idx[i], cbits = isel ? IB2[mode] : IB[mode];
            u32 ai = isel ? idx[i] : idx2[i], abits = isel ? IB[mode] : IB2[mode];
            for (u32 c = 0; c < 3; c++) px[c] = bc7_interp(ep[0][c], ep[1][c], ci, cbits);
            px[3] = bc7_interp(ep[0][3], ep[1][3], ai, abits);
        }
        if (rot) { u8 t = px[3]; px[3] = px[rot - 1]; px[rot - 1] = t; }
        for (u32 c = 0; c < 4; c++) out[i * 4 + c] = px[c];
    }
}
