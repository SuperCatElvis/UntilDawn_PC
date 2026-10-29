/* pad_hid_test.c - checks e5_pad_hid.h against report layouts (byte offsets as documented by Linux's hid-playstation.c; absolute byte
   positions in the report, report id at 0). Build: gcc -O1 -Wall -o /tmp/pad_hid_test pad_hid_test.c && /tmp/pad_hid_test */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32;
#include "e5_pad_hid.h"
static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void put16(u8 *p, int v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void touch(u8 *p, int active, int id, int x, int y) { p[0] = (u8)((active ? 0 : 0x80) | id); p[1] = (u8)x; p[2] = (u8)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4)); p[3] = (u8)(y >> 4); }
static int feq(float a, float b) { float d = a - b; return d < 1e-4f && d > -1e-4f; }
int main(void) {
    u8 r[100]; PlatPad P;
    /* DualShock 4 over USB: report 0x01, 64 bytes. common block at [1]: sticks [1..4], buttons [5..7], L2 [8], R2 [9],
       timestamp [10..11], temperature [12], gyro [13..18], accel [19..24]; touch: count [33], report 0 timestamp [34],
       points [35..38], [39..42] */
    for (int bt = 0; bt < 2; bt++) {
        memset(r, 0, sizeof r); int o = bt ? 2 : 0;   /* Bluetooth 0x11: two more bytes before the common block */
        r[0] = bt ? 0x11 : 0x01;
        r[1 + o] = 10; r[2 + o] = 20; r[3 + o] = 30; r[4 + o] = 250;
        r[5 + o] = 0x20 | 2;          /* cross + d-pad right */
        r[6 + o] = 0x01 | 0x20 | 0x80; /* L1, options, R3 */
        r[7 + o] = 0x02;              /* touch pad click */
        r[8 + o] = 77; r[9 + o] = 200;
        put16(r + 13 + o, 164); put16(r + 15 + o, -328); put16(r + 17 + o, 0);   /* gyro: 10, -20, 0 deg/s */
        put16(r + 19 + o, 0); put16(r + 21 + o, 8192); put16(r + 23 + o, -4096);  /* accel: 0, 1, -0.5 g */
        r[33 + o] = 1; touch(r + 35 + o, 1, 5, 1234, 567); touch(r + 39 + o, 0, 6, 0, 0);
        memset(&P, 0, sizeof P);
        CK(hid_parse(0, r, bt ? 78 : 64, &P) == 1, "DS4 %s: parse", bt ? "BT" : "USB");
        CK(P.lx == 10 && P.ly == 20 && P.rx == 30 && P.ry == 250, "DS4 sticks %u %u %u %u", P.lx, P.ly, P.rx, P.ry);
        CK(P.buttons == (0x4000 | 0x20 | 0x400 | 0x8 | 0x4 | 0x100000), "DS4 buttons %x", P.buttons);
        CK(P.l2 == 77 && P.r2 == 200, "DS4 triggers %u %u", P.l2, P.r2);
        CK(P.has_motion && feq(P.gyro[0], 164 / 16.4f * 0.0174532925f), "DS4 gyro x %f", P.gyro[0]);
        CK(feq(P.gyro[1], -328 / 16.4f * 0.0174532925f) && feq(P.gyro[2], 0), "DS4 gyro %f %f", P.gyro[1], P.gyro[2]);
        CK(feq(P.accel[0], 0) && feq(P.accel[1], 1) && feq(P.accel[2], -0.5f), "DS4 accel %f %f %f", P.accel[0], P.accel[1], P.accel[2]);
        CK(P.ntouch == 1 && P.tx[0] == 1234 && P.ty[0] == 567 && P.tid[0] == 5, "DS4 touch %d %u %u %u", P.ntouch, P.tx[0], P.ty[0], P.tid[0]);
    }
    /* DualShock 4 over Bluetooth before the full reports: 0x01, 10 bytes, no sensors */
    memset(r, 0, sizeof r); r[0] = 0x01; r[1] = 128; r[2] = 128; r[3] = 128; r[4] = 128; r[5] = 0x80 | 8; r[8] = 0; r[9] = 0;
    memset(&P, 0, sizeof P);
    CK(hid_parse(0, r, 10, &P) == 1 && P.buttons == 0x1000 && !P.has_motion, "DS4 basic: buttons %x motion %d", P.buttons, P.has_motion);
    /* DualSense over USB: 0x01, 64 bytes: sticks [1..4], L2 [5], R2 [6], seq [7], buttons [8..11], gyro [16..21],
       accel [22..27], touch points [33..36], [37..40]; Bluetooth 0x31: one more byte before the block */
    for (int bt = 0; bt < 2; bt++) {
        memset(r, 0, sizeof r); int o = bt ? 1 : 0;
        r[0] = bt ? 0x31 : 0x01;
        r[1 + o] = 1; r[2 + o] = 2; r[3 + o] = 3; r[4 + o] = 4; r[5 + o] = 99; r[6 + o] = 111;
        r[8 + o] = 0x10 | 0x40 | 6;   /* square, circle, d-pad left */
        r[9 + o] = 0x02 | 0x08 | 0x40; /* R1, R2, L3 */
        r[10 + o] = 0x02;
        put16(r + 16 + o, -164); put16(r + 18 + o, 0); put16(r + 20 + o, 82);
        put16(r + 22 + o, 8192); put16(r + 24 + o, 0); put16(r + 26 + o, 0);
        touch(r + 33 + o, 0, 1, 0, 0); touch(r + 37 + o, 1, 9, 1900, 1080);
        memset(&P, 0, sizeof P);
        CK(hid_parse(1, r, bt ? 78 : 64, &P) == 1, "DualSense %s: parse", bt ? "BT" : "USB");
        CK(P.lx == 1 && P.ly == 2 && P.rx == 3 && P.ry == 4 && P.l2 == 99 && P.r2 == 111, "DualSense sticks / triggers");
        CK(P.buttons == (0x8000 | 0x2000 | 0x80 | 0x800 | 0x200 | 0x2 | 0x100000), "DualSense buttons %x", P.buttons);
        CK(feq(P.gyro[0], -164 / 16.4f * 0.0174532925f) && feq(P.gyro[2], 82 / 16.4f * 0.0174532925f), "DualSense gyro %f %f", P.gyro[0], P.gyro[2]);
        CK(feq(P.accel[0], 1) && feq(P.accel[1], 0), "DualSense accel %f %f", P.accel[0], P.accel[1]);
        CK(P.ntouch == 1 && P.tx[0] == 1900 && P.ty[0] == 943 && P.tid[0] == 9, "DualSense touch %d %u %u %u (y scaled 1080 -> 943)", P.ntouch, P.tx[0], P.ty[0], P.tid[0]);
    }
    /* d-pad hat values */
    static const u32 hat[9] = {0x10, 0x30, 0x20, 0x60, 0x40, 0xC0, 0x80, 0x90, 0};
    for (u32 h = 0; h < 9; h++) { memset(r, 0, sizeof r); r[0] = 1; r[5] = (u8)h; memset(&P, 0, sizeof P); hid_parse(0, r, 64, &P); CK(P.buttons == hat[h], "hat %u -> %x", h, P.buttons); }
    /* other reports are ignored */
    memset(r, 0, sizeof r); r[0] = 0x05; memset(&P, 0, sizeof P); CK(hid_parse(0, r, 64, &P) == 0 && !P.connected, "report 0x05 ignored");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
