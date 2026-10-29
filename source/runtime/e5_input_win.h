/* e5_input_win.h - game controllers and mouse aiming for the PS4 pad emulation (Windows part of e3_platform.h).
 *
 * Controllers (STATUS 49):
 *   - Sony DualShock 4 (v1, v2, wireless adapter) and DualSense / DualSense Edge, over USB or Bluetooth, read as HID
 *     devices by a background thread: buttons, sticks, triggers, touch pad, and the motion sensors (the "Don't move"
 *     prompts read them);
 *   - Xbox-style controllers through XInput (also DS4Windows / Steam virtual pads): buttons, sticks, triggers,
 *     vibration; no motion sensors (the pad then reports a controller lying still, as before).
 * Nothing is required: without a controller the keyboard mapping works as before. Every system library is loaded at
 * run time (hid.dll, setupapi.dll, xinput*.dll); a missing one only disables that kind of controller.
 *
 * Mouse aiming: plat_mouse_delta() gives the pointer movement since the last call while the pointer is captured
 * (plat_mouse_capture(1): hidden, kept in the window centre, confined to the window). */

#include "e5_pad_hid.h"

typedef struct { u32 data1; u16 data2, data3; u8 data4[8]; } PlatGuid;
typedef struct { u32 size; PlatGuid cls; u32 flags; u64 reserved; } PlatDevIfData;
typedef struct { u32 size; u16 vid, pid, ver; } PlatHidAttr;
typedef struct { u32 packet; u16 buttons; u8 lt, rt; short lx, ly, rx, ry; } PlatXState;
typedef struct { u16 left, right; } PlatXVib;
static void *(WINAPI *p_SetupDiGetClassDevsA)(const PlatGuid *, const char *, void *, u32);
static int (WINAPI *p_SetupDiEnumDeviceInterfaces)(void *, void *, const PlatGuid *, u32, PlatDevIfData *);
static int (WINAPI *p_SetupDiGetDeviceInterfaceDetailA)(void *, PlatDevIfData *, void *, u32, u32 *, void *);
static int (WINAPI *p_SetupDiDestroyDeviceInfoList)(void *);
static void (WINAPI *p_HidD_GetHidGuid)(PlatGuid *);
static u8 (WINAPI *p_HidD_GetAttributes)(void *, PlatHidAttr *);
static u8 (WINAPI *p_HidD_GetFeature)(void *, void *, u32);
static u8 (WINAPI *p_HidD_GetPreparsedData)(void *, void **);
static u8 (WINAPI *p_HidD_FreePreparsedData)(void *);
static i32 (WINAPI *p_HidP_GetCaps)(void *, u16 *);
static u32 (WINAPI *p_XInputGetState)(u32, PlatXState *);
static u32 (WINAPI *p_XInputSetState)(u32, PlatXVib *);
__declspec(dllimport) void *WINAPI LoadLibraryA(const char *);
__declspec(dllimport) void *WINAPI GetProcAddress(void *, const char *);

/* ---- Sony controllers over HID: a reader thread keeps the latest state ---- */
static volatile u32 g_hid_seq;        /* seqlock: odd while the thread writes */
static PlatPad g_hid_pad;
static volatile int g_hid_started, g_hid_open;
static char g_hid_name[64];
static u64 g_hid_reports, g_hid_opens;
static void *hid_find(int *sense) {
    PlatGuid g; p_HidD_GetHidGuid(&g);
    void *set = p_SetupDiGetClassDevsA(&g, 0, 0, 0x12 /* DIGCF_PRESENT | DIGCF_DEVICEINTERFACE */);
    if (!set || set == (void *)-1) return 0;
    void *found = 0;
    for (u32 i = 0; i < 256 && !found; i++) {
        PlatDevIfData d; h_memset(&d, 0, sizeof d); d.size = sizeof d;
        if (!p_SetupDiEnumDeviceInterfaces(set, 0, &g, i, &d)) break;
        static u8 det[1024]; u32 need = 0;
        *(u32 *)det = 8;   /* SP_DEVICE_INTERFACE_DETAIL_DATA_A on x64 */
        if (!p_SetupDiGetDeviceInterfaceDetailA(set, &d, det, sizeof det, &need, 0)) continue;
        const char *path = (const char *)det + 4;
        void *h = CreateFileA(path, 0xC0000000 /* read | write */, 3, 0, 3 /* OPEN_EXISTING */, 0, 0);
        if (h == (void *)-1) h = CreateFileA(path, 0x80000000, 3, 0, 3, 0, 0);
        if (h == (void *)-1) continue;
        PlatHidAttr a; h_memset(&a, 0, sizeof a); a.size = sizeof a;
        int keep = 0;
        if (p_HidD_GetAttributes(h, &a) && a.vid == 0x054C) {
            if (a.pid == 0x05C4 || a.pid == 0x09CC || a.pid == 0x0BA0) { keep = 1; *sense = 0; }
            else if (a.pid == 0x0CE6 || a.pid == 0x0DF2) { keep = 1; *sense = 1; }
        }
        if (keep) {   /* only the game-pad collection (usage page 1, usage 5) delivers the input reports */
            void *pp = 0; u16 caps[32]; h_memset(caps, 0, sizeof caps);
            if (p_HidD_GetPreparsedData(h, &pp) && pp) { p_HidP_GetCaps(pp, caps); p_HidD_FreePreparsedData(pp); if (caps[1] != 1 || (caps[0] != 5 && caps[0] != 4)) keep = 0; }
        }
        if (keep) {
            found = h;
            const char *nm = *sense ? "DualSense" : "DualShock 4"; int k = 0; while (nm[k] && k < 63) { g_hid_name[k] = nm[k]; k++; } g_hid_name[k] = 0;
        } else CloseHandle(h);
    }
    p_SetupDiDestroyDeviceInfoList(set);
    return found;
}
static WINAPI u32 hid_thread(void *arg) {
    (void)arg;
    g_veh_ignore_tid = GetCurrentThreadId();
    for (;;) {
        int sense = 0; void *h = hid_find(&sense);
        if (!h) { Sleep(2000); continue; }
        /* Bluetooth controllers send full reports (with the sensors) only after a feature report has been read:
           the calibration report (0x02 on a DualShock 4, 0x05 on a DualSense; also on USB, harmless) */
        { u8 f[64]; h_memset(f, 0, sizeof f); f[0] = sense ? 0x05 : 0x02; p_HidD_GetFeature(h, f, sense ? 41 : 37); }
        g_hid_open = 1; g_hid_opens++;
        static u8 buf[1024]; u32 got;
        for (;;) {
            got = 0;
            if (!ReadFile(h, buf, sizeof buf, &got, 0) || !got) break;
            PlatPad P = g_hid_pad;
            if (!hid_parse(sense, buf, got, &P)) continue;
            P.kind = g_hid_name;
            __atomic_add_fetch(&g_hid_seq, 1, __ATOMIC_ACQ_REL);
            g_hid_pad = P;
            __atomic_add_fetch(&g_hid_seq, 1, __ATOMIC_ACQ_REL);
            g_hid_reports++;
        }
        CloseHandle(h);
        g_hid_open = 0;
        __atomic_add_fetch(&g_hid_seq, 1, __ATOMIC_ACQ_REL); g_hid_pad.connected = 0; __atomic_add_fetch(&g_hid_seq, 1, __ATOMIC_ACQ_REL);
        Sleep(1000);   /* unplugged / switched off: look again */
    }
    return 0;
}
/* XInput, polled by its own host thread (system calls stay off the game's threads): polling an empty slot is slow,
   so empty slots are tried every 2 seconds */
static volatile u32 g_xin_seq; static PlatPad g_xin_pad; static volatile u8 g_rumble_large, g_rumble_small; static volatile u32 g_rumble_seq;

static int xinput_read(int slot, PlatPad *P) {
    PlatXState st; h_memset(&st, 0, sizeof st);
    if (p_XInputGetState((u32)slot, &st) != 0) return 0;
    u32 b = 0, w = st.buttons;
    if (w & 0x0001) b |= 0x10; if (w & 0x0002) b |= 0x40; if (w & 0x0004) b |= 0x80; if (w & 0x0008) b |= 0x20;   /* d-pad */
    if (w & 0x0010) b |= 0x8;        /* Start -> Options */
    if (w & 0x0020) b |= 0x100000;   /* Back -> touch pad click */
    if (w & 0x0040) b |= 0x2; if (w & 0x0080) b |= 0x4;          /* stick clicks -> L3 / R3 */
    if (w & 0x0100) b |= 0x400; if (w & 0x0200) b |= 0x800;      /* shoulders -> L1 / R1 */
    if (w & 0x1000) b |= 0x4000; if (w & 0x2000) b |= 0x2000; if (w & 0x4000) b |= 0x8000; if (w & 0x8000) b |= 0x1000;   /* A B X Y -> cross circle square triangle */
    if (st.lt > 30) b |= 0x100; if (st.rt > 30) b |= 0x200;
    #define AX(v, flip) (u8)(((flip) ? (32767 - (i32)(v)) : ((i32)(v) + 32768)) >> 8)
    P->lx = AX(st.lx, 0); P->ly = AX(st.ly, 1); P->rx = AX(st.rx, 0); P->ry = AX(st.ry, 1);
    #undef AX
    P->l2 = st.lt; P->r2 = st.rt; P->buttons = b; P->connected = 1; P->has_motion = 0; P->ntouch = 0; P->kind = "XInput controller";
    return 1;
}
static WINAPI u32 xinput_thread(void *arg) {
    (void)arg;
    g_veh_ignore_tid2 = GetCurrentThreadId();
    int slot = -1; u32 rumble_done = 0; u8 rl = 0, rs = 0;
    for (;;) {
        PlatPad P; h_memset(&P, 0, sizeof P);
        if (slot >= 0 && !xinput_read(slot, &P)) slot = -1;
        if (slot < 0) {
            for (int i = 0; i < 4 && slot < 0; i++) if (xinput_read(i, &P)) slot = i;
            if (slot < 0) {
                __atomic_add_fetch(&g_xin_seq, 1, __ATOMIC_ACQ_REL); g_xin_pad.connected = 0; __atomic_add_fetch(&g_xin_seq, 1, __ATOMIC_ACQ_REL);
                Sleep(2000); continue;
            }
        }
        __atomic_add_fetch(&g_xin_seq, 1, __ATOMIC_ACQ_REL); g_xin_pad = P; __atomic_add_fetch(&g_xin_seq, 1, __ATOMIC_ACQ_REL);
        u32 rq = __atomic_load_n(&g_rumble_seq, __ATOMIC_ACQUIRE);
        if (p_XInputSetState && (rq != rumble_done || g_rumble_large != rl || g_rumble_small != rs)) {
            rl = g_rumble_large; rs = g_rumble_small; rumble_done = rq;
            PlatXVib v = {(u16)(rl * 257u), (u16)(rs * 257u)}; p_XInputSetState((u32)slot, &v);
        }
        Sleep(4);
    }
    return 0;
}
static int seq_read(volatile u32 *seq, const PlatPad *src, PlatPad *dst) {
    for (int tries = 0; tries < 100; tries++) {
        u32 s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        PlatPad c = *src;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(seq, __ATOMIC_ACQUIRE) == s0) { if (!c.connected) return 0; *dst = c; return 1; }
    }
    return 0;
}
static int g_pad_lib_done, g_pad_no_controllers;
static void plat_pad_start(void) {   /* once, when the game window opens (not in replays) */
    if (g_pad_lib_done) return;
    g_pad_lib_done = 1;
    if (g_pad_no_controllers) return;
    void *hid = LoadLibraryA("hid.dll"), *sa = LoadLibraryA("setupapi.dll");
    if (hid && sa) {
        p_HidD_GetHidGuid = (void (WINAPI *)(PlatGuid *))GetProcAddress(hid, "HidD_GetHidGuid");
        p_HidD_GetAttributes = (u8 (WINAPI *)(void *, PlatHidAttr *))GetProcAddress(hid, "HidD_GetAttributes");
        p_HidD_GetFeature = (u8 (WINAPI *)(void *, void *, u32))GetProcAddress(hid, "HidD_GetFeature");
        p_HidD_GetPreparsedData = (u8 (WINAPI *)(void *, void **))GetProcAddress(hid, "HidD_GetPreparsedData");
        p_HidD_FreePreparsedData = (u8 (WINAPI *)(void *))GetProcAddress(hid, "HidD_FreePreparsedData");
        p_HidP_GetCaps = (i32 (WINAPI *)(void *, u16 *))GetProcAddress(hid, "HidP_GetCaps");
        p_SetupDiGetClassDevsA = (void *(WINAPI *)(const PlatGuid *, const char *, void *, u32))GetProcAddress(sa, "SetupDiGetClassDevsA");
        p_SetupDiEnumDeviceInterfaces = (int (WINAPI *)(void *, void *, const PlatGuid *, u32, PlatDevIfData *))GetProcAddress(sa, "SetupDiEnumDeviceInterfaces");
        p_SetupDiGetDeviceInterfaceDetailA = (int (WINAPI *)(void *, PlatDevIfData *, void *, u32, u32 *, void *))GetProcAddress(sa, "SetupDiGetDeviceInterfaceDetailA");
        p_SetupDiDestroyDeviceInfoList = (int (WINAPI *)(void *))GetProcAddress(sa, "SetupDiDestroyDeviceInfoList");
        if (p_HidD_GetHidGuid && p_HidD_GetAttributes && p_HidD_GetFeature && p_HidD_GetPreparsedData && p_HidD_FreePreparsedData && p_HidP_GetCaps &&
            p_SetupDiGetClassDevsA && p_SetupDiEnumDeviceInterfaces && p_SetupDiGetDeviceInterfaceDetailA && p_SetupDiDestroyDeviceInfoList) {
            CreateThread(0, 0x40000, (void *)hid_thread, 0, 0, 0); g_hid_started = 1;
        }
    }
    static const char *xin[3] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !p_XInputGetState; i++) {
        void *x = LoadLibraryA(xin[i]);
        if (!x) continue;
        p_XInputGetState = (u32 (WINAPI *)(u32, PlatXState *))GetProcAddress(x, "XInputGetState");
        p_XInputSetState = (u32 (WINAPI *)(u32, PlatXVib *))GetProcAddress(x, "XInputSetState");
    }
    if (p_XInputGetState) CreateThread(0, 0x40000, (void *)xinput_thread, 0, 0, 0);
}
/* the controller to use: a Sony controller (it has the motion sensors) before an XInput one. Memory reads only. */
static int plat_pad_poll(PlatPad *P) {
    h_memset(P, 0, sizeof *P);
    if (g_hid_started && g_hid_open && seq_read(&g_hid_seq, &g_hid_pad, P)) return 1;
    return seq_read(&g_xin_seq, &g_xin_pad, P);
}
static void plat_pad_rumble(u8 large, u8 small) {
    if (large != g_rumble_large || small != g_rumble_small) { g_rumble_large = large; g_rumble_small = small; __atomic_add_fetch(&g_rumble_seq, 1, __ATOMIC_ACQ_REL); }
}

/* ---- mouse capture for aiming ---- */
__declspec(dllimport) int WINAPI SetCursorPos(i32, i32);
__declspec(dllimport) int WINAPI ClientToScreen(void *, i32 *);
__declspec(dllimport) int WINAPI ClipCursor(const i32 *);
static int g_mcap; static i32 g_mcap_cx, g_mcap_cy;
static void plat_mouse_capture(int on) {
    if (on == g_mcap || !g_win.hwnd) return;
    if (on) {
        i32 rc[4]; if (!GetClientRect(g_win.hwnd, rc)) return;
        i32 tl[2] = {rc[0], rc[1]}, br[2] = {rc[2], rc[3]}; ClientToScreen(g_win.hwnd, tl); ClientToScreen(g_win.hwnd, br);
        i32 clip[4] = {tl[0], tl[1], br[0], br[1]}; ClipCursor(clip);
        g_mcap_cx = (tl[0] + br[0]) / 2; g_mcap_cy = (tl[1] + br[1]) / 2; SetCursorPos(g_mcap_cx, g_mcap_cy);
        g_mcap_hide = 1;
    } else { ClipCursor(0); g_mcap_hide = 0; }
    g_mcap = on;
}
/* pointer movement in screen pixels since the last call (captured mode only) */
static int plat_mouse_delta(i32 *dx, i32 *dy) {
    *dx = *dy = 0;
    if (!g_mcap) return 0;
    i32 pt[2]; if (!GetCursorPos(pt)) return 0;
    *dx = pt[0] - g_mcap_cx; *dy = pt[1] - g_mcap_cy;
    if (*dx || *dy) SetCursorPos(g_mcap_cx, g_mcap_cy);
    return 1;
}
