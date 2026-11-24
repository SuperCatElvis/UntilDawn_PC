/* e5_gyro_cal.h (w25_fix, --gyro-cal) - gyro zero-rate (bias) removal for DualShock 4 / DualSense motion data.
   Portable (no OS calls): included by e5_system.c and by gyro_cal_test.c.

   Why: the HID reports carry the raw gyro, which has a zero-rate offset (the user's DualSense: -4 / +5 / -3 mrad/s,
   bench\results_dev\w25_gyro_01). Sony's pad library hands the game bias-free angular velocity; the game integrates
   angular velocity x dt itself (STATUS 23), so an offset becomes a steady drift of the "Don't Move" indicator.

   How (v2, after w25_gyrocal_01): the controller is "steady" when acceleration and angular velocity are both steady
   (50 ms averages equal the 1 s averages within small limits, about 1 g). After 1 s steady, the 1 s average of the
   angular velocity IS the offset and the estimate follows it (time constant 0.5 s for the first 2 s learned, then 4 s).
   Once there is an estimate, a steady value 0.5-2 deg/s away from it is only taken after 5 s steady (more than 2 deg/s
   away: never - a real turn): a wrong estimate recovers (v1 locked itself out: it learned +27 mrad/s yaw from a slow
   movement in the first second and then never accepted the true +5 again - the "fast drift to the right").
   The corrected value then passes a small deadband (default 0.5 deg/s, --gyro-deadband DEG; 0 = none). */
typedef struct {
    float bias[3];          /* rad/s */
    float wl[3], al[3];     /* 50 ms averages of angular velocity and acceleration */
    float ws[3], as[3];     /* 1 s averages */
    float steady_t;         /* seconds steady */
    float still_t;          /* seconds learning (kept for the log: > 0 = learning now) */
    float learned_t;        /* seconds learned in all (0 = no estimate yet) */
    float deadband;         /* rad/s */
    int primed;
    unsigned long long n_still, n_moving;
} GyroCal;

static float gc_abs(float x) { return x < 0 ? -x : x; }
static void gyro_cal_init(GyroCal *g, float deadband_rad) {
    for (int k = 0; k < 3; k++) g->bias[k] = g->wl[k] = g->al[k] = g->ws[k] = g->as[k] = 0;
    g->steady_t = g->still_t = g->learned_t = 0; g->deadband = deadband_rad; g->primed = 0; g->n_still = g->n_moving = 0;
}
/* w: raw angular velocity (rad/s), a: acceleration (g), dt: seconds since the last call; out: corrected rad/s */
static void gyro_cal_step(GyroCal *g, const float w[3], const float a[3], float dt, float out[3]) {
    if (dt <= 0) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    if (!g->primed) { for (int k = 0; k < 3; k++) { g->wl[k] = g->ws[k] = w[k]; g->al[k] = g->as[k] = a[k]; } g->primed = 1; }
    float kf = dt / (dt + 0.05f), ks = dt / (dt + 1.0f);   /* 50 ms / 1 s averages */
    int steady = 1, near = 1, recover = 1;
    for (int k = 0; k < 3; k++) {
        g->wl[k] += (w[k] - g->wl[k]) * kf; g->ws[k] += (w[k] - g->ws[k]) * ks;
        g->al[k] += (a[k] - g->al[k]) * kf; g->as[k] += (a[k] - g->as[k]) * ks;
        if (gc_abs(a[k] - g->al[k]) > 0.03f || gc_abs(g->al[k] - g->as[k]) > 0.015f) steady = 0;   /* acceleration changing: moved */
        if (gc_abs(w[k] - g->wl[k]) > 0.06f || gc_abs(g->wl[k] - g->ws[k]) > 0.015f) steady = 0;   /* angular velocity changing (0.9 deg/s) */
        if (gc_abs(g->ws[k]) > 0.15f) steady = 0;                                                    /* no offset is that large (8.6 deg/s): a turn */
        if (gc_abs(g->ws[k] - g->bias[k]) > 0.0087f) near = 0;                                        /* 0.5 deg/s from the estimate */
        if (gc_abs(g->ws[k] - g->bias[k]) > 0.035f) recover = 0;                                      /* 2 deg/s: a real turn, never an estimate error */
    }
    float am = g->al[0] * g->al[0] + g->al[1] * g->al[1] + g->al[2] * g->al[2];
    if (am < 0.81f || am > 1.21f) steady = 0;                       /* not about 1 g: shaken / in free fall */
    if (steady) { g->steady_t += dt; g->n_still++; } else { g->steady_t = 0; g->n_moving++; }
    int learn = g->steady_t >= 1.0f && (g->learned_t < 2.0f || near || (recover && g->steady_t >= 5.0f));
    if (learn) {
        float tau = g->learned_t < 2.0f ? 0.5f : 4.0f, kb = dt / (dt + tau);
        for (int k = 0; k < 3; k++) g->bias[k] += (g->ws[k] - g->bias[k]) * kb;
        g->learned_t += dt; g->still_t += dt;
    } else g->still_t = 0;
    for (int k = 0; k < 3; k++) {
        float c = w[k] - g->bias[k];
        if (gc_abs(c) < g->deadband) c = 0;
        out[k] = c;
    }
}
