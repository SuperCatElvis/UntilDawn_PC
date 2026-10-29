/* e5_pad_hid.h - input reports of Sony controllers (DualShock 4, DualSense) -> PS4 pad state. Portable (no OS calls),
   included by e5_input_win.h and by pad_hid_test.c. Report layouts: the byte offsets documented by Linux's hid-playstation.c; the parser is our own. */
typedef struct {
    int connected;                 /* 1 = a controller's data is in here */
    u32 buttons;                   /* PS4 pad bits (PAD_* in e5_system.c) */
    u8 lx, ly, rx, ry, l2, r2;     /* 0..255, 128 = centre */
    int has_motion; float accel[3], gyro[3];   /* g, rad/s (raw sensor axes) */
    int ntouch; u16 tx[2], ty[2]; u8 tid[2];   /* touch pad, 1920 x 943 */
    const char *kind;
} PlatPad;

/* button byte -> PS4 pad bits: shapes (bits 4..7 of the d-pad byte), shoulders / triggers / options / sticks */
static const u32 k_pad_shapes[8] = {0, 0, 0, 0, 0x8000, 0x4000, 0x2000, 0x1000};      /* square, cross, circle, triangle */
static const u32 k_pad_misc[8] = {0x400, 0x800, 0x100, 0x200, 0, 0x8, 0x2, 0x4};       /* L1 R1 L2 R2 share/create options L3 R3 */
static u32 pad_bits(u32 v, const u32 *map) { u32 b = 0; for (int i = 0; i < 8; i++) if (v >> i & 1) b |= map[i]; return b; }
static int hid_dpad(u32 d, u32 *b) {   /* hat: 0 = up, clockwise, 8 = none */
    static const u8 m[9] = {0x10, 0x30, 0x20, 0x60, 0x40, 0xC0, 0x80, 0x90, 0};
    *b |= d < 9 ? m[d] : 0; return 0;
}
static short rd16(const u8 *p) { return (short)(p[0] | (p[1] << 8)); }
/* parse one input report into P; returns 1 when it was a full report */
static int hid_parse(int sense, const u8 *r, u32 n, PlatPad *P) {
    u32 b = 0; const u8 *s;
    if (!sense) {   /* DualShock 4: USB report 0x01 (64 bytes), Bluetooth 0x11 (78), Bluetooth basic 0x01 (10) */
        if (r[0] == 0x11 && n >= 78) s = r + 3; else if (r[0] == 0x01 && n >= 64) s = r + 1;
        else if (r[0] == 0x01 && n >= 10) { s = r + 1; P->has_motion = 0; P->ntouch = 0; goto basic; }
        else return 0;
        P->has_motion = 1;
        for (int k = 0; k < 3; k++) { P->gyro[k] = (float)rd16(s + 12 + 2 * k) * (1.0f / 16.4f) * 0.0174532925f; P->accel[k] = (float)rd16(s + 18 + 2 * k) * (1.0f / 8192.0f); }
        P->ntouch = 0;
        for (int f = 0; f < 2; f++) {
            const u8 *t = s + 34 + 4 * f;
            if (!(t[0] & 0x80)) { P->tid[P->ntouch] = t[0] & 0x7F; P->tx[P->ntouch] = (u16)(t[1] | ((t[2] & 0x0F) << 8)); P->ty[P->ntouch] = (u16)((t[2] >> 4) | (t[3] << 4)); P->ntouch++; }
        }
    basic:
        P->lx = s[0]; P->ly = s[1]; P->rx = s[2]; P->ry = s[3];
        hid_dpad(s[4] & 0x0F, &b);
        b |= pad_bits(s[4], k_pad_shapes) | pad_bits(s[5], k_pad_misc);
        if (s[6] & 0x02) b |= 0x100000;
        P->l2 = s[7]; P->r2 = s[8];
    } else {        /* DualSense: USB report 0x01 (64 bytes), Bluetooth 0x31 (78) */
        if (r[0] == 0x31 && n >= 78) s = r + 2; else if (r[0] == 0x01 && n >= 64) s = r + 1; else return 0;
        P->lx = s[0]; P->ly = s[1]; P->rx = s[2]; P->ry = s[3]; P->l2 = s[4]; P->r2 = s[5];
        hid_dpad(s[7] & 0x0F, &b);
        b |= pad_bits(s[7], k_pad_shapes) | pad_bits(s[8], k_pad_misc);
        if (s[9] & 0x02) b |= 0x100000;
        P->has_motion = 1;
        for (int k = 0; k < 3; k++) { P->gyro[k] = (float)rd16(s + 15 + 2 * k) * (1.0f / 16.4f) * 0.0174532925f; P->accel[k] = (float)rd16(s + 21 + 2 * k) * (1.0f / 8192.0f); }
        P->ntouch = 0;
        for (int f = 0; f < 2; f++) {
            const u8 *t = s + 32 + 4 * f;
            if (!(t[0] & 0x80)) { u32 y = (u32)((t[2] >> 4) | (t[3] << 4)); P->tid[P->ntouch] = t[0] & 0x7F; P->tx[P->ntouch] = (u16)(t[1] | ((t[2] & 0x0F) << 8)); P->ty[P->ntouch] = (u16)(y * 943 / 1080); P->ntouch++; }
        }
    }
    P->buttons = b; P->connected = 1;
    return 1;
}
