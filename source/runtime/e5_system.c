/* e5_system.c - system service, user service and controller (keyboard -> DualShock 4 on Windows).
 * Included by e3_hle.c. */

/* ---- system service ---- */
static u64 g_sys_events_polls;
static SYSV int ss_receive_event(void *ev) { (void)ev; g_sys_events_polls++; return (int)0x80A10003; }   /* NO_EVENT */
static SYSV int ss_get_status(u8 *st) {
    TRACE("sceSystemServiceGetStatus", st, 0, 0);
    if (st) h_memset(st, 0, 0x88);   /* eventNum 0, not overlaid, not in background */
    return 0;
}
static SYSV int ss_param_get_int(int id, int *v) {
    TRACE("sceSystemServiceParamGetInt", id, v, 0);
    if (!v) return (int)0x80A10001;
    switch (id) {
    case 1: *v = 1; break;      /* language: English (US) */
    case 2: *v = 0; break;      /* date format YYYYMMDD */
    case 3: *v = 0; break;      /* 12-hour clock */
    case 4: *v = 0; break;      /* time zone offset (minutes) */
    case 5: *v = 0; break;      /* summer time */
    case 7: *v = 9; break;      /* parental level */
    case 1000: *v = 1; break;   /* confirm button: Cross */
    default: *v = 0; break;
    }
    return 0;
}
static SYSV int ss_safe_area(float *info) { if (info) { h_memset(info, 0, 128); info[0] = 1.0f; } return 0; }

/* ---- user service ---- */
static SYSV int us_login_list(int *ids) { if (ids) { ids[0] = 1; ids[1] = ids[2] = ids[3] = -1; } return 0; }

/* ---- controller ---- */
#define PAD_L3 0x2
#define PAD_R3 0x4
#define PAD_OPTIONS 0x8
#define PAD_UP 0x10
#define PAD_RIGHT 0x20
#define PAD_DOWN 0x40
#define PAD_LEFT 0x80
#define PAD_L2 0x100
#define PAD_R2 0x200
#define PAD_L1 0x400
#define PAD_R1 0x800
#define PAD_TRIANGLE 0x1000
#define PAD_CIRCLE 0x2000
#define PAD_CROSS 0x4000
#define PAD_SQUARE 0x8000
#define PAD_TOUCH 0x100000
static u64 g_pad_reads, g_pad_presses;
/* ---- controllers and mouse aiming (STATUS 49; e5_input_win.h) ---- */
#ifdef _WIN32
#include "e5_input_win.h"
#else
#include "e5_pad_hid.h"
static int g_pad_no_controllers;
static void plat_pad_start(void) {}
static int plat_pad_poll(PlatPad *P) { h_memset(P, 0, sizeof *P); return 0; }
static void plat_pad_rumble(u8 large, u8 small) { (void)large; (void)small; }
static void plat_mouse_capture(int on) { (void)on; }
static int plat_mouse_delta(i32 *dx, i32 *dy) { *dx = *dy = 0; return 0; }
#endif
static float g_motion_scale = 1.0f, g_mouse_sens = 3.0f;   /* --motion-scale X, --mouse-sens X */
static int g_no_motion;                                    /* --no-motion: controllers report lying still (as before) */
static float g_pad_q[4] = {1, 0, 0, 0};                    /* orientation (w, x, y, z) integrated from the gyro */
static u64 g_pad_q_t;
static int g_mouse_aim; static float g_aim_vx, g_aim_vy;
static u64 g_pad_ctrl_reads; static float g_pad_max_gyro;
static void pad_orientation_reset(void) { g_pad_q[0] = 1; g_pad_q[1] = g_pad_q[2] = g_pad_q[3] = 0; }
static void pad_orientation_step(const float w[3], float dt) {   /* q += 0.5 * q * (0, w) * dt, normalised */
    float *q = g_pad_q, x = w[0] * dt * 0.5f, y = w[1] * dt * 0.5f, z = w[2] * dt * 0.5f;
    float qw = q[0] - q[1] * x - q[2] * y - q[3] * z, qx = q[1] + q[0] * x + q[2] * z - q[3] * y,
          qy = q[2] + q[0] * y - q[1] * z + q[3] * x, qz = q[3] + q[0] * z + q[1] * y - q[2] * x;
    float n = qw * qw + qx * qx + qy * qy + qz * qz;
    if (n > 1e-12f) { float sq; __asm__("sqrtss %1, %0" : "=x"(sq) : "x"(n)); float r = 1.0f / sq; q[0] = qw * r; q[1] = qx * r; q[2] = qy * r; q[3] = qz * r; }
}
/* --press FLIP:BUTTON - a scripted press, held for 8 flips, for unattended runs (x o up down left right options) */
static u64 g_press_at[64]; static u32 g_press_btn[64]; static int g_npress;
static u32 press_due(void) {
    u32 b = 0;
    for (int i = 0; i < g_npress; i++) if (g_flips >= g_press_at[i] && g_flips < g_press_at[i] + 8) b |= g_press_btn[i];
    return b;
}
static u32 g_pad_last;
static SYSV int pad_open(int user, int type, int index, void *param) { TRACE("scePadOpen", user, type, index); (void)param; return 1; }
static SYSV int pad_read_state(int h, u8 *d) {
    if (h != 1 || !d) return (int)0x80920003;
    g_pad_reads++;
    h_memset(d, 0, 120);
    u32 b = 0; int lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
    if (g_win.focused) {
        /* keyboard mapping (Windows virtual-key codes) */
        if (plat_key(0x0D) || plat_key(0x20)) b |= PAD_CROSS;          /* Enter / Space  -> Cross */
        if (plat_key(0x08) || plat_key(0x1B)) b |= PAD_CIRCLE;         /* Backspace / Esc -> Circle */
        if (plat_key('Q')) b |= PAD_SQUARE;
        if (plat_key('E')) b |= PAD_TRIANGLE;
        if (plat_key(0x09)) b |= PAD_OPTIONS;                          /* Tab -> Options */
        if (plat_key('T')) b |= PAD_TOUCH;
        if (plat_key(0x26)) b |= PAD_UP;                               /* arrow keys -> d-pad */
        if (plat_key(0x28)) b |= PAD_DOWN;
        if (plat_key(0x25)) b |= PAD_LEFT;
        if (plat_key(0x27)) b |= PAD_RIGHT;
        if (plat_key('1')) b |= PAD_L1;
        if (plat_key('3')) b |= PAD_R1;
        if (plat_key('Z')) { b |= PAD_L2; l2 = 255; }
        if (plat_key('C')) { b |= PAD_R2; r2 = 255; }
        if (plat_key('W')) ly = 0;                                      /* WASD -> left stick */
        if (plat_key('S')) ly = 255;
        if (plat_key('A')) lx = 0;
        if (plat_key('D')) lx = 255;
        if (plat_key('I')) ry = 0;                                      /* IJKL -> right stick */
        if (plat_key('K')) ry = 255;
        if (plat_key('J')) rx = 0;
        if (plat_key('L')) rx = 255;
    }
    /* mouse aiming (M toggles): the pointer is captured, its movement drives the right stick, left button = R2 (shoot),
       right button = L2 (aim) */
    {
        static int mwas; int m = g_win.focused && plat_key('M');
        if (m && !mwas) { g_mouse_aim = !g_mouse_aim; g_aim_vx = g_aim_vy = 0; say(g_mouse_aim ? "  mouse aiming on (M to turn off)\n" : "  mouse aiming off\n"); }
        mwas = m;
        if (!g_win.focused) plat_mouse_capture(0);
        else plat_mouse_capture(g_mouse_aim);
        if (g_mouse_aim && g_win.focused) {
            i32 dx, dy; plat_mouse_delta(&dx, &dy);
            g_aim_vx = g_aim_vx * 0.5f + (float)dx * g_mouse_sens; g_aim_vy = g_aim_vy * 0.5f + (float)dy * g_mouse_sens;
            float vx = g_aim_vx < -127 ? -127 : g_aim_vx > 127 ? 127 : g_aim_vx, vy = g_aim_vy < -127 ? -127 : g_aim_vy > 127 ? 127 : g_aim_vy;
            if (vx > 2 || vx < -2) rx = 128 + (int)vx;
            if (vy > 2 || vy < -2) ry = 128 + (int)vy;
            if (plat_key(0x01)) { b |= PAD_R2; r2 = 255; }
            if (plat_key(0x02)) { b |= PAD_L2; l2 = 255; }
        }
    }
    /* a game controller (DualShock 4 / DualSense over HID, or XInput) adds to the keyboard */
    PlatPad cp; int ctrl = g_win.focused && plat_pad_poll(&cp);
    if (ctrl) {
        static const char *last_kind; if (cp.kind != last_kind) { say("  controller: "); say(cp.kind ? cp.kind : "?"); say(cp.has_motion ? " (with motion sensors)\n" : "\n"); last_kind = cp.kind; }
        g_pad_ctrl_reads++;
        b |= cp.buttons;
        #define DEF(v) ((v) > 128 ? (v) - 128 : 128 - (v))
        if (DEF(cp.lx) > 14 && DEF(cp.lx) > DEF(lx)) lx = cp.lx; if (DEF(cp.ly) > 14 && DEF(cp.ly) > DEF(ly)) ly = cp.ly;
        if (DEF(cp.rx) > 14 && DEF(cp.rx) > DEF(rx)) rx = cp.rx; if (DEF(cp.ry) > 14 && DEF(cp.ry) > DEF(ry)) ry = cp.ry;
        #undef DEF
        if (cp.l2 > l2) l2 = cp.l2; if (cp.r2 > r2) r2 = cp.r2;
    }
    { u32 sb = press_due(); static u32 last_sb; if (sb & ~last_sb) { say("  scripted press at flip "); say_dec((i64)g_flips); say("\n"); } last_sb = sb; b |= sb; }
    if (b & ~g_pad_last) g_pad_presses++;
    g_pad_last = b;
    *(u32 *)d = b;
    d[4] = (u8)lx; d[5] = (u8)ly; d[6] = (u8)rx; d[7] = (u8)ry; d[8] = (u8)l2; d[9] = (u8)r2;
    /* ScePadData motion: orientation quaternion (x, y, z, w) at +12, acceleration (g) at +28, angular velocity (rad/s)
       at +40. Without sensors: a controller lying still. */
    if (ctrl && cp.has_motion && !g_no_motion) {
        u64 now = plat_ticks(); float dt = g_pad_q_t ? (float)(now - g_pad_q_t) / (float)plat_ticks_per_sec() : 0.0f; g_pad_q_t = now;
        if (dt > 0.1f) dt = 0.1f;
        float w[3]; for (int k = 0; k < 3; k++) w[k] = cp.gyro[k] * g_motion_scale;
        pad_orientation_step(w, dt);
        for (int k = 0; k < 3; k++) { float a = w[k] < 0 ? -w[k] : w[k]; if (a > g_pad_max_gyro) g_pad_max_gyro = a; }
        *(float *)(d + 12) = g_pad_q[1]; *(float *)(d + 16) = g_pad_q[2]; *(float *)(d + 20) = g_pad_q[3]; *(float *)(d + 24) = g_pad_q[0];
        for (int k = 0; k < 3; k++) { *(float *)(d + 28 + 4 * k) = cp.accel[k]; *(float *)(d + 40 + 4 * k) = w[k]; }
    } else {
        *(float *)(d + 24) = 1.0f;                       /* orientation quaternion w = 1 */
        *(float *)(d + 32) = 1.0f;                       /* acceleration: 1 g on y (resting) */
    }
    /* touch pad (ScePadTouchData at +52: count, then 2 x {u16 x, u16 y, u8 id} at +60 / +68; pad 1920 x 943):
       left mouse button held inside the window = one finger at the pointer (drag = swipe; click with T = press);
       F / G / R / V = a quick swipe left / right / up / down across the middle of the pad */
    if (g_win.focused) {
        static int sw_dir = -1; static u64 sw_t0; static u8 tid; static int was_down;
        u64 now = plat_ticks(), dur = plat_ticks_per_sec() / 4;   /* a swipe takes 250 ms */
        int sw_on = sw_dir >= 0 && now - sw_t0 < dur;
        u32 mx, my; int down = !g_mouse_aim && plat_key(0x01) && plat_mouse(&mx, &my);
        if (!sw_on) { int k = plat_key('F') ? 0 : plat_key('G') ? 1 : plat_key('R') ? 2 : plat_key('V') ? 3 : -1;
                      static int kwas; if (k >= 0 && !kwas) { sw_dir = k; sw_t0 = now; sw_on = 1; tid++; } kwas = k >= 0; }
        int tx = -1, ty = -1;
        if (sw_on) {   /* from one side to the other */
            u64 p = (now - sw_t0) * 1000 / dur; int c = (int)(p * 1700 / 1000) + 110, cy = (int)(p * 800 / 1000) + 70;
            if (sw_dir == 0) { tx = 1920 - c; ty = 471; } else if (sw_dir == 1) { tx = c; ty = 471; }
            else if (sw_dir == 2) { tx = 960; ty = 943 - cy; } else { tx = 960; ty = cy; }
        } else if (down) { if (!was_down) tid++; tx = (int)(mx * 1919 / 65535); ty = (int)(my * 942 / 65535); }
        was_down = down;
        if (tx >= 0) { d[52] = 1; *(u16 *)(d + 60) = (u16)tx; *(u16 *)(d + 62) = (u16)ty; d[64] = tid; }
    }
    if (ctrl && cp.ntouch && !d[52]) {   /* the controller's own touch pad (DualShock 4 / DualSense) */
        d[52] = (u8)cp.ntouch;
        for (int f = 0; f < cp.ntouch && f < 2; f++) { *(u16 *)(d + 60 + 8 * f) = cp.tx[f] > 1919 ? 1919 : cp.tx[f]; *(u16 *)(d + 62 + 8 * f) = cp.ty[f] > 942 ? 942 : cp.ty[f]; d[64 + 8 * f] = cp.tid[f]; }
    }
    d[76] = 1;                                        /* connected */
    *(u64 *)(d + 80) = plat_ticks() * 1000000ull / plat_ticks_per_sec();
    d[104] = 1;                                       /* connected count */
    return 0;
}
static SYSV int pad_info(int h, u8 *info) {
    if (h != 1 || !info) return (int)0x80920003;
    h_memset(info, 0, 28);
    *(float *)info = 44.86f; *(u16 *)(info + 4) = 1920; *(u16 *)(info + 6) = 943;
    info[8] = 13; info[9] = 13; info[10] = 0; info[11] = 1; info[12] = 1;
    return 0;
}
static SYSV int pad_ok(void) { return 0; }
/* scePadSetVibration(handle, {u8 large motor, u8 small motor}) -> the XInput controller's motors */
static SYSV int pad_vibration(int h, const u8 *p) { (void)h; if (p) plat_pad_rumble(p[0], p[1]); return 0; }
static SYSV int pad_reset_orientation(int h) {
    (void)h; pad_orientation_reset();
    static int nl; if (nl++ < 50 && g_pad_ctrl_reads) { say("  pad: orientation reset by the game (largest angular velocity since the last reset: "); say_dec((i64)(g_pad_max_gyro * 1000.0f)); say(" mrad/s)\n"); }
    g_pad_max_gyro = 0;
    return 0;
}

/* ---- M3 batch 1: services the game called that used to return 0 without writing their outputs ---- */
/* libSceNet byte order (the PS4 is little-endian, network order is big-endian) */
static SYSV u16 net_htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static SYSV u32 net_htonl(u32 v) { return __builtin_bswap32(v); }
/* libSceNetCtl: the console is offline. State 0 = DISCONNECTED. */
static SYSV int nc_ok(void) { return 0; }
static SYSV int nc_get_state(int *st) { TRACE("sceNetCtlGetState", st, 0, 0); if (st) *st = 0; return 0; }
static SYSV int nc_register_cb(void *fn, void *arg, int *cid) { TRACE("sceNetCtlRegisterCallback", fn, arg, cid); if (cid) *cid = 0; return 0; }
/* SceNetCtlNatInfo {u32 size; int stunStatus; int natType; u32 mappedAddr}: size is the caller's; the rest
   zero = STUN not checked, no NAT type, no address */
static SYSV int nc_nat_info(u32 *info) { TRACE("sceNetCtlGetNatInfo", info, 0, 0); if (info) { info[1] = 0; info[2] = 0; info[3] = 0; } return 0; }
/* libSceRtc: SceRtcDateTime {u16 year, month, day, hour, minute, second; u32 microsecond}. E6 models the
   time zone as UTC (sceSystemServiceParamGetInt time zone 0, libc localtime == gmtime), so local time is UTC. */
static void rtc_from_ns(u64 ns, u16 *dt) {
    i64 secs = (i64)(ns / 1000000000ull), days = secs / 86400, rem = secs % 86400;
    i64 z = days + 719468, era = z / 146097, doe = z - era * 146097;            /* civil-from-days (proleptic Gregorian) */
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    dt[0] = (u16)(y + (m <= 2)); dt[1] = (u16)m; dt[2] = (u16)d;
    dt[3] = (u16)(rem / 3600); dt[4] = (u16)(rem / 60 % 60); dt[5] = (u16)(rem % 60);
    *(u32 *)(dt + 6) = (u32)(ns / 1000ull % 1000000ull);
}
static SYSV int rtc_local_time(u16 *dt) {
    TRACE("sceRtcGetCurrentClockLocalTime", dt, 0, 0);
    if (!dt) return (int)0x80020016;   /* null pointer; the library's own error code is not modelled */
    rtc_from_ns(plat_wall_ns(), dt); return 0;
}
/* SceRtcTick: microseconds since 0001-01-01 00:00:00 (UTC here, see above). Not reached in Chapter 1; the game's date
   helpers at eboot+0x97b04b..0x97b36c use them. */
#define RTC_UNIX_EPOCH_TICK 62135596800000000ull
static i64 rtc_days_from_civil(i64 y, i64 m, i64 d) {   /* days since 1970-01-01 */
    y -= m <= 2; i64 era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int rtc_valid(const u16 *dt) {
    static const u8 md[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return dt && dt[0] >= 1 && dt[0] <= 9999 && dt[1] >= 1 && dt[1] <= 12 && dt[2] >= 1 && dt[2] <= md[dt[1]] && dt[3] < 24 && dt[4] < 60 && dt[5] < 60 && *(const u32 *)(dt + 6) < 1000000;
}
static u64 rtc_tick_of(const u16 *dt) {
    i64 days = rtc_days_from_civil(dt[0], dt[1], dt[2]);
    return (u64)((days * 86400 + dt[3] * 3600 + dt[4] * 60 + dt[5]) * 1000000ll + *(const u32 *)(dt + 6)) + RTC_UNIX_EPOCH_TICK;
}
static SYSV int rtc_current_tick(u64 *t) {
    TRACE("sceRtcGetCurrentTick", t, 0, 0);
    if (!t) return (int)0x80B50002;
    *t = plat_wall_ns() / 1000ull + RTC_UNIX_EPOCH_TICK; return 0;
}
static SYSV int rtc_set_tick(u16 *dt, const u64 *t) {
    TRACE("sceRtcSetTick", dt, t, 0);
    if (!dt || !t) return (int)0x80B50002;
    if (*t < RTC_UNIX_EPOCH_TICK) { h_memset(dt, 0, 16); dt[0] = 1970; dt[1] = 1; dt[2] = 1; return 0; }   /* before 1970: not modelled */
    rtc_from_ns((*t - RTC_UNIX_EPOCH_TICK) * 1000ull, dt); return 0;
}
static SYSV int rtc_day_of_week(int y, int m, int d) {   /* 0 = Sunday */
    TRACE("sceRtcGetDayOfWeek", y, m, d);
    if (y < 1 || m < 1 || m > 12 || d < 1 || d > 31) return (int)0x80B50008;
    i64 days = rtc_days_from_civil(y, m, d);   /* 1970-01-01 was a Thursday */
    return (int)(((days % 7) + 7 + 4) % 7);
}
static SYSV int rtc_win32_filetime(const u16 *dt, u64 *ft) {   /* 100 ns intervals since 1601-01-01 */
    TRACE("sceRtcGetWin32FileTime", dt, ft, 0);
    if (!dt || !ft) return (int)0x80B50002;
    if (!rtc_valid(dt)) return (int)0x80B50003;
    u64 tick = rtc_tick_of(dt), t1601 = 50491123200000000ull;   /* 1601-01-01 in ticks */
    *ft = tick >= t1601 ? (tick - t1601) * 10ull : 0; return 0;
}
static SYSV int rtc_format_rfc2822_local(char *out, const u64 *t) {   /* "Wed, 21 Oct 2026 03:24:00 +0000" */
    TRACE("sceRtcFormatRFC2822LocalTime", out, t, 0);
    if (!out) return (int)0x80B50002;
    u64 tick = t ? *t : plat_wall_ns() / 1000ull + RTC_UNIX_EPOCH_TICK;
    u16 dt[8]; if (tick < RTC_UNIX_EPOCH_TICK) tick = RTC_UNIX_EPOCH_TICK; rtc_from_ns((tick - RTC_UNIX_EPOCH_TICK) * 1000ull, dt);
    static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    i64 days = rtc_days_from_civil(dt[0], dt[1], dt[2]); int w = (int)(((days % 7) + 7 + 4) % 7);
    char *o = out; const char *q;
#define RC(c) (*o++ = (char)(c))
#define R2(v) do { RC('0' + (v) / 10 % 10); RC('0' + (v) % 10); } while (0)
    for (q = wd[w]; *q; q++) RC(*q);
    RC(','); RC(' '); R2(dt[2]); RC(' ');
    for (q = mn[dt[1] - 1]; *q; q++) RC(*q);
    RC(' '); RC('0' + dt[0] / 1000 % 10); RC('0' + dt[0] / 100 % 10); R2(dt[0]); RC(' ');
    R2(dt[3]); RC(':'); R2(dt[4]); RC(':'); R2(dt[5]); RC(' '); RC('+'); RC('0'); RC('0'); RC('0'); RC('0'); RC(0);
#undef R2
#undef RC
    return 0;
}
/* libSceAppContent. Boot parameter {char reserved[4]; u32 attr; char reserved2[32]}: attr 0 = no special boot. */
static SYSV int ac_initialize(const void *init, u8 *boot) { TRACE("sceAppContentInitialize", init, boot, 0); if (boot) h_memset(boot, 0, 40); return 0; }
/* paramId 1..4 = USER_DEFINED_PARAM_1..4 in sce_sys/param.sfo (0 when the key is absent, as it is for CUSA02636 01.03) */
static int sfo_get_int(const char *key, u32 *out) {
    char path[1100]; path_join(path, g_game_dir, "sce_sys/param.sfo");
    void *f = hf_open_rb(path); if (!f) return 0;
    static u8 buf[65536]; u64 n = hf_read(f, buf, sizeof buf); hf_close(f);
    if (n < 20 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F') return 0;
    u32 ks = *(u32 *)(buf + 8), ds = *(u32 *)(buf + 12), cnt = *(u32 *)(buf + 16);
    for (u32 i = 0; i < cnt && 20 + 16 * (u64)i + 16 <= n; i++) {
        u8 *e = buf + 20 + 16 * i;
        u32 ko = *(u16 *)e, fmt = *(u16 *)(e + 2), doff = *(u32 *)(e + 12);
        if (ks + ko >= n || fmt != 0x404 || ds + doff + 4 > n) continue;
        if (str_eq((const char *)buf + ks + ko, key)) { *out = *(u32 *)(buf + ds + doff); return 1; }
    }
    return 0;
}
static SYSV int ac_param_get_int(int id, int *v) {
    TRACE("sceAppContentAppParamGetInt", id, v, 0);
    if (!v) return (int)0x80020016;   /* null pointer; the library's own error code is not modelled */
    static const char *keys[4] = {"USER_DEFINED_PARAM_1", "USER_DEFINED_PARAM_2", "USER_DEFINED_PARAM_3", "USER_DEFINED_PARAM_4"};
    u32 x = 0;
    if (id >= 1 && id <= 4) sfo_get_int(keys[id - 1], &x);
    *v = (int)x; return 0;
}
/* libSceGnmDriver */
static SYSV int gnm_user_pa(void) { return 0; }     /* user PA debug interface: disabled, as on a retail console */
static SYSV int gnm_ok(void) { return 0; }
/* The tessellation-factor ring's GPU address. The game maps its own direct memory there (seen in the log:
   a fixed mapping at exactly the returned address), so this only returns the address and maps nothing.
   0xFF0000000 is at the top of the 8-64 GB range, away from the game's other mappings. */
static SYSV void *gnm_tf_ring(void) {
    TRACE("sceGnmGetTheTessellationFactorRingBufferBaseAddress", 0, 0, 0);
    return (void *)0xFF0000000ull;
}

/* ---- M3 batch 2 (each behaviour checked against the game's own calling code in eboot 01.03) ---- */
/* libSceMsgDialog. Status: 0 none, 1 initialized, 2 running, 3 finished. The game opens one OK-only user
   message (eboot+0x1560680: mode 1, buttonType 0) and polls until status 3, then terminates it. E6 used
   to answer 0, so the game believed a dialog stayed on screen. Nothing is drawn: an OK-only dialog is
   finished at once, as if OK were pressed; any other kind stays running and is reported. */
static int g_msg_status, g_msg_mode, g_msg_opens;
static void say_text(const char *t, int max) {
    char b[160]; int n = 0;
    for (int i = 0; t[i] && i < max && n < (int)sizeof b - 1; i++) b[n++] = (t[i] >= 32 && t[i] < 127) ? t[i] : '.';
    b[n] = 0; say(b);
}
static SYSV int md_init(void) { TRACE("sceMsgDialogInitialize", 0, 0, 0); g_msg_status = 1; return 0; }
static SYSV int md_open(const u8 *prm) {
    TRACE("sceMsgDialogOpen", prm, 0, 0);
    if (!prm) return (int)0x80020016;
    g_msg_mode = *(const int *)(prm + 0x38); g_msg_opens++;
    int auto_ok = 0;
    say("  message dialog opened: mode "); say_dec(g_msg_mode);
    if (g_msg_mode == 1) {
        const u8 *um = *(const u8 *const *)(prm + 0x40);
        int bt = um ? *(const int *)um : -1; const char *msg = um ? *(const char *const *)(um + 8) : 0;
        say(", button type "); say_dec(bt);
        if (msg) { say(", text \""); say_text(msg, 150); say("\""); }
        auto_ok = bt == 0;
    }
    say(auto_ok ? " -> finished as if OK were pressed\n" : " -> left running (not answered automatically)\n");
    g_msg_status = auto_ok ? 3 : 2;
    return 0;
}
static SYSV int md_status(void) { return g_msg_status; }
static SYSV int md_result(int *r) {     /* SceMsgDialogResult {mode, result, buttonId, reserved[32]}: OK, button 1 */
    TRACE("sceMsgDialogGetResult", r, 0, 0);
    if (!r) return (int)0x80020016;
    h_memset(r, 0, 44); r[0] = g_msg_mode; r[1] = 0; r[2] = 1; return 0;
}
static SYSV int md_close(void) { TRACE("sceMsgDialogClose", 0, 0, 0); if (g_msg_status == 2) g_msg_status = 3; return 0; }
static SYSV int md_term(void) { TRACE("sceMsgDialogTerminate", 0, 0, 0); g_msg_status = 0; return 0; }
static SYSV int md_ok(void) { return 0; }
/* libSceCompanionUtil: no companion app ever connects. The game (eboot+0x14b2bd8) treats any non-zero
   result as "no event"; a 0 made it read an event type from uninitialised stack. The library's own
   no-event code is not modelled; the kernel's EAGAIN stands in for it. */
static SYSV int cu_get_event(void *ev) { (void)ev; return (int)0x80020023; }
/* libSceAppContent entitlement (STATUS 87). The game asks once per start, for the pre-order bonus chapter "The Road
   Not Taken" (label UDAWNDLC00000001): eboot+0xad04e0 copies the label, calls this and treats 0 as "owned"; the
   16-byte key is never read. The add-on is an unlock key only (its package holds no data, PUBTOOLINFO
   img0_l0_size=2): the chapter and clues are in the base game. It counts as installed when the player's own copy of
   it is in the game folder as <label>\sce_sys\param.sfo, an add-on (CATEGORY ac) of this title whose CONTENT_ID
   ends in the label. --no-dlc answers "not owned" regardless. */
static int g_opt_no_dlc;
static int sfo_str(const u8 *d, u64 n, const char *key, char *out, int outn) {   /* param.sfo string value (0 if absent) */
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
static int ac_addon_present(const char *label) {
    int L = 0; while (L < 17 && label[L]) { char c = label[L]; if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0; L++; }
    if (L == 0 || L > 16) return 0;
    char dir[24], path[1100]; mem_copy(dir, label, (u64)L); dir[L] = 0;
    path_join(path, g_game_dir, dir); path_join(path, path, "sce_sys/param.sfo");
    void *f = hf_open_rb(path); if (!f) return 0;
    static u8 buf[65536]; u64 n = hf_read(f, buf, sizeof buf); hf_close(f);
    char cat[8], cid[64]; int cl;
    if (!sfo_str(buf, n, "CATEGORY", cat, sizeof cat) || !str_eq(cat, "ac")) return 0;
    if (!sfo_str(buf, n, "CONTENT_ID", cid, sizeof cid)) return 0;
    cl = str_len(cid);
    if (cl < L + 1 || cid[cl - L - 1] != '-' || !str_eq(cid + cl - L, dir)) return 0;
    for (int i = 0; i + 9 <= cl; i++) if (cid[i] == 'C' && cid[i + 1] == 'U' && cid[i + 2] == 'S' && cid[i + 3] == 'A' && cid[i + 4] == '0' && cid[i + 5] == '2' && cid[i + 6] == '6' && cid[i + 7] == '3' && cid[i + 8] == '6') return 1;
    return 0;
}
static SYSV int ac_ent_key(int label_kind, const char *label, void *key) {
    TRACE("sceAppContentGetEntitlementKey", label_kind, label, key);
    char lb[17]; int k = 0; if (label) while (k < 16 && label[k]) { lb[k] = label[k]; k++; } lb[k] = 0;
    int owned = !g_opt_no_dlc && label && ac_addon_present(lb);
    say("  add-on entitlement asked: \""); say(lb); say(owned ? "\" -> owned (found in the game folder)\n" : g_opt_no_dlc ? "\" -> not owned (--no-dlc)\n" : "\" -> not owned (not in the game folder)\n");
    if (!owned) return (int)0x80020002;   /* kernel ENOENT stands in for the library's own code */
    if (key) h_memset(key, 0, 16);
    return 0;
}
/* libScePlayGo: a fully installed package; every chunk local (locus 3 = local, fast). The game also handles
   0x80B20008 / 0x80B2000E itself (eboot+0xacf780). */
static SYSV int pg_open(int *h, const void *prm) { TRACE("scePlayGoOpen", h, prm, 0); if (!h) return (int)0x80B2000A; *h = 1; return 0; }
static SYSV int pg_locus(int h, const u16 *ids, u32 n, i8 *loci) {
    if (h != 1) return (int)0x80B20009;
    if (!ids || !loci) return (int)0x80B2000A;
    for (u32 i = 0; i < n; i++) loci[i] = 3;
    return 0;
}
static SYSV int pg_todo(int h, void *list, u32 n, u32 *entries) { (void)list; (void)n; if (h != 1) return (int)0x80B20009; if (entries) *entries = 0; return 0; }
static SYSV int pg_handle_ok(int h) { return h == 1 ? 0 : (int)0x80B20009; }
/* Prefetch: everything is already local, so there is nothing to fetch. The game ignores the result
   (eboot+0x128e659, after New Story). GetProgress: SceProgress {u64 progressSize, totalSize}; equal sizes
   mean complete (the real chunk sizes are not modelled, so 1 per requested chunk). */
static SYSV int pg_prefetch(int h, const u16 *ids, u32 n, int min_locus) { TRACE("scePlayGoPrefetch", h, n, min_locus); (void)ids; return h == 1 ? 0 : (int)0x80B20009; }
static SYSV int pg_progress(int h, const u16 *ids, u32 n, u64 *pr) {
    TRACE("scePlayGoGetProgress", h, n, pr); (void)ids;
    if (h != 1) return (int)0x80B20009;
    if (!pr) return (int)0x80B2000A;
    pr[0] = pr[1] = n; return 0;
}
/* libSceAjm: context and codec registration only. No instance is created in the menu; decoding is M4. */
static SYSV int ajm_init(i64 reserved, u32 *ctx) { TRACE("sceAjmInitialize", reserved, ctx, 0); if (!ctx) return (int)0x80020016; *ctx = 1; return 0; }
static SYSV int ajm_ok(void) { return 0; }

static const HleEntry g_hle_sys[] = {
    {"sceSystemServiceReceiveEvent", (void *)ss_receive_event}, {"sceSystemServiceGetStatus", (void *)ss_get_status},
    {"sceSystemServiceParamGetInt", (void *)ss_param_get_int}, {"sceSystemServiceGetDisplaySafeAreaInfo", (void *)ss_safe_area},
    {"sceUserServiceGetLoginUserIdList", (void *)us_login_list},
    {"scePadOpen", (void *)pad_open}, {"scePadReadState", (void *)pad_read_state}, {"scePadGetControllerInformation", (void *)pad_info},
    {"scePadSetLightBar", (void *)pad_ok}, {"scePadSetVibration", (void *)pad_vibration}, {"scePadResetOrientation", (void *)pad_reset_orientation},
    {"scePadSetTiltCorrectionState", (void *)pad_ok}, {"scePadClose", (void *)pad_ok},
    {"sceNetHtons", (void *)net_htons}, {"sceNetHtonl", (void *)net_htonl},
    {"sceNetCtlInit", (void *)nc_ok}, {"sceNetCtlCheckCallback", (void *)nc_ok}, {"sceNetCtlGetState", (void *)nc_get_state},
    {"sceNetCtlRegisterCallback", (void *)nc_register_cb}, {"sceNetCtlGetNatInfo", (void *)nc_nat_info},
    {"sceRtcGetCurrentClockLocalTime", (void *)rtc_local_time},
    {"sceRtcGetCurrentTick", (void *)rtc_current_tick}, {"sceRtcSetTick", (void *)rtc_set_tick}, {"sceRtcGetDayOfWeek", (void *)rtc_day_of_week},
    {"sceRtcGetWin32FileTime", (void *)rtc_win32_filetime}, {"sceRtcFormatRFC2822LocalTime", (void *)rtc_format_rfc2822_local},
    {"sceAppContentInitialize", (void *)ac_initialize}, {"sceAppContentAppParamGetInt", (void *)ac_param_get_int},
    {"sceGnmIsUserPaEnabled", (void *)gnm_user_pa}, {"sceGnmSetupMipStatsReport", (void *)gnm_ok},
    {"sceGnmGetTheTessellationFactorRingBufferBaseAddress", (void *)gnm_tf_ring},
    {"sceMsgDialogInitialize", (void *)md_init}, {"sceMsgDialogOpen", (void *)md_open}, {"sceMsgDialogUpdateStatus", (void *)md_status},
    {"sceMsgDialogGetResult", (void *)md_result}, {"sceMsgDialogClose", (void *)md_close}, {"sceMsgDialogTerminate", (void *)md_term},
    {"sceMsgDialogProgressBarSetValue", (void *)md_ok},
    {"sceCompanionUtilGetEvent", (void *)cu_get_event}, {"sceAppContentGetEntitlementKey", (void *)ac_ent_key},
    {"scePlayGoOpen", (void *)pg_open}, {"scePlayGoGetLocus", (void *)pg_locus}, {"scePlayGoGetToDoList", (void *)pg_todo},
    {"scePlayGoSetInstallSpeed", (void *)pg_handle_ok}, {"scePlayGoPrefetch", (void *)pg_prefetch}, {"scePlayGoGetProgress", (void *)pg_progress}, {"scePlayGoTerminate", (void *)ajm_ok},
    {"sceAjmInitialize", (void *)ajm_init}, {"sceAjmModuleRegister", (void *)ajm_ok}, {"sceAjmModuleUnregister", (void *)ajm_ok},
    {"sceAjmFinalize", (void *)ajm_ok},
    {0, 0}};
