/* e5_audio.c - libSceAudioOut: real port handles; the samples go to the PC's sound device (waveOut, stereo).
 * Included by e3_hle.c. */

typedef struct { int used, type; u32 len, freq, fmt; u64 next_tick; u64 buffers; float gain_l, gain_r; PlatAudio *dev; short *mix;
                 u64 ad_t0, ad_sent, ad_last_ret; u32 ad_clip, ad_peak; u64 ad_hash[4]; u32 ad_nh; u64 rd_last; float *lim; float lim_g; u32 ad_limmin; u64 rd_hash[4]; u32 rd_n; } AudioPort;   /* ad_*: w27_audio --audio-diag */
static int g_opt_mute;   /* --mute: keep the old silent behaviour */

static AudioPort g_aports[16];

/* ---- w27_audio: audio delivery diagnostics (--audio-diag [S], default off) and a WAV dump (--audio-wav) ----
   Per buffer handed to sceAudioOutOutput(s) one record: when (us since the first), the port, the game's frame (flip, last frame
   time), the time since this port's previous output call returned (the game's own work between buffers: Wwise mixing on
   AK::EventManager + AK::LibAudioOut's turn), the time the call blocked for a free device buffer, the longest single 1 ms sleep
   in that wait (a thread that sleeps 1 ms and wakes 10 ms later was not given a processor), how many of our device buffers
   were still queued (0 = the device ran dry: an UNDERRUN - Windows played silence), the device level from waveOutGetPosition,
   the submission time against the ideal schedule (start + samples sent / rate; reset at an underrun), the estimated silence
   an underrun inserted, clipped output samples and the peak. Records go to Logs\captures\audio_diag.csv from a writer thread
   (the audio thread only fills memory); a summary every S seconds (default 10) goes to the log. A probe thread at normal
   priority (the game's audio threads' priority unless --audio-prio) measures how late a 1 ms sleep wakes: CPU starvation of
   normal-priority threads independent of the audio path. --audio-wav: the first sound-device port's output as it was handed
   to the device (audio_raw.wav) and as played, with the estimated underrun silence inserted (audio_played.wav); analyse
   offline with bench\agents\w27_audio\audio_spikes.py. */
static int g_ad_on, g_ad_wav, g_opt_audio_prio, g_opt_audio_mmcss, g_opt_audio_late_read = 1, g_opt_audio_limiter = 1;   /* w28_int: late read + limiter ON by default (w27f; settings audio_late_read / audio_limiter, --no-audio-late-read / --no-audio-limiter); --audio-prio / --audio-mmcss not merged (no option sets them) */
 static float g_opt_audio_test_gain = 1.0f; static volatile u64 g_lr_retry, g_lr_giveup;   /* --audio-test-gain X: diagnostic only (limiter tests at the player's peak levels) */ static u32 g_ad_period = 10;
typedef struct { u64 t_us; volatile u64 ready; u64 smp_off; u32 flip, frame_ms, gap_us, wait_us, maxsleep_us, silence_us, clip, peak, frames, sleeps;
                 i32 late_us, level_us; u32 limmin; u8 port, qb, flags, nbuf; } AdRec;   /* flags: 1 underrun, 2 dropped, 4 samples in the WAV ring, 8 WAV ring full */
enum { AD_CAP = 1 << 18, AD_SMP = 1 << 21 };
static AdRec *g_ad_rec; static short *g_ad_smp;
static volatile u64 g_ad_head, g_ad_tail, g_ad_lost, g_ad_smp_head, g_ad_smp_tail;
static u64 g_ad_t0; static int g_ad_wav_port = -1; static u32 g_ad_wav_freq;
static volatile u32 g_ad_probe_max_us, g_ad_probe_n5, g_ad_probe_n10, g_ad_probe_n20, g_ad_probe_n;
static void *g_ad_csv, *g_ad_raw, *g_ad_played; static u64 g_ad_raw_frames, g_ad_played_frames; static volatile int g_ad_stop, g_ad_finished;
typedef struct { u64 n, under, near, drop, sil_us, clip, frames, stale; u32 gap_max, wait_max, sleep_max, nsleep5, frame_max, peak, lim_min, nlim; i32 late_max, lvl_min; u64 flip0, flip1; } AdWin;
static AdWin g_ad_w, g_ad_all; static u64 g_ad_wend_us;
static void ad_path(char *b, int cap, const char *name) { Sb t = {b, 0, cap}; sb_s(&t, g_cap_dir); sb_s(&t, "\\"); sb_s(&t, name); }
static void ad_wav_header(void *h, u32 freq, u64 frames) {
    u32 data = (u32)(frames * 4 > 0xFFFFFFF0ull ? 0xFFFFFFF0ull : frames * 4);
    struct __attribute__((packed)) { char riff[4]; u32 sz; char wave[4], fmt_[4]; u32 fsz; u16 tag, ch; u32 rate, bps; u16 align, bits; char dat[4]; u32 dsz; } w =
        { {'R','I','F','F'}, data + 36, {'W','A','V','E'}, {'f','m','t',' '}, 16, 1, 2, freq, freq * 4, 4, 16, {'d','a','t','a'}, data };
    plat_write_bin(h, &w, sizeof w);
}
static void ad_probe_thread(void *arg) {
    (void)arg; u64 f = plat_ticks_per_sec();
    while (!g_ad_stop) {
        u64 a = plat_ticks(); plat_sleep_ms(1); u32 us = (u32)((plat_ticks() - a) * 1000000ull / f);
        if (us > g_ad_probe_max_us) g_ad_probe_max_us = us;
        g_ad_probe_n++; if (us > 5000) g_ad_probe_n5++; if (us > 10000) g_ad_probe_n10++; if (us > 20000) g_ad_probe_n20++;
    }
}
static void ad_win_add(AdWin *w, const AdRec *r) {
    if (!w->n) { w->flip0 = r->flip; w->lvl_min = 0x7FFFFFFF; w->late_max = -0x7FFFFFFF; w->lim_min = 1000; }
    if (r->limmin < 1000) { w->nlim++; if (r->limmin < w->lim_min) w->lim_min = r->limmin; }
    w->n++; w->flip1 = r->flip; w->frames += r->frames;
    if (r->flags & 1) w->under++; if (r->flags & 2) w->drop++; if (r->qb <= 1) w->near++; if (r->flags & 16) w->stale++;
    w->sil_us += r->silence_us; w->clip += r->clip;
    if (r->gap_us > w->gap_max) w->gap_max = r->gap_us; if (r->wait_us > w->wait_max) w->wait_max = r->wait_us;
    if (r->maxsleep_us > w->sleep_max) w->sleep_max = r->maxsleep_us; if (r->maxsleep_us > 5000) w->nsleep5++;
    if (r->frame_ms > w->frame_max) w->frame_max = r->frame_ms; if (r->peak > w->peak) w->peak = r->peak;
    if (r->late_us > w->late_max) w->late_max = r->late_us; if (r->level_us >= 0 && r->level_us < w->lvl_min) w->lvl_min = r->level_us;
}
static void ad_win_print(const AdWin *w, const char *what, u64 secs) {
    char l[700]; Sb t = {l, 0, sizeof l};
    sb_s(&t, "    audio diag ("); sb_s(&t, what); sb_s(&t, "): buffers "); sb_d(&t, (i64)w->n); sb_s(&t, " (");
    sb_d(&t, (i64)(secs ? w->frames / secs : 0)); sb_s(&t, " samples/s), underruns "); sb_d(&t, (i64)w->under); sb_s(&t, " (silence ~");
    sb_d(&t, (i64)(w->sil_us / 1000)); sb_s(&t, " ms), near-empty "); sb_d(&t, (i64)w->near); sb_s(&t, ", dropped "); sb_d(&t, (i64)w->drop); sb_s(&t, ", STALE (repeated) buffers "); sb_d(&t, (i64)w->stale);
    sb_s(&t, ", max gap "); sb_d(&t, w->gap_max / 1000); sb_s(&t, " ms, max wait "); sb_d(&t, w->wait_max / 1000);
    sb_s(&t, " ms, max 1-ms sleep "); sb_d(&t, w->sleep_max / 1000); sb_s(&t, " ms (>5 ms: "); sb_d(&t, w->nsleep5); sb_s(&t, ")");
    sb_s(&t, ", min device level "); sb_d(&t, w->lvl_min == 0x7FFFFFFF ? -1 : w->lvl_min / 1000); sb_s(&t, " ms, max lateness "); sb_d(&t, w->late_max / 1000);
    sb_s(&t, " ms, clipped samples "); sb_d(&t, (i64)w->clip); sb_s(&t, " (peak "); sb_d(&t, w->peak); sb_s(&t, "/1000)");
    if (g_opt_audio_limiter) { sb_s(&t, ", limiter active in "); sb_d(&t, w->nlim); sb_s(&t, " buffers (lowest gain "); sb_d(&t, w->lim_min); sb_s(&t, "/1000)"); }
    sb_s(&t, "; flips ");
    sb_d(&t, (i64)w->flip0); sb_s(&t, "-"); sb_d(&t, (i64)w->flip1); sb_s(&t, ", slowest frame "); sb_d(&t, w->frame_max); sb_s(&t, " ms");
    sb_s(&t, "; normal-priority wake-up probe: max "); sb_d(&t, g_ad_probe_max_us / 1000); sb_s(&t, " ms, late >5/10/20 ms ");
    sb_d(&t, g_ad_probe_n5); sb_s(&t, "/"); sb_d(&t, g_ad_probe_n10); sb_s(&t, "/"); sb_d(&t, g_ad_probe_n20); sb_s(&t, " of "); sb_d(&t, g_ad_probe_n); sb_s(&t, "\n");
    say(l);
}
static volatile int g_ad_drain_busy;
static void ad_drain(void) {
    if (__atomic_exchange_n(&g_ad_drain_busy, 1, __ATOMIC_ACQUIRE)) return;
    static char cb[1 << 16]; int cn = 0;
    static short zeros[4096];
    while (g_ad_tail < g_ad_head) {
        AdRec *r = &g_ad_rec[g_ad_tail % AD_CAP];
        if (__atomic_load_n(&r->ready, __ATOMIC_ACQUIRE) != g_ad_tail + 1) break;
        AdRec c = *r; __atomic_store_n(&g_ad_tail, g_ad_tail + 1, __ATOMIC_RELEASE);
        if (g_ad_csv) {
            char l[300]; Sb t = {l, 0, sizeof l};
            sb_d(&t, (i64)c.t_us); sb_s(&t, ","); sb_d(&t, c.port); sb_s(&t, ","); sb_d(&t, c.flip); sb_s(&t, ","); sb_d(&t, c.frame_ms); sb_s(&t, ",");
            sb_d(&t, c.gap_us); sb_s(&t, ","); sb_d(&t, c.wait_us); sb_s(&t, ","); sb_d(&t, c.maxsleep_us); sb_s(&t, ","); sb_d(&t, c.sleeps); sb_s(&t, ",");
            sb_d(&t, c.qb); sb_s(&t, ","); sb_d(&t, c.nbuf); sb_s(&t, ","); sb_d(&t, c.level_us); sb_s(&t, ","); sb_d(&t, c.late_us); sb_s(&t, ",");
            sb_d(&t, c.flags & 1); sb_s(&t, ","); sb_d(&t, (c.flags >> 1) & 1); sb_s(&t, ","); sb_d(&t, c.silence_us); sb_s(&t, ","); sb_d(&t, c.clip); sb_s(&t, ",");
            sb_d(&t, c.peak); sb_s(&t, ","); sb_d(&t, c.frames); sb_s(&t, ","); sb_d(&t, (c.flags >> 4) & 1); sb_s(&t, ","); sb_d(&t, c.limmin); sb_s(&t, "\n");
            if (cn + t.n >= (int)sizeof cb) { plat_write_bin(g_ad_csv, cb, (u64)cn); cn = 0; }
            for (int i = 0; i < t.n; i++) cb[cn++] = l[i];
        }
        if (g_ad_raw && (int)c.port == g_ad_wav_port) {
            if (c.silence_us && g_ad_played) { u64 z = (u64)c.silence_us * g_ad_wav_freq / 1000000ull; g_ad_played_frames += z;
                while (z) { u64 k = z > 2048 ? 2048 : z; plat_write_bin(g_ad_played, zeros, k * 4); z -= k; } }
            if (c.flags & 4) {
                u64 off = c.smp_off % AD_SMP; u64 n1 = c.frames; if (off + n1 > AD_SMP) n1 = AD_SMP - off;
                plat_write_bin(g_ad_raw, g_ad_smp + off * 2, n1 * 4); if (n1 < c.frames) plat_write_bin(g_ad_raw, g_ad_smp, (c.frames - n1) * 4);
                if (g_ad_played) { plat_write_bin(g_ad_played, g_ad_smp + off * 2, n1 * 4); if (n1 < c.frames) plat_write_bin(g_ad_played, g_ad_smp, (c.frames - n1) * 4); }
                __atomic_store_n(&g_ad_smp_tail, c.smp_off + c.frames, __ATOMIC_RELEASE);
            } else { u64 z = c.frames; while (z) { u64 k = z > 2048 ? 2048 : z; plat_write_bin(g_ad_raw, zeros, k * 4); if (g_ad_played) plat_write_bin(g_ad_played, zeros, k * 4); z -= k; } }
            g_ad_raw_frames += c.frames; g_ad_played_frames += c.frames;
        }
        while (c.t_us >= g_ad_wend_us) {
            if (g_ad_w.n) ad_win_print(&g_ad_w, "last period", g_ad_period);
            h_memset(&g_ad_w, 0, sizeof g_ad_w); g_ad_probe_max_us = 0; g_ad_probe_n5 = g_ad_probe_n10 = g_ad_probe_n20 = g_ad_probe_n = 0;
            g_ad_wend_us += (u64)g_ad_period * 1000000ull;
        }
        ad_win_add(&g_ad_w, &c); ad_win_add(&g_ad_all, &c);
    }
    if (cn && g_ad_csv) plat_write_bin(g_ad_csv, cb, (u64)cn);
    __atomic_store_n(&g_ad_drain_busy, 0, __ATOMIC_RELEASE);
}
static void ad_writer_thread(void *arg) {
    (void)arg;
    while (!g_ad_stop) { plat_sleep_ms(200); ad_drain(); }
}
static void audio_diag_init(void) {
    static int done; if (done || (!g_ad_on && !g_ad_wav)) return; done = 1;
    if (!g_ad_on) g_ad_on = 1;
    g_ad_rec = (AdRec *)plat_alloc_rw((u64)AD_CAP * sizeof(AdRec));
    if (g_ad_wav) g_ad_smp = (short *)plat_alloc_rw((u64)AD_SMP * 4);
    if (!g_ad_rec) { g_ad_on = g_ad_wav = 0; say("  audio diag: no memory - off\n"); return; }
    g_ad_t0 = plat_ticks(); g_ad_wend_us = (u64)g_ad_period * 1000000ull;
    char p[600]; ad_path(p, sizeof p, "audio_diag.csv"); g_ad_csv = plat_open_write(p);
    if (g_ad_csv) { const char *hd = "t_us,port,flip,frame_ms,gap_us,wait_us,maxsleep_us,sleeps,queued_before,nbuf,level_us,late_us,underrun,dropped,silence_us,clipped,peak,frames,stale,limiter_min\n"; plat_write_bin(g_ad_csv, hd, (u64)str_len(hd)); }
    say("  audio diag (w27_audio): on, summary every "); say_dec(g_ad_period); say(" s, records "); say(p); say(g_ad_csv ? "" : " (could not be created)");
    say("; device buffers per port "); say_dec(g_plat_audio_nbuf); say(g_opt_audio_late_read ? "; late read on" : ""); say(g_ad_wav ? "; WAV dump on (audio_raw.wav, audio_played.wav)" : ""); say("\n");
    pt_create(ad_writer_thread, 0, 1u << 18);
    pt_create(ad_probe_thread, 0, 1u << 16);
}
/* one record per buffer; called on the game's audio thread after the buffer was handed to the device (st may be 0) */
static void ad_record(AudioPort *p, int port, u64 t_call, const PlatAudioStat *st, const short *mix, u32 n) {
    u64 f = plat_ticks_per_sec(), now = plat_ticks();
    if (g_ad_head - __atomic_load_n(&g_ad_tail, __ATOMIC_ACQUIRE) >= AD_CAP - 64) { __atomic_add_fetch(&g_ad_lost, 1, __ATOMIC_RELAXED); p->ad_last_ret = now; return; }
    u64 i = __atomic_fetch_add(&g_ad_head, 1, __ATOMIC_ACQ_REL);
    AdRec *r = &g_ad_rec[i % AD_CAP];
    r->t_us = (t_call - g_ad_t0) * 1000000ull / f; r->port = (u8)port; r->frames = n;
    r->flip = (u32)g_flips; r->frame_ms = g_flips ? g_frames[(g_flips - 1) % MAX_FRAMES].ms : 0;
    r->gap_us = p->ad_last_ret ? (u32)((t_call - p->ad_last_ret) * 1000000ull / f) : 0;
    r->flags = 0; r->silence_us = 0; r->qb = 0; r->nbuf = 0; r->sleeps = 0; r->wait_us = 0; r->maxsleep_us = 0; r->level_us = -1;
    if (st) {
        r->qb = (u8)st->queued_before; r->nbuf = (u8)(p->dev ? p->dev->nbuf : 0); r->sleeps = st->sleeps;
        r->wait_us = (u32)(st->wait_ticks * 1000000ull / f); r->maxsleep_us = (u32)(st->max_sleep_ticks * 1000000ull / f);
        r->level_us = st->level < 0 ? -1 : (i32)((u64)st->level * 1000000ull / p->freq);
        if (st->dropped) r->flags |= 2;
    }
    /* ideal schedule: the device started at ad_t0 and has been fed ad_sent samples since without a gap */
    u64 ideal = p->ad_t0 + p->ad_sent * f / p->freq;
    if (!p->ad_t0) { p->ad_t0 = t_call; p->ad_sent = 0; ideal = t_call; }
    else if (st && st->queued_before == 0) {   /* the device had played everything: underrun */
        r->flags |= 1; if (t_call > ideal) r->silence_us = (u32)((t_call - ideal) * 1000000ull / f);
        p->ad_t0 = t_call; p->ad_sent = 0; ideal = t_call;
    }
    r->late_us = (i32)(((i64)t_call - (i64)ideal) * 1000000 / (i64)f);
    if (!(r->flags & 2)) p->ad_sent += n;
    r->clip = p->ad_clip; r->peak = p->ad_peak; r->limmin = p->ad_limmin;
    if (mix && p->ad_peak) {   /* stale: this non-silent buffer is an exact copy of one of the 3 before it (the game's ring slot not yet rendered) */
        u64 hv = 1469598103934665603ull; const u32 *w32 = (const u32 *)mix; for (u32 k = 0; k < n; k++) { hv ^= w32[k]; hv *= 1099511628211ull; }
        for (u32 k = 0; k < 3 && k < p->ad_nh; k++) if (p->ad_hash[(p->ad_nh - 1 - k) & 3] == hv) { r->flags |= 16; break; }
        p->ad_hash[p->ad_nh++ & 3] = hv;
    } else if (mix) p->ad_hash[p->ad_nh++ & 3] = 0;
    r->smp_off = 0;
    if (g_ad_smp && port == g_ad_wav_port && mix) {
        u64 h = g_ad_smp_head;
        if (h + n - __atomic_load_n(&g_ad_smp_tail, __ATOMIC_ACQUIRE) <= AD_SMP) {
            for (u32 k = 0; k < n; k++) { u64 o = ((h + k) % AD_SMP) * 2; g_ad_smp[o] = mix[2 * k]; g_ad_smp[o + 1] = mix[2 * k + 1]; }
            r->smp_off = h; r->flags |= 4; g_ad_smp_head = h + n;
        } else r->flags |= 8;
    }
    __atomic_store_n(&r->ready, i + 1, __ATOMIC_RELEASE);
    p->ad_last_ret = plat_ticks();
}
static void audio_diag_finish(void) {
    if (!g_ad_on || g_ad_finished) return; g_ad_finished = 1;
    g_ad_stop = 1; plat_sleep_ms(300);
    for (int i = 0; i < 3000 && __atomic_load_n(&g_ad_drain_busy, __ATOMIC_ACQUIRE); i++) plat_sleep_ms(1);
    ad_drain();
    u64 secs = (plat_ticks() - g_ad_t0) / plat_ticks_per_sec();
    ad_win_print(&g_ad_all, "whole session", secs ? secs : 1);
    if (g_opt_audio_late_read) { say("    audio late read: "); say_dec((i64)g_lr_retry); say(" re-reads of a not yet rendered game buffer, "); say_dec((i64)g_lr_giveup); say(" given up after 15 ms\n"); }
    if (g_ad_lost) { say("    audio diag: "); say_dec((i64)g_ad_lost); say(" records lost (writer too slow)\n"); }
    if (g_ad_raw) { hf_seek(g_ad_raw, 0, 0); ad_wav_header(g_ad_raw, g_ad_wav_freq, g_ad_raw_frames); plat_close(g_ad_raw); }
    if (g_ad_played) { hf_seek(g_ad_played, 0, 0); ad_wav_header(g_ad_played, g_ad_wav_freq, g_ad_played_frames); plat_close(g_ad_played); }
    if (g_ad_csv) plat_close(g_ad_csv);
}
/* --audio-prio N (default 0 = off): the game's audio threads AK::LibAudioOut (calls sceAudioOutOutput) and AK::EventManager (Wwise
   mixing) get Windows priority 1 = HIGHEST (= our renderer threads), 2 = TIME_CRITICAL; --audio-mmcss: also the "Pro Audio"
   multimedia class. Called on every game thread at its start. */
static void audio_thread_start(const char *name, int ps4_prio, u64 ps4_aff) {
    if (!name || name[0] != 'A' || name[1] != 'K' || name[2] != ':') return;
    int is_out = str_eq(name, "AK::LibAudioOut"), is_mix = str_eq(name, "AK::EventManager");
    int ok = -1;
    if ((is_out || is_mix) && (g_opt_audio_prio || g_opt_audio_mmcss)) ok = plat_audio_thread_prio(g_opt_audio_prio >= 2 ? 15 : g_opt_audio_prio == 1 ? 2 : 0, g_opt_audio_mmcss);
    if (g_ad_on || ok >= 0) {
        char l[300]; Sb t = {l, 0, sizeof l};
        sb_s(&t, "  audio thread \""); sb_s(&t, name); sb_s(&t, "\" started: PS4 priority "); sb_d(&t, ps4_prio); sb_s(&t, ", affinity "); sb_x(&t, ps4_aff);
        sb_s(&t, ", Windows priority now "); sb_d(&t, plat_this_thread_prio()); if (ok >= 0) sb_s(&t, ok ? " (--audio-prio/--audio-mmcss applied)" : " (--audio-prio/--audio-mmcss: FAILED)"); sb_s(&t, "\n");
        say(l);
    }
}


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
        if (g_ad_on || g_ad_wav) {
            audio_diag_init();
            if (g_ad_wav && p->dev && g_ad_wav_port < 0 && g_ad_smp) {
                char q[600]; g_ad_wav_port = h + 1; g_ad_wav_freq = freq;
                ad_path(q, sizeof q, "audio_raw.wav"); g_ad_raw = plat_open_write(q); if (g_ad_raw) ad_wav_header(g_ad_raw, freq, 0);
                ad_path(q, sizeof q, "audio_played.wav"); g_ad_played = plat_open_write(q); if (g_ad_played) ad_wav_header(g_ad_played, freq, 0);
            }
            say("  audio diag: port "); say_dec(h + 1); say(" opened, type "); say_dec(type); say(", "); say_dec(len); say(" samples at "); say_dec(freq);
            say(" Hz, format "); say_dec(param & 0xFF); say(p->dev ? ", sound device" : ", no sound device"); say(g_ad_wav_port == h + 1 ? ", WAV dump" : ""); say("\n");
        }
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
/* w27_audio --audio-late-read: on the PS4, sceAudioOutOutput(buf) returns when the PREVIOUS buffer has finished and the audio
   hardware reads buf only afterwards, while it plays. Wwise relies on that: AK::LibAudioOut hands over the next slot of its
   2-slot ring ~0.1 ms after the previous call returned, while AK::EventManager may still be rendering into it. We used to
   read buf at once: job_H10 measured 2.8 % of the non-silent buffers (1080p cave) to be exact copies of the buffer two
   before (the slot's stale content) - a repeated 10.7 ms chunk + a skipped one = a click at every such buffer boundary.
   With the option the buffer is read only after the wait for a free device buffer (when the PS4 would start playing it),
   and not earlier than 90 % of a buffer period after the previous read. */
/* w27_audio --audio-limiter: the 7.1 -> stereo fold-down (front + 0.707 x (centre + side + back) + 0.5 x LFE) can reach 3.6x full
   scale; the player's logs show peaks of 2.1-2.6x, which were hard-clipped (distortion in loud scenes). A stereo-linked peak
   limiter instead: ceiling -0.2 dBFS; within the buffer the gain ramps down up to 1 ms ahead of a peak (look-ahead inside the
   buffer, no added latency), recovers with a 150 ms time constant; the gain carries over between buffers. A hard clip stays
   as the last safety. Quiet passages are untouched (gain 1). */
static void ao_limit(AudioPort *p, float *x, u32 n) {
    const float ceil_ = 0.977f, att = 1.0f / 48.0f, rel = 1.0f / (0.15f * (float)p->freq);
    float *need = x + 2 * (u64)n;   /* the port's limiter buffer holds 4 floats per sample: stereo + this */
    u32 m = n;
    for (u32 i = 0; i < m; i++) { float a = x[2 * i] < 0 ? -x[2 * i] : x[2 * i], b = x[2 * i + 1] < 0 ? -x[2 * i + 1] : x[2 * i + 1]; if (b > a) a = b; need[i] = a > ceil_ ? ceil_ / a : 1.0f; }
    for (u32 i = m - 1; i-- > 0; ) { float v = need[i + 1] + att; if (v < need[i]) need[i] = v; }   /* ramp down ahead of a peak */
    float g = p->lim_g, gmin = 1.0f;
    for (u32 i = 0; i < n; i++) {
        float t = need[i];
        g = g + (1.0f - g) * rel; if (g > 0.9995f) g = 1.0f; if (g > t) g = t;   /* (snap: in float the release step underflows near 1 - job_H30 stuck at 0.9998) */
        if (g < gmin) gmin = g;
        float l = x[2 * i] * g, r = x[2 * i + 1] * g;
        if (l > 1) l = 1; if (l < -1) l = -1; if (r > 1) r = 1; if (r < -1) r = -1;
        p->mix[2 * i] = (short)(l * 32767.0f); p->mix[2 * i + 1] = (short)(r * 32767.0f);
    }
    p->lim_g = g; p->ad_limmin = (u32)(gmin * 1000.0f);
}
static void ao_play(AudioPort *p, const void *buf, int port) {
    u64 t_call = g_ad_on ? plat_ticks() : 0; u32 nclip = 0;
    PlatAudioStat st; st.waited = 0;
    if (g_opt_audio_late_read) {
        plat_audio_wait(p->dev, &st);
        /* w27f: instead of a fixed minimum spacing (w27b-e: >= 0.9 period between reads, which kept the device queue from refilling
           once Wwise paced itself - job_H50: queue near empty, 2-4 underruns per run), check the slot: a non-silent buffer whose
           bytes equal what this port delivered two or three calls ago is the ring slot's old content - wait 1 ms and read again
           (at most 15 ms). */
        u32 f0 = p->fmt, bps = (f0 == 3 || f0 == 4 || f0 == 5 || f0 == 7) ? 4 : 2, chn = (f0 == 0 || f0 == 3) ? 1 : (f0 == 1 || f0 == 4) ? 2 : 8;
        u64 nw = (u64)p->len * chn * bps / 8; const u64 *w64 = (const u64 *)buf;
        u64 hv = 0;
        for (int tries = 0; ; tries++) {
            u64 h_ = 1469598103934665603ull, nz = 0; for (u64 k = 0; k < nw; k++) { nz |= w64[k]; h_ ^= w64[k]; h_ *= 1099511628211ull; }
            hv = nz ? h_ : 0;
            int old_ = 0; if (hv) for (u32 k = 1; k <= 3 && k <= p->rd_n; k++) if (k >= 2 && p->rd_hash[(p->rd_n - k) & 3] == hv) old_ = 1;
            if (!old_) break;
            if (tries >= 15) { g_lr_giveup++; break; }
            g_lr_retry++; plat_sleep_ms(1);
        }
        p->rd_hash[p->rd_n++ & 3] = hv;
    }
    u32 f = p->fmt, n = p->len;
    int fl = f == 3 || f == 4 || f == 5 || f == 7, ch = (f == 0 || f == 3) ? 1 : (f == 1 || f == 4) ? 2 : 8;
    const short *si = (const short *)buf; const float *fi = (const float *)buf;
    float pk = 0;
    float *lim = 0;
    if (g_opt_audio_limiter) { if (!p->lim) { p->lim = (float *)plat_alloc_rw((u64)n * 8 * 2 + 64); p->lim_g = 1.0f; } lim = p->lim; }
    for (u32 i = 0; i < n; i++) {
        float c[8];
        for (int k = 0; k < ch; k++) c[k] = fl ? fi[(u64)i * ch + k] : si[(u64)i * ch + k] * (1.0f / 32768.0f);
        float l, r;
        if (ch == 1) l = r = c[0];
        else if (ch == 2) { l = c[0]; r = c[1]; }
        else { l = c[0] + 0.7071f * (c[2] + c[4] + c[6]) + 0.5f * c[3]; r = c[1] + 0.7071f * (c[2] + c[5] + c[7]) + 0.5f * c[3]; }
        l *= p->gain_l * g_opt_audio_test_gain; r *= p->gain_r * g_opt_audio_test_gain;
        if (l != l) l = 0; if (r != r) r = 0;
        float al = l < 0 ? -l : l, ar = r < 0 ? -r : r; if (al > pk) pk = al; if (ar > pk) pk = ar;
        if (al > 1 || ar > 1) nclip++;
        if (lim) { lim[2 * i] = l; lim[2 * i + 1] = r; continue; }
        if (l > 1) l = 1; if (l < -1) l = -1; if (r > 1) r = 1; if (r < -1) r = -1;
        p->mix[2 * i] = (short)(l * 32767.0f); p->mix[2 * i + 1] = (short)(r * 32767.0f);
    }
    p->ad_limmin = 1000;
    if (lim) ao_limit(p, lim, n);
    u64 pq = (u64)(pk * 1000.0f); if (pq > g_audio_peak && pq < 100000) g_audio_peak = pq;
    if (g_ad_on && g_ad_rec) { p->ad_clip = nclip; p->ad_peak = (u32)(pq < 100000 ? pq : 99999); plat_audio_write2(p->dev, p->mix, n, &st); ad_record(p, port, t_call, &st, p->mix, n); }
    else plat_audio_write2(p->dev, p->mix, n, st.waited ? &st : 0);
    g_audio_dev_buffers++;
}
static SYSV int ao_output(int h, const void *buf) {
    AudioPort *p = aport(h);
    if (!p) return (int)0x80260004;
    if (!buf) return 0;
    if (p->dev) ao_play(p, buf, h); else { u64 t0 = g_ad_on ? plat_ticks() : 0; ao_pace(p); if (g_ad_on && g_ad_rec) { p->ad_clip = 0; p->ad_peak = 0; ad_record(p, h, t0, 0, 0, p->len); } }   /* the sound device's clock paces the caller; without one, the timer does */
    p->buffers++; g_audio_buffers_total++;
    return (int)p->len;
}
typedef struct { int handle; const void *ptr; } AoOutParam;
static SYSV int ao_outputs(const AoOutParam *prm, u32 num) {
    AudioPort *first = 0;
    int played = 0;
    for (u32 i = 0; i < num; i++) { AudioPort *p = aport(prm[i].handle); if (p) { if (!first) first = p; p->buffers++; g_audio_buffers_total++; if (p->dev && prm[i].ptr) { ao_play(p, prm[i].ptr, prm[i].handle); played = 1; } } }
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
