/* e5_audio.c - libSceAudioOut: real port handles; the samples go to the PC's sound device (waveOut, stereo).
 * Included by e3_hle.c. */

typedef struct { int used, type; u32 len, freq, fmt; u64 next_tick; u64 buffers; float gain_l, gain_r; PlatAudio *dev; short *mix; } AudioPort;
static int g_opt_mute;   /* --mute: keep the old silent behaviour */

static AudioPort g_aports[16];


/* sceAudioOutGetPortState reports the channels of the format the port was opened with (1, 2 or 8). E6 used to
   report 2 for every port; Wwise opens an 8-channel float port, and with "2" its mixer buffers and its output
   disagreed: float samples landed in freed Wwise objects and AK::EventManager crashed ~1 s into the first gameplay
   scene, every run. Reporting 8 fixed it (17 Oct: 6,000 frames, 4,000 of them in gameplay, no crash).
   --audio-port-stereo restores the old answer; --no-audio-out refuses every port (the game cannot run that way). */
static int g_opt_no_audio_out, g_opt_port_channels = 1;
static SYSV int ao_init(void) { TRACE("sceAudioOutInit", 0, 0, 0); return 0; }
static SYSV int ao_open(int user, int type, int index, u32 len, u32 freq, u32 param) {
    TRACE("sceAudioOutOpen", type, len, freq); (void)user; (void)index;
    if (!len || !freq) return (int)0x80260005;
    if (g_opt_no_audio_out) { tr("    audio port refused (--no-audio-out)\n"); return (int)0x8026000A; }
    pm_lock(&g_audio_lock);
    int h = -1;
    for (int i = 0; i < 16; i++) if (!g_aports[i].used) { h = i; break; }
    if (h >= 0) { AudioPort *p = &g_aports[h]; h_memset(p, 0, sizeof *p); p->used = 1; p->type = type; p->len = len; p->freq = freq; p->fmt = param & 0xFF; p->gain_l = p->gain_r = 1.0f; }
    pm_unlock(&g_audio_lock);
    if (h < 0) return (int)0x8026000A;   /* port full */
    if (!g_opt_mute) {
        AudioPort *p = &g_aports[h];
        p->mix = (short *)plat_alloc_rw((u64)len * 4 + 64);
        p->dev = p->mix ? plat_audio_open(freq, len) : 0;
        tr(p->dev ? "    audio port -> PC sound device (stereo)\n" : "    audio port: no PC sound device, silent\n");
    }
    tr("    audio port "); tr_dec(h + 1); tr(": type "); tr_dec(type); tr(", "); tr_dec(len); tr(" samples @ "); tr_dec(freq); tr(" Hz, format "); tr_dec(param & 0xFF); tr("\n");
    return h + 1;
}
static AudioPort *aport(int h) { return (h >= 1 && h <= 16 && g_aports[h - 1].used) ? &g_aports[h - 1] : 0; }
/* each output call blocks until the previously queued buffer has 'played' */
static void ao_pace(AudioPort *p) {
    u64 f = plat_ticks_per_sec(), now = plat_ticks();
    u64 dur = (u64)p->len * f / p->freq;
    if (p->next_tick > now) {
        u64 wait_us = (p->next_tick - now) * 1000000ull / f;
        if (wait_us >= 1000) plat_sleep_ms((u32)(wait_us / 1000));
        now = plat_ticks();
    }
    p->next_tick = (p->next_tick > now ? p->next_tick : now) + dur;
}
/* PS4 port formats: 0 S16 mono, 1 S16 stereo, 2 S16 8ch, 3 float mono, 4 float stereo, 5 float 8ch, 6 S16 8ch (std), 7 float 8ch (std).
   8 channels are L R C LFE and two surround pairs; for the PC's stereo they fold down with the usual -3 dB for centre and surrounds. */
static void ao_play(AudioPort *p, const void *buf) {
    u32 f = p->fmt, n = p->len;
    int fl = f == 3 || f == 4 || f == 5 || f == 7, ch = (f == 0 || f == 3) ? 1 : (f == 1 || f == 4) ? 2 : 8;
    const short *si = (const short *)buf; const float *fi = (const float *)buf;
    float pk = 0;
    for (u32 i = 0; i < n; i++) {
        float c[8];
        for (int k = 0; k < ch; k++) c[k] = fl ? fi[(u64)i * ch + k] : si[(u64)i * ch + k] * (1.0f / 32768.0f);
        float l, r;
        if (ch == 1) l = r = c[0];
        else if (ch == 2) { l = c[0]; r = c[1]; }
        else { l = c[0] + 0.7071f * (c[2] + c[4] + c[6]) + 0.5f * c[3]; r = c[1] + 0.7071f * (c[2] + c[5] + c[7]) + 0.5f * c[3]; }
        l *= p->gain_l; r *= p->gain_r;
        if (l != l) l = 0; if (r != r) r = 0;
        float al = l < 0 ? -l : l, ar = r < 0 ? -r : r; if (al > pk) pk = al; if (ar > pk) pk = ar;
        if (l > 1) l = 1; if (l < -1) l = -1; if (r > 1) r = 1; if (r < -1) r = -1;
        p->mix[2 * i] = (short)(l * 32767.0f); p->mix[2 * i + 1] = (short)(r * 32767.0f);
    }
    u64 pq = (u64)(pk * 1000.0f); if (pq > g_audio_peak && pq < 100000) g_audio_peak = pq;
    plat_audio_write(p->dev, p->mix, n);
    g_audio_dev_buffers++;
}
static SYSV int ao_output(int h, const void *buf) {
    AudioPort *p = aport(h);
    if (!p) return (int)0x80260004;
    if (!buf) return 0;
    if (p->dev) ao_play(p, buf); else ao_pace(p);   /* the sound device's clock paces the caller; without one, the timer does */
    p->buffers++; g_audio_buffers_total++;
    return (int)p->len;
}
typedef struct { int handle; const void *ptr; } AoOutParam;
static SYSV int ao_outputs(const AoOutParam *prm, u32 num) {
    AudioPort *first = 0;
    int played = 0;
    for (u32 i = 0; i < num; i++) { AudioPort *p = aport(prm[i].handle); if (p) { if (!first) first = p; p->buffers++; g_audio_buffers_total++; if (p->dev && prm[i].ptr) { ao_play(p, prm[i].ptr); played = 1; } } }
    if (first && !played) ao_pace(first);
    return 0;
}
static SYSV int ao_close(int h) {
    AudioPort *p = aport(h);
    if (p) { PlatAudio *d = p->dev; p->dev = 0; plat_audio_close(d); p->used = 0; }   /* (the mix buffer is kept: a few KB per open) */
    return 0;
}
/* sceAudioOutSetVolume(handle, flag, vol[]): flag bit k sets channel k; 32768 is 0 dB */
static SYSV int ao_set_volume(int h, u32 flag, const int *vol) {
    AudioPort *p = aport(h);
    if (!p) return (int)0x80260004;
    if (!vol) return 0;
    if (flag & 1) p->gain_l = vol[0] / 32768.0f;
    if (flag & 2) p->gain_r = vol[1] / 32768.0f;
    if (p->fmt == 0 || p->fmt == 3) p->gain_r = p->gain_l;
    return 0;
}
static SYSV int ao_ok(void) { return 0; }
typedef struct { u16 output; u8 channel, r0; short volume; u16 reroute; u64 flag; u64 r1[2]; } AoPortState;
static SYSV int ao_port_state(int h, AoPortState *st) {
    if (!aport(h)) return (int)0x80260004;
    if (st) {
        h_memset(st, 0, sizeof *st); st->output = 1; st->volume = 127; st->channel = 2;
        if (g_opt_port_channels) { u32 f = aport(h)->fmt; st->channel = (f == 0 || f == 3) ? 1 : (f == 1 || f == 4) ? 2 : 8; }
    }
    return 0;
}

static const HleEntry g_hle_audio[] = {
    {"sceAudioOutInit", (void *)ao_init}, {"sceAudioOutOpen", (void *)ao_open}, {"sceAudioOutOutput", (void *)ao_output},
    {"sceAudioOutOutputs", (void *)ao_outputs}, {"sceAudioOutClose", (void *)ao_close}, {"sceAudioOutSetVolume", (void *)ao_set_volume},
    {"sceAudioOutGetPortState", (void *)ao_port_state}, {"sceAudioOutSetMixLevelPadSpk", (void *)ao_ok},
    {0, 0}};
