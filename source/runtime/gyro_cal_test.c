/* gyro_cal_test.c (w25_fix) - synthetic checks of e5_gyro_cal.h. Build: gcc -O2 -o /tmp/gct gyro_cal_test.c -lm
   The game integrates angular velocity x dt (STATUS 23); the "Don't Move" indicator follows that angle. Each case
   feeds 100 Hz reads (the game's pad thread polls every ~10 ms) and integrates the output as the game does. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "e5_gyro_cal.h"
#define D2R 0.0174532925f
static unsigned rs = 12345; static float urand(void) { rs = rs * 1103515245u + 12345u; return ((rs >> 8) & 0xFFFF) / 65535.0f; }
static float gauss(void) { float u = urand() * 0.999f + 1e-6f, v = urand(); return sqrtf(-2 * logf(u)) * cosf(6.2831853f * v); }
static int fails;
static void check(const char *what, double v, double lo, double hi) {
    int ok = v >= lo && v <= hi; if (!ok) fails++;
    printf("  %-62s %10.3f  [%g, %g] %s\n", what, v, lo, hi, ok ? "ok" : "FAIL");
}
/* run: secs of samples; motion(t, w) adds the true angular velocity (rad/s); returns integrated angle (deg) per axis with
   and without calibration over [t0, secs] */
typedef void (*MotionFn)(float t, float w[3]);
static void run(float secs, float t0, const float bias[3], float noise, MotionFn mf, int cal, float db_deg, double ang[3]) {
    GyroCal g; gyro_cal_init(&g, db_deg * D2R);
    ang[0] = ang[1] = ang[2] = 0;
    for (float t = 0; t < secs; t += 0.01f) {
        float wt[3] = {0, 0, 0}; if (mf) mf(t, wt);
        float w[3], a[3] = {0.002f * gauss(), 1.0f + 0.002f * gauss(), 0.002f * gauss()}, o[3];
        for (int k = 0; k < 3; k++) w[k] = wt[k] + bias[k] + noise * gauss();
        if (cal) gyro_cal_step(&g, w, a, 0.01f, o); else for (int k = 0; k < 3; k++) o[k] = w[k];
        if (t >= t0) for (int k = 0; k < 3; k++) ang[k] += o[k] * 0.01 / D2R;
    }
}
static void m_tremor_soft(float t, float w[3]) { w[0] = 0.4f * D2R * sinf(6.2831853f * 1.5f * t); w[1] = 0.3f * D2R * sinf(6.2831853f * 2.3f * t + 1); w[2] = 0.3f * D2R * sinf(6.2831853f * 0.7f * t + 2); }
/* the user's run w25_gyrocal_01: slow irregular movement in the first seconds (yaw mean ~49 mrad/s), then the pad lies still */
static void m_user_start(float t, float w[3]) { if (t < 2.5f) { w[1] = 0.045f + 0.02f * sinf(6.2831853f * 0.8f * t); w[0] = 0.01f * sinf(6.2831853f * 0.5f * t); } }
/* calibrated on the table for 20 s, then held in the hand with a stronger tremor */
static void m_table_then_hand(float t, float w[3]) { if (t >= 20) { w[0] = 1.5f * D2R * sinf(6.2831853f * 1.5f * t); w[1] = 1.0f * D2R * sinf(6.2831853f * 2.3f * t + 1); w[2] = 0.8f * D2R * sinf(6.2831853f * 0.7f * t + 2); } }
static __attribute__((unused)) void m_tremor(float t, float w[3]) { w[0] = 1.5f * D2R * sinf(6.2831853f * 1.5f * t); w[1] = 1.0f * D2R * sinf(6.2831853f * 2.3f * t + 1); w[2] = 0.8f * D2R * sinf(6.2831853f * 0.7f * t + 2); }
static void m_turn90(float t, float w[3]) { if (t >= 20 && t < 21) w[1] = 90 * D2R; }
static void m_slow(float t, float w[3]) { if (t >= 20 && t < 30) w[1] = 3 * D2R; }
static void m_slow_db(float t, float w[3]) { if (t >= 20 && t < 30) w[1] = 1.5f * D2R; }
int main(void) {
    const float bias[3] = {1.1f * D2R, -2.0f * D2R, 0.6f * D2R};   /* deg/s offsets typical of a DS4 / DualSense */
    double a[3];
    printf("1. lying still 60 s (noise 0.1 deg/s):\n");
    run(60, 0, bias, 0.1f * D2R, 0, 0, 0.5f, a); check("raw: yaw drift over 60 s (deg)", a[1], -121, -119);
    run(60, 2, bias, 0.1f * D2R, 0, 1, 0.5f, a); check("cal: yaw drift 2..60 s (first estimate after 1 s steady) (deg)", a[1], -1.5, 1.5);
    check("cal: pitch drift (deg)", a[0], -1.5, 1.5); check("cal: roll drift (deg)", a[2], -1.5, 1.5);
    run(60, 5, bias, 0.1f * D2R, 0, 1, 0.0f, a); check("cal, no deadband: yaw drift 5..60 s (deg)", a[1], -1.0, 1.0);
    printf("2. held in the hand from the first read (tremor 0.3-0.4 deg/s, as the user's keep-still logs), 60 s:\n");
    run(60, 0, bias, 0.1f * D2R, m_tremor_soft, 0, 0.5f, a); check("raw: yaw drift (deg)", a[1], -125, -115);
    run(60, 10, bias, 0.1f * D2R, m_tremor_soft, 1, 0.5f, a); check("cal: yaw drift 10..60 s (deg)", a[1], -3, 3);
    check("cal: pitch drift 10..60 s (deg)", a[0], -3, 3);
    printf("2b. on the table 20 s, then held with a strong tremor (1-1.5 deg/s), drift 20..80 s:\n");
    run(80, 20, bias, 0.1f * D2R, m_table_then_hand, 1, 0.5f, a); check("cal: yaw drift (deg)", a[1], -3, 3); check("cal: pitch drift (deg)", a[0], -3, 3);
    printf("2c. REGRESSION w25_gyrocal_01: the user's offsets (-4 / +5 / -3 mrad/s), slow movement in the first 2.5 s, then still:\n");
    { const float ub[3] = {-0.004f, 0.005f, -0.003f};
      run(70, 10, ub, 0.002f, m_user_start, 0, 0.5f, a); check("raw: yaw drift 10..70 s (deg)", a[1], 16, 19);
      run(70, 10, ub, 0.002f, m_user_start, 1, 0.0f, a); check("cal (no deadband): yaw drift 10..70 s (deg)", a[1], -1, 1);
      run(70, 10, ub, 0.002f, m_user_start, 1, 0.5f, a); check("cal: yaw drift 10..70 s (deg)", a[1], -1, 1); }
    printf("3. real movement is passed through (after 20 s on the table):\n");
    run(25, 19, bias, 0.1f * D2R, m_turn90, 1, 0.5f, a); check("cal: 90 deg turn in 1 s, measured (deg)", a[1], 87, 93);
    run(35, 19, bias, 0.1f * D2R, m_slow, 1, 0.5f, a); check("cal: slow 3 deg/s for 10 s = 30 deg, measured", a[1], 27, 33);
    run(35, 19, bias, 0.1f * D2R, m_slow_db, 1, 0.5f, a); check("cal: perfectly constant 1.5 deg/s for 10 s = 15 deg (taken as offset after 5 s)", a[1], 6, 17);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
