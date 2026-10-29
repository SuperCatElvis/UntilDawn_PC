/* e3_hle.c - high-level emulation of the PS4 system functions needed during startup.
 * Included by e3_loader.c. All functions called by game code use the System V ABI (SYSV). */

static void nid_of(const char *name, char out[12]);   /* e5_nid.c */
static int idx_of(const char *name) {   /* the import with this name's NID (and the import is named after it from now on) */
    char t[12]; nid_of(name, t);
    for (int i = 0; i < N_IMPORTS; i++) if (str_eq(g_imports[i].nid, t)) { g_imports[i].name = name; return i; }
    return -1;
}
#define TRACE(nm, a, b, c) do { static int ix_ = -2; if (ix_ == -2 && N_IMPORTS) ix_ = idx_of(nm); \
    if (ix_ >= 0) note_call(ix_, (u64)__builtin_return_address(0), (u64)(a), (u64)(b), (u64)(c)); } while (0)

static void fatal_from_game(const char *what) {
    say("\n*** game called "); say(what); say(" during constructor #"); say_dec(g_cur_ctor);
    say(" (last OS call "); say(g_last_import); say(")\n");
    if (g_on_fatal) g_on_fatal();
    plat_exit(4);
}
static SYSV void hle_unresolved(void) { fatal_from_game("an import with no HLE entry"); }

/* ---------------- data imports ---------------- */
static u64 g_stack_guard = 0x2D7A11E5C0DE5EEDull;
static u8 g_stdin_obj[512], g_stdout_obj[512], g_stderr_obj[512];
static u8 g_zero_blob[4096];
/* t152: the libc data objects _Flt and _Dbl: the float / double properties of <float.h> as the PS4 libc lays them out
   (80 bytes each: DIG, MANT_DIG, MAX_10_EXP, MAX_EXP, MIN_10_EXP, MIN_EXP as ints, then EPSILON at +0x20, MAX at
   +0x30, MIN at +0x40, each in a 16-byte slot; layout checked against the game's own libc.prx exports). Until t151
   both were the zero blob, so the game's code read FLT_EPSILON = FLT_MAX = FLT_MIN = 0 (142 reads of _Flt in the
   eboot). The collectible arm: the two-bone IK (eboot 0x878c50 -> 0x8788e0) guards a length against _Flt.MIN; with 0
   the guard let a zero length through and the upper arm's rotation became NaN (reproduced here by running the IK on
   the memory recorded by --ik-probe: NaN with zeros, a finite unit quaternion with these values). */
typedef struct { i32 dig, mant_dig, max_10_exp, max_exp, min_10_exp, min_exp, pad[2]; union { float f; double d; u8 b[16]; } eps, max, min; } FloatProps;
static __attribute__((aligned(16))) FloatProps g_flt_props = {6, 24, 38, 128, -37, -125, {0, 0}, {.f = 1.1920928955078125e-07f}, {.f = 3.40282346638528859812e+38f}, {.f = 1.17549435082228750797e-38f}};
static __attribute__((aligned(16))) FloatProps g_dbl_props = {15, 53, 308, 1024, -307, -1021, {0, 0}, {.d = 2.2204460492503131e-16}, {.d = 1.7976931348623157e+308}, {.d = 2.2250738585072014e-308}};
_Static_assert(sizeof(FloatProps) == 80, "_Flt / _Dbl are 80 bytes");
static u8 g_typeinfo_vt[3][128];
static int g_errno;

/* ---------------- memory / strings ---------------- */
static SYSV void *h_memcpy(void *d, const void *s, u64 n) { mem_copy(d, s, n); return d; }   /* t107b: word / REP MOVSB copies (e3_platform.h) */
static SYSV void *h_memmove(void *d, const void *s, u64 n) {
    u8 *a = (u8 *)d; const u8 *b = (const u8 *)s;
    if (a < b || a >= b + n) mem_copy_fwd(a, b, n);
    else { a += n; b += n; while (n >= 8 && (u64)(a - b) >= 8) { a -= 8; b -= 8; n -= 8; *(u64_ua *)a = *(const u64_ua *)b; } while (n--) *--a = *--b; }
    return d;
}
static SYSV void *h_memset(void *d, int c, u64 n) { mem_fill(d, c, n); return d; }
static void hw_note(u64 a, u64 n, int kind);
static int lz_ensure(u64 a, u64 n);   /* t111: commit game memory reserved for first use (Windows file calls do not fault on it) */   /* e6_heapwatch.c: journal of our writes into game memory (--watch-heap) */
static SYSV int h_memcmp(const void *x, const void *y, u64 n) {
    const u8 *a = (const u8 *)x, *b = (const u8 *)y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
static SYSV void *h_memchr(const void *s, int c, u64 n) { const u8 *p = (const u8 *)s; for (; n; n--, p++) if (*p == (u8)c) return (void *)p; return 0; }
static SYSV u64 h_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static SYSV int h_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
static SYSV int h_strncmp(const char *a, const char *b, u64 n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (u8)*a - (u8)*b; if (!*a) return 0; }
    return 0;
}
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static SYSV int h_strcasecmp(const char *a, const char *b) { while (*a && lc(*a) == lc(*b)) { a++; b++; } return lc((u8)*a) - lc((u8)*b); }
static SYSV int h_strncasecmp(const char *a, const char *b, u64 n) {
    for (; n; n--, a++, b++) { if (lc(*a) != lc(*b)) return lc((u8)*a) - lc((u8)*b); if (!*a) return 0; }
    return 0;
}
static SYSV char *h_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static SYSV char *h_strncpy(char *d, const char *s, u64 n) { u64 i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static SYSV char *h_strcat(char *d, const char *s) { h_strcpy(d + h_strlen(d), s); return d; }
static SYSV char *h_strncat(char *d, const char *s, u64 n) { char *e = d + h_strlen(d); while (n-- && *s) *e++ = *s++; *e = 0; return d; }
static SYSV char *h_strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
static SYSV char *h_strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
static SYSV char *h_strstr(const char *h, const char *n) {
    u64 ln = h_strlen(n); if (!ln) return (char *)h;
    for (; *h; h++) if (!h_strncmp(h, n, ln)) return (char *)h;
    return 0;
}
static SYSV char *h_strpbrk(const char *s, const char *a) { for (; *s; s++) if (h_strchr(a, *s)) return (char *)s; return 0; }
static SYSV u64 h_strcspn(const char *s, const char *r) { u64 n = 0; while (s[n] && !h_strchr(r, s[n])) n++; return n; }
static char *g_tok;
static SYSV char *h_strtok(char *s, const char *d) {
    if (s) g_tok = s;
    if (!g_tok) return 0;
    while (*g_tok && h_strchr(d, *g_tok)) g_tok++;
    if (!*g_tok) return 0;
    char *r = g_tok;
    while (*g_tok && !h_strchr(d, *g_tok)) g_tok++;
    if (*g_tok) *g_tok++ = 0; else g_tok = 0;
    return r;
}
static SYSV i64 h_strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0; if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = s[0] == '0' ? 8 : 10;
    i64 v = 0;
    for (;; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
static SYSV u64 h_wcslen(const u16 *s) { u64 n = 0; while (s[n]) n++; return n; }  /* PS4 wchar_t is 16-bit */
static SYSV u16 *h_wcsncpy(u16 *d, const u16 *s, u64 n) { u64 i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static u32 g_rand = 1;
static SYSV int h_rand(void) { g_rand = g_rand * 1103515245u + 12345u; return (int)((g_rand >> 16) & 0x7fff); }
static SYSV void h_srand(u32 s) { g_rand = s; }
static SYSV void h_qsort(void *base, u64 n, u64 sz, int (SYSV *cmp)(const void *, const void *)) {
    u8 *b = (u8 *)base; u8 tmp[512];
    if (sz > sizeof tmp) { fatal_from_game("qsort with element > 512 bytes"); return; }
    for (u64 i = 1; i < n; i++)               /* insertion sort: fine for startup */
        for (u64 j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--) {
            h_memcpy(tmp, b + j * sz, sz); h_memcpy(b + j * sz, b + (j - 1) * sz, sz); h_memcpy(b + (j - 1) * sz, tmp, sz);
        }
}

/* ---------------- heap (host) ---------------- */
static SYSV void *h_malloc(u64 n) { TRACE("malloc", n, 0, 0); void *p = malloc(n ? n : 1); if (p) h_memset(p, 0, n); return p; }
static SYSV void h_free(void *p) { if (p) free(p); }
static SYSV void *h_calloc(u64 a, u64 b) { return calloc(a ? a : 1, b ? b : 1); }
static SYSV void *h_realloc(void *p, u64 n) { return realloc(p, n ? n : 1); }
static SYSV void *h_memalign(u64 al, u64 n) {
    TRACE("memalign", al, n, 0);
    if (al < 16) al = 16;
    u8 *raw = (u8 *)malloc(n + al + 16);
    if (!raw) return 0;
    u64 a = ((u64)raw + 16 + al - 1) & ~(al - 1);
    ((void **)a)[-1] = raw;   /* leaked on free: E3 only */
    h_memset((void *)a, 0, n);
    return (void *)a;
}

/* ---------------- C++ runtime ---------------- */
static SYSV int h_cxa_guard_acquire(u64 *g) { return *(u8 *)g ? 0 : 1; }
static SYSV void h_cxa_guard_release(u64 *g) { *(u8 *)g = 1; }
static int g_atexit_count;
static SYSV int h_cxa_atexit(void *f, void *a, void *d) { (void)f; (void)a; (void)d; g_atexit_count++; return 0; }
static SYSV int h_atexit(void *f) { (void)f; g_atexit_count++; return 0; }
static SYSV void h_pure_virtual(void) { fatal_from_game("__cxa_pure_virtual"); }
static SYSV void h_terminate(void) { fatal_from_game("std::terminate"); }
static SYSV void h_cxa_throw(void *o, void *t, void *d) { (void)o; (void)t; (void)d; fatal_from_game("__cxa_throw (C++ exception)"); }
static SYSV void *h_cxa_alloc_exc(u64 n) { return h_malloc(n + 128); }
static SYSV void h_unwind_resume(void *e) { (void)e; fatal_from_game("_Unwind_Resume"); }
static SYSV void h_stack_chk_fail(void) { fatal_from_game("__stack_chk_fail (stack corruption detected)"); }
static SYSV void h_assert(const char *msg) { say("\n_Assert: "); say(msg ? msg : "?"); say("\n"); fatal_from_game("_Assert"); }
static SYSV void h_exit(int c) { say("\ngame called exit("); say_dec(c); say(")\n"); if (g_on_fatal) g_on_fatal(); plat_exit(5); }
static int *thread_errno(void);   /* e4_threads.c: this thread's errno */
static SYSV int *h_error(void) { return thread_errno(); }
static SYSV void *h_tls_get_addr(u64 *ti) {
    TRACE("__tls_get_addr", ti[0], ti[1], 0);
    u8 *tp = (u8 *)plat_get_thread_pointer();
    return tp - g_tls_offset + ti[1];
}

/* ---------------- math ---------------- */
#define PI_ 3.14159265358979323846
/* t147: every libm call the game makes is counted, and a call that returns NaN from non-NaN arguments is recorded
   (function, arguments, the game's call site, flip) - the collectible arm's bones are NaN that no FP trap saw being
   made. Printed at the next flip (mnan_report). */
typedef struct { const char *fn; u64 ret, frame; double a, b; u32 n; } MNan;
static MNan g_mnan[96]; static volatile int g_nmnan, g_mnan_lock, g_mnan_printed;
static volatile u64 g_mcalls[40];
static void mnan_note(const char *fn, void *ret, double a, double b) {
    int n = __atomic_load_n(&g_nmnan, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (g_mnan[i].fn == fn && g_mnan[i].ret == (u64)ret) { __atomic_add_fetch(&g_mnan[i].n, 1, __ATOMIC_RELAXED); return; }
    while (__atomic_exchange_n(&g_mnan_lock, 1, __ATOMIC_ACQUIRE)) {}
    n = g_nmnan;
    if (n < 96) { MNan *m = &g_mnan[n]; m->fn = fn; m->ret = (u64)ret; m->frame = g_plat_frame; m->a = a; m->b = b; m->n = 1; __atomic_store_n(&g_nmnan, n + 1, __ATOMIC_RELEASE); }
    __atomic_store_n(&g_mnan_lock, 0, __ATOMIC_RELEASE);
}
#define MN1(k, name, r, a) do { g_mcalls[k]++; if ((r) != (r) && (a) == (a)) mnan_note(name, __builtin_return_address(0), (double)(a), 0); } while (0)
#define MN2(k, name, r, a, b) do { g_mcalls[k]++; if ((r) != (r) && (a) == (a) && (b) == (b)) mnan_note(name, __builtin_return_address(0), (double)(a), (double)(b)); } while (0)
static const char *const g_mfnames[40] = {"sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "atan2f", "expf", "exp2f", "powf", "fmodf", "modff",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "exp", "exp2", "pow", "fmod", "fmax", "fmin", "ldexp", "modf", "hypot",
    "_Sin", "_FSin", "_Log", "_FLog", "_FSinh"};
static SYSV float h_sinf(float x) { float r = (float)sin(x); MN1(0, "sinf", r, x); return r; }
static SYSV float h_cosf(float x) { float r = (float)cos(x); MN1(1, "cosf", r, x); return r; }
static SYSV float h_tanf(float x) { float r = (float)tan(x); MN1(2, "tanf", r, x); return r; }
static SYSV float h_asinf(float x) { float r = (float)asin(x); MN1(3, "asinf", r, x); return r; }
static SYSV float h_acosf(float x) { float r = (float)acos(x); MN1(4, "acosf", r, x); return r; }
static SYSV float h_atanf(float x) { float r = (float)atan(x); MN1(5, "atanf", r, x); return r; }
static SYSV float h_atan2f(float y, float x) { float r = (float)atan2(y, x); MN2(6, "atan2f", r, y, x); return r; }
static SYSV float h_expf(float x) { float r = (float)exp(x); MN1(7, "expf", r, x); return r; }
static SYSV float h_exp2f(float x) { float r = (float)pow(2.0, x); MN1(8, "exp2f", r, x); return r; }
static SYSV float h_powf(float x, float y) { float r = (float)pow(x, y); MN2(9, "powf", r, x, y); return r; }
static SYSV float h_fmodf(float x, float y) { float r = (float)fmod(x, y); MN2(10, "fmodf", r, x, y); return r; }
static SYSV float h_modff(float x, float *ip) { double i; double f = modf(x, &i); *ip = (float)i; g_mcalls[11]++; return (float)f; }
static SYSV double h_sin(double x) { double r = sin(x); MN1(12, "sin", r, x); return r; }
static SYSV double h_cos(double x) { double r = cos(x); MN1(13, "cos", r, x); return r; }
static SYSV double h_tan(double x) { double r = tan(x); MN1(14, "tan", r, x); return r; }
static SYSV double h_asin(double x) { double r = asin(x); MN1(15, "asin", r, x); return r; }
static SYSV double h_acos(double x) { double r = acos(x); MN1(16, "acos", r, x); return r; }
static SYSV double h_atan(double x) { double r = atan(x); MN1(17, "atan", r, x); return r; }
static SYSV double h_atan2(double y, double x) { double r = atan2(y, x); MN2(18, "atan2", r, y, x); return r; }
static SYSV double h_exp(double x) { double r = exp(x); MN1(19, "exp", r, x); return r; }
static SYSV double h_exp2(double x) { double r = pow(2.0, x); MN1(20, "exp2", r, x); return r; }
static SYSV double h_pow(double x, double y) { double r = pow(x, y); MN2(21, "pow", r, x, y); return r; }
static SYSV double h_fmod(double x, double y) { double r = fmod(x, y); MN2(22, "fmod", r, x, y); return r; }
static SYSV double h_fmax(double x, double y) { g_mcalls[23]++; return x > y ? x : y; }
static SYSV double h_fmin(double x, double y) { g_mcalls[24]++; return x < y ? x : y; }
static SYSV double h_ldexp(double x, int e) { g_mcalls[25]++; return ldexp(x, e); }
static SYSV double h_modf(double x, double *ip) { g_mcalls[26]++; return modf(x, ip); }
static SYSV double h_hypot(double x, double y) { double r = sqrt(x * x + y * y); MN2(27, "hypot", r, x, y); return r; }
/* Dinkumware internals: _Sin(x, q): q&1 -> cos; _Log(x, b): 0 ln, 1 log10, 2 log2 */
static SYSV double h__Sin(double x, u32 q) { double r = (q & 1) ? cos(x) : sin(x); MN1(28, "_Sin", r, x); return r; }
static SYSV float h__FSin(float x, u32 q) { float r = (float)((q & 1) ? cos(x) : sin(x)); MN1(29, "_FSin", r, x); return r; }
static double log_b(double x, int b) { double l = log(x); return b == 1 ? l / 2.302585092994046 : b == 2 ? l / 0.6931471805599453 : l; }
static SYSV double h__Log(double x, int b) { double r = log_b(x, b); MN1(30, "_Log", r, x); return r; }
static SYSV float h__FLog(float x, int b) { float r = (float)log_b(x, b); MN1(31, "_FLog", r, x); return r; }
static SYSV float h__FSinh(float x, float y) { float r = (float)(y * (exp(x) - exp(-x)) * 0.5); MN2(32, "_FSinh", r, x, y); return r; }

/* ---------------- printf family (System V varargs) ---------------- */
typedef struct { u32 gp_offset, fp_offset; u8 *overflow; u8 *reg_save; } SysVaList;
static u64 va_int(SysVaList *v) {
    if (v->gp_offset < 48) { u64 x = *(u64 *)(v->reg_save + v->gp_offset); v->gp_offset += 8; return x; }
    u64 x = *(u64 *)v->overflow; v->overflow += 8; return x;
}
static double va_dbl(SysVaList *v) {
    if (v->fp_offset < 176) { double x = *(double *)(v->reg_save + v->fp_offset); v->fp_offset += 16; return x; }
    double x = *(double *)v->overflow; v->overflow += 8; return x;
}
typedef struct { char *buf; u64 cap, len; } Out;
static void out_c(Out *o, char c) { if (o->len + 1 < o->cap) o->buf[o->len] = c; o->len++; }
static void out_pad(Out *o, char c, int n) { while (n-- > 0) out_c(o, c); }
static int fmt_core(char *buf, u64 cap, const char *f, SysVaList *va) {
    Out o = {buf, cap, 0};
    for (; *f; f++) {
        if (*f != '%') { out_c(&o, *f); continue; }
        f++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = -1, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break;
        }
        if (*f == '*') { width = (int)va_int(va); f++; } else if (*f >= '0' && *f <= '9') { width = 0; while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0'; }
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = (int)va_int(va); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + *f++ - '0'; }
        for (;; f++) { if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'q' || *f == 'L') lng++; else if (*f == 'h') lng = -1; else break; }
        char tmp[80]; int n = 0; const char *s = tmp; char sign = 0;
        switch (*f) {
        case 'd': case 'i': {
            i64 v = lng > 0 ? (i64)va_int(va) : (i64)(i32)va_int(va);
            u64 u = v < 0 ? (u64)(-v) : (u64)v;
            if (v < 0) sign = '-'; else if (plus) sign = '+'; else if (space) sign = ' ';
            do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            goto emit_rev; }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            u64 u = (*f == 'p' || lng > 0) ? va_int(va) : (u64)(u32)va_int(va);
            int base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            const char *dg = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do { tmp[n++] = dg[u % base]; u /= base; } while (u);
            if (*f == 'p' || (alt && base == 16)) { tmp[n++] = *f == 'X' ? 'X' : 'x'; tmp[n++] = '0'; }
            goto emit_rev; }
        emit_rev: {
            int digits = n;
            int pz = prec > digits ? prec - digits : 0;
            int total = digits + pz + (sign ? 1 : 0);
            if (!left && !zero) out_pad(&o, ' ', width - total);
            if (sign) out_c(&o, sign);
            if (!left && zero) out_pad(&o, '0', width - total);
            out_pad(&o, '0', pz);
            while (n) out_c(&o, tmp[--n]);
            if (left) out_pad(&o, ' ', width - total);
            continue; }
        case 'c': tmp[0] = (char)va_int(va); n = 1; break;
        case 's': {
            s = (const char *)va_int(va);
            if (!s) s = "(null)";
            if (lng > 0) {   /* wide string: narrow it */
                const u16 *w = (const u16 *)s; while (w[n] && n < 79) { tmp[n] = (char)w[n]; n++; } s = tmp;
            } else { while (s[n] && (prec < 0 || n < prec)) n++; }
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_dbl(va);
            if (prec < 0) prec = 6;
            if (v != v) { s = "nan"; n = 3; break; }
            if (v < 0) { tmp[n++] = '-'; v = -v; } else if (plus) tmp[n++] = '+';
            if (v > 1e18) { s = "inf/large"; n = 9; break; }
            double r = 0.5; for (int i = 0; i < prec; i++) r /= 10; v += r;
            u64 ip = (u64)v; double fr = v - (double)ip;
            char t2[24]; int k = 0; do { t2[k++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (k) tmp[n++] = t2[--k];
            if (prec > 0) { tmp[n++] = '.'; for (int i = 0; i < prec && n < 78; i++) { fr *= 10; int d = (int)fr; tmp[n++] = (char)('0' + d); fr -= d; } }
            break; }
        case '%': tmp[0] = '%'; n = 1; break;
        case 'n': va_int(va); continue;
        default: tmp[0] = '%'; tmp[1] = *f; n = 2; if (!*f) { f--; n = 1; } break;
        }
        if (!left) out_pad(&o, ' ', width - n);
        for (int i = 0; i < n; i++) out_c(&o, s[i]);
        if (left) out_pad(&o, ' ', width - n);
    }
    if (o.cap) o.buf[o.len < o.cap ? o.len : o.cap - 1] = 0;
    return (int)o.len;
}
static void game_print(const char *s) { tr("[game] "); tr(s); }
static int is_std(void *f) { return f == g_stdout_obj || f == g_stderr_obj; }
SYSV int core_vsnprintf(char *b, u64 n, const char *f, SysVaList *v) { return fmt_core(b, n, f, v); }
SYSV int core_vsprintf(char *b, const char *f, SysVaList *v) { return fmt_core(b, 1ull << 30, f, v); }
SYSV int core_vprintf(const char *f, SysVaList *v) { char b[2048]; int n = fmt_core(b, sizeof b, f, v); game_print(b); return n; }
SYSV int core_vfprintf(void *fp, const char *f, SysVaList *v) {
    char b[2048]; int n = fmt_core(b, sizeof b, f, v);
    if (is_std(fp)) game_print(b);
    return n;
}
/* variadic entry thunks: build a System V va_list from the incoming registers, then call core_* */
#define VTHUNK(name, core, reg) \
    __asm__(".text\n.globl " #name "\n" #name ":\n" \
        "push %rbp\nmov %rsp,%rbp\nsub $0xd0,%rsp\n" \
        "mov %rdi,0x00(%rsp)\nmov %rsi,0x08(%rsp)\nmov %rdx,0x10(%rsp)\nmov %rcx,0x18(%rsp)\nmov %r8,0x20(%rsp)\nmov %r9,0x28(%rsp)\n" \
        "movaps %xmm0,0x30(%rsp)\nmovaps %xmm1,0x40(%rsp)\nmovaps %xmm2,0x50(%rsp)\nmovaps %xmm3,0x60(%rsp)\n" \
        "movaps %xmm4,0x70(%rsp)\nmovaps %xmm5,0x80(%rsp)\nmovaps %xmm6,0x90(%rsp)\nmovaps %xmm7,0xa0(%rsp)\n" \
        "lea 0xb0(%rsp),%rax\n" \
        "movl $" #reg "*8,0x0(%rax)\nmovl $48,0x4(%rax)\nlea 16(%rbp),%r10\nmov %r10,0x8(%rax)\nmov %rsp,%r10\nmov %r10,0x10(%rax)\n" \
        VT_ARG_##reg \
        "call " #core "\nleave\nret\n");
#define VT_ARG_1 "mov %rax,%rsi\n"
#define VT_ARG_2 "mov %rax,%rdx\n"
#define VT_ARG_3 "mov %rax,%rcx\n"
VTHUNK(vt_printf, core_vprintf, 1)
VTHUNK(vt_sprintf, core_vsprintf, 2)
VTHUNK(vt_fprintf, core_vfprintf, 2)
VTHUNK(vt_snprintf, core_vsnprintf, 3)
void vt_printf(void); void vt_sprintf(void); void vt_fprintf(void); void vt_snprintf(void);
static SYSV int h_vsnprintf(char *b, u64 n, const char *f, SysVaList *v) { return fmt_core(b, n, f, v); }
static SYSV int h_vsprintf(char *b, const char *f, SysVaList *v) { return fmt_core(b, 1ull << 30, f, v); }
static SYSV int h_vprintf(const char *f, SysVaList *v) { return core_vprintf(f, v); }
static SYSV int h_vfprintf(void *fp, const char *f, SysVaList *v) { return core_vfprintf(fp, f, v); }
static SYSV int h_puts(const char *s) { game_print(s); tr("\n"); return 0; }
static SYSV int h_putchar(int c) { char b[2] = {(char)c, 0}; game_print(b); return c; }
static SYSV void h_perror(const char *s) { tr("[game perror] "); tr(s ? s : ""); tr("\n"); }
static SYSV int h_swprintf(u16 *b, u64 n, const u16 *f) { (void)f; if (b && n) b[0] = 0; return 0; }
static SYSV int h_sscanf(const char *s, const char *f) { (void)s; (void)f; return 0; }

/* ---------------- stdio files ---------------- */
/* writable mounts (save data, temporary data): "/savedata0" -> a host folder next to the loader */
typedef struct { char vpath[16]; char host[1024]; int used; } WMount;
static WMount g_wm[8];
static int wmount_add(const char *vpath, const char *host) {
    for (int i = 0; i < 8; i++) if (!g_wm[i].used) {
        int k = 0; for (; vpath[k] && k < 15; k++) g_wm[i].vpath[k] = vpath[k]; g_wm[i].vpath[k] = 0;
        k = 0; for (; host[k] && k < 1023; k++) g_wm[i].host[k] = host[k]; g_wm[i].host[k] = 0;
        g_wm[i].used = 1; return i;
    }
    return -1;
}
static void wmount_del(const char *vpath) { for (int i = 0; i < 8; i++) if (g_wm[i].used && str_eq(g_wm[i].vpath, vpath)) g_wm[i].used = 0; }
/* returns 1 for read-only game files, 2 for writable mounts, 0 for unknown paths */
static int map_path(char *out, const char *p) {
    if (str_starts(p, "/app0/")) { path_join(out, g_game_dir, p + 6); return 1; }
    if (str_starts(p, "/app0")) { path_join(out, g_game_dir, p + 5); return 1; }
    for (int i = 0; i < 8; i++) if (g_wm[i].used) {
        int n = str_len(g_wm[i].vpath);
        if (str_starts(p, g_wm[i].vpath) && (p[n] == 0 || p[n] == '/')) { path_join(out, g_wm[i].host, p + n); return 2; }
    }
    return 0;
}
static SYSV void *h_fopen(const char *p, const char *mode) {
    char hp[1100];
    TRACE("fopen", p, mode, 0);
    tr("    fopen \""); tr(p); tr("\" mode "); tr(mode); tr("\n");
    int kind = map_path(hp, p);
    if (!kind) return 0;
    int plus = mode[1] == '+' || (mode[1] && mode[2] == '+');
    if (mode[0] == 'r' && !plus) return hf_open_rb(hp);
    if (kind != 2) return 0;                       /* never write into the game folder */
    if (mode[0] == 'r') return hf_open_rw(hp, 0, 0);
    if (mode[0] == 'w') return hf_open_rw(hp, 1, 1);
    if (mode[0] == 'a') { void *f = hf_open_rw(hp, 1, 0); if (f) hf_seek(f, 0, 2); return f; }
    return 0;
}
static SYSV int h_fclose(void *f) { if (f && !is_std(f)) hf_close(f); return 0; }
static u64 game_read(void *f, void *b, u64 n);
static SYSV u64 h_fread(void *b, u64 s, u64 n, void *f) { if (!f || is_std(f) || !s) return 0; return game_read(f, b, s * n) / s; }
static SYSV u64 h_fwrite(const void *b, u64 s, u64 n, void *f) {
    if (is_std(f)) { char t[1024]; u64 k = s * n < 1023 ? s * n : 1023; h_memcpy(t, b, k); t[k] = 0; game_print(t); return n; }
    if (!f || !s) return 0;
    lz_ensure((u64)b, s * n); return hf_write(f, b, s * n) / s;
}
static SYSV int h_fseek(void *f, i64 o, int w) { return (f && !is_std(f) && hf_seek(f, o, w) >= 0) ? 0 : -1; }
static SYSV i64 h_ftell(void *f) { return (f && !is_std(f)) ? hf_tell(f) : -1; }
static SYSV int h_fflush(void *f) { (void)f; return 0; }
static SYSV int h_fputs(const char *s, void *f) { if (is_std(f)) game_print(s); return 0; }
static SYSV int h_fputc(int c, void *f) { if (is_std(f)) h_putchar(c); return c; }
static SYSV int h_getc(void *f) { u8 c; return (f && !is_std(f) && hf_read(f, &c, 1) == 1) ? c : -1; }
static SYSV char *h_fgets(char *b, int n, void *f) {
    int i = 0, c;
    while (i < n - 1 && (c = h_getc(f)) != -1) { b[i++] = (char)c; if (c == '\n') break; }
    if (!i) return 0;
    b[i] = 0; return b;
}
static SYSV int h_feof(void *f) { (void)f; return 0; }
static SYSV int h_ferror(void *f) { (void)f; return 0; }

/* ---------------- kernel: files ---------------- */
#define MAX_FD 256
static void *g_fd[MAX_FD];
static char g_fd_path[MAX_FD][96]; static u64 g_fd_bytes[MAX_FD]; static u64 g_n_short_reads;   /* STATUS 82: per-fd path (tail) and bytes read; short / failed reads are logged */
static void fd_set_path(int fd, const char *p) { int n = str_len(p); const char *t = n > 95 ? p + n - 95 : p; int k = 0; while (t[k] && k < 95) { g_fd_path[fd][k] = t[k]; k++; } g_fd_path[fd][k] = 0; g_fd_bytes[fd] = 0; }
static int g_opt_trace_vis; static int g_vis_read_log; static u64 g_vis_read_lo, g_vis_read_hi, g_vis_read_lines;   /* STATUS 83: every file read in a flip window goes to the trace (what does the pickup load?) */
static void vis_read_line(const char *what, int fd, i64 r, u64 n, i64 off);
typedef struct { u64 flip; i64 off, r; u64 n; int fd; char path[64]; char what[8]; } VisRead;
static VisRead g_vis_ring[512]; static u32 g_vis_ring_n; static volatile int g_vis_ring_lock;
static void fd_read_note(int fd, i64 r, u64 n, i64 off, const char *what) {
    if (g_vis_read_log && g_plat_frame >= g_vis_read_lo && g_plat_frame < g_vis_read_hi && g_vis_read_lines < 3000) { g_vis_read_lines++; vis_read_line(what, fd, r, n, off); }
    else if (g_opt_trace_vis) {   /* the last 512 reads, written out when the pickup switches its meshes on */
        while (__atomic_exchange_n(&g_vis_ring_lock, 1, __ATOMIC_ACQUIRE)) { }
        VisRead *v = &g_vis_ring[g_vis_ring_n++ & 511]; v->flip = g_plat_frame; v->off = off; v->r = r; v->n = n; v->fd = fd;
        { int k = 0; const char *t = g_fd_path[fd]; int L = str_len(t); if (L > 63) t += L - 63; while (t[k] && k < 63) { v->path[k] = t[k]; k++; } v->path[k] = 0; k = 0; while (what[k] && k < 7) { v->what[k] = what[k]; k++; } v->what[k] = 0; }
        __atomic_store_n(&g_vis_ring_lock, 0, __ATOMIC_RELEASE);
    }
    if (r > 0) g_fd_bytes[fd] += (u64)r;
    if (r == (i64)n) return;
    g_n_short_reads++;
    if (g_n_short_reads <= 200) { pm_lock(&g_log_lock); tr("    "); tr(what); tr(" short/failed: fd "); tr_dec(fd); tr(" "); tr(g_fd_path[fd]); tr(" asked "); tr_dec((i64)n); tr(" at "); tr_dec(off); tr(" got "); tr_dec(r); tr(" (flip "); tr_dec((i64)g_plat_frame); tr(")\n"); pm_unlock(&g_log_lock); }
}
/* Every fd has a lock: sceKernelPread/Pwrite must read at an offset as one step, but a host handle has one file
   position, so seek + read on the same fd from two threads (Fios, Lump Loader and Wwise all read in parallel)
   could read from the other thread's offset. The slot table has its own lock so two opens cannot share a slot. */
static PMutex g_fd_lock[MAX_FD], g_fd_alloc; static int g_fd_locks_ready;
static i64 g_fd_contended, g_fd_preads;
static void fd_locks_init(void) { if (g_fd_locks_ready) return; for (int i = 0; i < MAX_FD; i++) pm_init(&g_fd_lock[i]); pm_init(&g_fd_alloc); g_fd_locks_ready = 1; }
static void fd_lock(int fd) { if (pm_trylock(&g_fd_lock[fd])) { __atomic_add_fetch(&g_fd_contended, 1, __ATOMIC_RELAXED); pm_lock(&g_fd_lock[fd]); } }
static void fd_unlock(int fd) { pm_unlock(&g_fd_lock[fd]); }
static SYSV int h_kopen(const char *p, int flags, int mode) {
    TRACE("sceKernelOpen", p, flags, mode);
    char hp[1100];
    tr("    open \""); tr(p); tr("\" flags "); tr_hex((u64)flags); tr("\n");
    int kind = map_path(hp, p);
    if (!kind) return (int)0x80020002;                              /* ENOENT */
    void *f;
    if (kind == 2) {
        if (plat_is_dir(hp)) return (int)0x80020015;                /* EISDIR: directory handles not supported yet */
        int exists = 0; { void *t = hf_open_rb(hp); if (t) { exists = 1; hf_close(t); } }
        if ((flags & 0x800) && (flags & 0x200) && exists) return (int)0x80020011;   /* O_EXCL: EEXIST */
        if (!exists && !(flags & 0x200)) return (int)0x80020002;
        f = (flags & 3) ? hf_open_rw(hp, (flags & 0x200) != 0, (flags & 0x400) != 0) : hf_open_rb(hp);
        if (f && (flags & 8)) hf_seek(f, 0, 2);                     /* O_APPEND */
    } else {
        if (flags & 3) return (int)0x8002001E;                      /* EROFS: never write into the game folder */
        f = hf_open_rb(hp);
    }
    if (!f) return (int)0x80020002;
    pm_lock(&g_fd_alloc);
    for (int i = 3; i < MAX_FD; i++) if (!g_fd[i]) { g_fd[i] = f; fd_set_path(i, p); pm_unlock(&g_fd_alloc); return i; }
    pm_unlock(&g_fd_alloc);
    hf_close(f); return (int)0x80020018;
}
static SYSV int h_kclose(int fd) {
    if (fd > 2 && fd < MAX_FD && g_fd[fd]) { fd_lock(fd); void *f = g_fd[fd]; if (f) { hf_close(f); pm_lock(&g_fd_alloc); g_fd[fd] = 0; pm_unlock(&g_fd_alloc); } fd_unlock(fd); }
    return 0;
}
/* t107: file reads land in a buffer of ours and are copied into game memory. Reading straight into game memory lets
   Windows probe the destination pages (it reads and writes back the first byte of each page), which can race another
   game thread writing the start of a page - the shape of the page-start damage of STATUS 40 / 69. --direct-reads: as before. */
static int g_opt_direct_reads; static u64 g_n_bounced_reads, g_n_bounced_bytes;
enum { RB_N = 8, RB_SIZE = 1 << 20 };
static u8 *g_rb_buf[RB_N]; static int g_rb_busy[RB_N];
static void rb_copy(u8 *d, const u8 *s, u64 n) {   /* 8 bytes at a time (whole streamed lumps go through here) */
    while (n >= 8) { u64 v; __builtin_memcpy(&v, s, 8); __builtin_memcpy(d, &v, 8); d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
}
static u64 game_read_(void *f, void *b, u64 n);
static volatile u64 g_io_n, g_io_bytes, g_io_ticks, g_io_max;   /* t144: the game's file reads (count, bytes, ticks, the longest) - for the long-frame lines */
static u64 game_read(void *f, void *b, u64 n) {
    u64 t0 = plat_ticks(); u64 r = game_read_(f, b, n); u64 dt = plat_ticks() - t0;
    __atomic_add_fetch(&g_io_n, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_io_bytes, r, __ATOMIC_RELAXED); __atomic_add_fetch(&g_io_ticks, dt, __ATOMIC_RELAXED);
    u64 m = g_io_max; while (dt > m && !__atomic_compare_exchange_n(&g_io_max, &m, dt, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
    return r;
}
static u64 game_read_(void *f, void *b, u64 n) {
    if (g_opt_direct_reads || !n) { lz_ensure((u64)b, n); return hf_read(f, b, n); }
    int slot = -1; u8 *buf = 0, own = 0;
    for (int i = 0; i < RB_N; i++) if (!__atomic_exchange_n(&g_rb_busy[i], 1, __ATOMIC_ACQUIRE)) { slot = i; break; }
    if (slot >= 0) { if (!g_rb_buf[slot]) g_rb_buf[slot] = (u8 *)malloc(RB_SIZE); buf = g_rb_buf[slot]; }
    if (!buf) { buf = (u8 *)malloc(RB_SIZE); own = 1; }
    if (!buf) { if (slot >= 0) __atomic_store_n(&g_rb_busy[slot], 0, __ATOMIC_RELEASE); lz_ensure((u64)b, n); return hf_read(f, b, n); }
    u64 tot = 0;
    lz_ensure((u64)b, n);   /* t111: the destination as one commit (one imported region), not granule by granule */
    while (tot < n) { u64 c = n - tot > RB_SIZE ? RB_SIZE : n - tot, got = hf_read(f, buf, c); if (got) rb_copy((u8 *)b + tot, buf, got); tot += got; if (got < c) break; }
    if (own) free(buf); else __atomic_store_n(&g_rb_busy[slot], 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_n_bounced_reads, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_n_bounced_bytes, tot, __ATOMIC_RELAXED);
    return tot;
}
static SYSV i64 h_kread(int fd, void *b, u64 n) {
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd]) return (i64)0x80020009;
    fd_lock(fd); i64 r = g_fd[fd] ? (i64)game_read(g_fd[fd], b, n) : (i64)0x80020009; fd_unlock(fd); fd_read_note(fd, r, n, -1, "read"); if (r > 0) hw_note((u64)b, (u64)r, 2); return r;
}
static SYSV i64 h_klseek(int fd, i64 o, int w) {
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd]) return (i64)0x80020009;
    fd_lock(fd); i64 r = g_fd[fd] ? hf_seek(g_fd[fd], o, w) : (i64)0x80020009; fd_unlock(fd); return r;
}
/* pread: read at off without moving the fd's own position (saved and restored under the fd lock) */
static SYSV i64 h_kpread(int fd, void *b, u64 n, i64 off) {
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd]) return (i64)0x80020009;
    __atomic_add_fetch(&g_fd_preads, 1, __ATOMIC_RELAXED);
    fd_lock(fd);
    if (!g_fd[fd]) { fd_unlock(fd); return (i64)0x80020009; }
    i64 cur = hf_seek(g_fd[fd], 0, 1);
    hf_seek(g_fd[fd], off, 0); i64 r = (i64)game_read(g_fd[fd], b, n);
    if (cur >= 0) hf_seek(g_fd[fd], cur, 0);
    fd_unlock(fd); fd_read_note(fd, r, n, off, "pread"); if (r > 0) hw_note((u64)b, (u64)r, 2); return r;
}
static SYSV i64 h_kwrite(int fd, const void *b, u64 n) {
    if (fd == 1 || fd == 2) { char t[1024]; u64 k = n < 1023 ? n : 1023; h_memcpy(t, b, k); t[k] = 0; game_print(t); return (i64)n; }
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd]) return (i64)0x80020009;
    lz_ensure((u64)b, n); fd_lock(fd); i64 r = g_fd[fd] ? (i64)hf_write(g_fd[fd], b, n) : (i64)0x80020009; fd_unlock(fd); return r;
}
static SYSV i64 h_kpwrite(int fd, const void *b, u64 n, i64 off) {
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd]) return (i64)0x80020009;
    fd_lock(fd);
    if (!g_fd[fd]) { fd_unlock(fd); return (i64)0x80020009; }
    i64 cur = hf_seek(g_fd[fd], 0, 1);
    lz_ensure((u64)b, n); hf_seek(g_fd[fd], off, 0); i64 r = (i64)hf_write(g_fd[fd], b, n);
    if (cur >= 0) hf_seek(g_fd[fd], cur, 0);
    fd_unlock(fd); return r;
}
static SYSV int h_kmkdir(const char *p, int mode) {
    TRACE("sceKernelMkdir", p, mode, 0);
    char hp[1100]; int kind = map_path(hp, p);
    tr("    mkdir \""); tr(p); tr("\"\n");
    if (!kind) return 0;                                  /* unknown mount: pretend */
    if (plat_is_dir(hp)) return (int)0x80020011;         /* EEXIST */
    if (kind == 1) return (int)0x8002001E;               /* EROFS */
    plat_mkdir(hp); return plat_is_dir(hp) ? 0 : (int)0x80020002;
}
static SYSV int h_kunlink(const char *p) {
    TRACE("sceKernelUnlink", p, 0, 0);
    char hp[1100]; int kind = map_path(hp, p);
    if (kind != 2) return (int)0x80020002;
    return plat_unlink(hp) ? (int)0x80020002 : 0;
}
static SYSV int h_kfsync(int fd) { (void)fd; return 0; }
/* rename / rmdir / truncate inside the writable mounts (save data, temporary data); the game folder stays read-only.
   Not reached in Chapter 1; the game's file-system layer (eboot+0x48b8d..0x48cf0, 0x98a658..0x98b405) has them. */
static SYSV int h_krename(const char *from, const char *to) {
    TRACE("sceKernelRename", from, to, 0);
    char ha[1100], hb[1100]; int ka = map_path(ha, from), kb = map_path(hb, to);
    tr("    rename \""); tr(from ? from : ""); tr("\" -> \""); tr(to ? to : ""); tr("\"\n");
    if (!ka || !kb) return (int)0x80020002;                       /* ENOENT */
    if (ka != 2 || kb != 2) return (int)0x8002001E;               /* EROFS */
    if (!plat_is_dir(ha) && plat_file_size(ha) < 0) return (int)0x80020002;
    return plat_replace_file(ha, hb) ? 0 : (int)0x80020005;       /* EIO */
}
static SYSV int h_krmdir(const char *p) {
    TRACE("sceKernelRmdir", p, 0, 0);
    char hp[1100]; int kind = map_path(hp, p);
    if (!kind || !plat_is_dir(hp)) return (int)0x80020002;
    if (kind != 2) return (int)0x8002001E;
    return plat_rmdir(hp) ? (int)0x80020042 /* ENOTEMPTY */ : 0;
}
static SYSV int h_kftruncate(int fd, i64 len) {
    TRACE("sceKernelFtruncate", fd, len, 0);
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd] || len < 0) return (int)0x80020009;
    fd_lock(fd); int r = g_fd[fd] ? hf_truncate(g_fd[fd], len) : -1; fd_unlock(fd);
    return r ? (int)0x80020009 : 0;   /* a file opened read-only cannot be changed: EBADF, as on FreeBSD */
}
static SYSV int h_ktruncate(const char *p, i64 len) {
    TRACE("sceKernelTruncate", p, len, 0);
    char hp[1100]; int kind = map_path(hp, p);
    if (!kind || plat_file_size(hp) < 0) return (int)0x80020002;
    if (kind != 2) return (int)0x8002001E;
    void *f = hf_open_rw(hp, 0, 0); if (!f) return (int)0x80020002;
    int r = hf_truncate(f, len); hf_close(f); return r ? (int)0x80020005 : 0;
}
/* permissions and file times are not modelled: succeed for paths that exist */
static SYSV int h_kchmod(const char *p, int mode) {
    TRACE("sceKernelChmod", p, mode, 0);
    char hp[1100]; if (!map_path(hp, p) || (!plat_is_dir(hp) && plat_file_size(hp) < 0)) return (int)0x80020002;
    return 0;
}
static SYSV int h_kutimes(const char *p, const void *tv) {
    TRACE("sceKernelUtimes", p, tv, 0);
    char hp[1100]; if (!map_path(hp, p) || (!plat_is_dir(hp) && plat_file_size(hp) < 0)) return (int)0x80020002;
    return 0;
}
/* POSIX rename: 0, or -1 with errno */
static SYSV int h_rename(const char *from, const char *to) {
    int r = h_krename(from, to);
    if (r) { *thread_errno() = r & 0xFF; return -1; }
    return 0;
}
/* sceKernelUuidCreate: a random (version 4) UUID */
static SYSV int h_kuuid(u8 *u) {
    TRACE("sceKernelUuidCreate", u, 0, 0);
    if (!u) return (int)0x80020016;
    static u64 x; if (!x) x = plat_wall_ns() ^ plat_ticks() ^ 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 16; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; u[i] = (u8)(x >> 32); }
    u[6] = (u8)((u[6] & 0x0F) | 0x40); u[8] = (u8)((u[8] & 0x3F) | 0x80);
    return 0;
}
static void fill_stat(u8 *st, u64 size, int dir) {
    h_memset(st, 0, 120);
    *(u16 *)(st + 8) = dir ? 0x41ED : 0x81A4;
    *(i64 *)(st + 0x48) = (i64)size;
    *(i64 *)(st + 0x50) = (i64)((size + 511) / 512);
    *(u32 *)(st + 0x58) = 0x10000;
}
static SYSV int h_kstat(const char *p, u8 *st) {
    TRACE("sceKernelStat", p, st, 0);
    char hp[1100];
    tr("    stat \""); tr(p); tr("\"\n");
    if (!map_path(hp, p)) return (int)0x80020002;
    if (plat_is_dir(hp)) { fill_stat(st, 0, 1); return 0; }
    void *f = hf_open_rb(hp);
    if (!f) return (int)0x80020002;
    u64 sz = (u64)hf_seek(f, 0, 2); hf_close(f);
    fill_stat(st, sz, 0);
    return 0;
}
static SYSV int h_kfstat(int fd, u8 *st) {
    if (fd < 3 || fd >= MAX_FD || !g_fd[fd]) return (int)0x80020009;
    fd_lock(fd); i64 cur = hf_tell(g_fd[fd]); u64 sz = (u64)hf_seek(g_fd[fd], 0, 2); hf_seek(g_fd[fd], cur, 0); fd_unlock(fd);
    fill_stat(st, sz, 0); return 0;
}

/* ---------------- kernel: memory ---------------- */
static u64 g_dmem_next = 0x100000;
/* journal of the last 512 memory-mapping calls (any thread), written into the crash report: the game's lump loader
   allocates, maps, queries and releases memory constantly, and a wrong answer here shows up much later as a crash */
typedef struct { u8 kind; const char *tn; u64 caller, a, b, c, d; i64 r; u64 o1, o2, o3; } MjEnt;
enum { MJ_N = 512, MJ_ALLOC = 1, MJ_MAPD, MJ_REL, MJ_VQ, MJ_MAPF };
static MjEnt g_mj[MJ_N]; static u64 g_mj_n;
static const char *cur_thread_name(void);
static void mj_add(int kind, u64 caller, u64 a, u64 b, u64 c, u64 d, i64 r, u64 o1, u64 o2, u64 o3) {
    MjEnt *e = &g_mj[__atomic_fetch_add(&g_mj_n, 1, __ATOMIC_RELAXED) % MJ_N];
    e->kind = (u8)kind; e->tn = cur_thread_name(); e->caller = caller; e->a = a; e->b = b; e->c = c; e->d = d; e->r = r; e->o1 = o1; e->o2 = o2; e->o3 = o3;
}
static SYSV int h_alloc_direct(u64 start, u64 end, u64 len, u64 align, int type, i64 *phys) {
    TRACE("sceKernelAllocateDirectMemory", len, align, type);
    if (!align) align = 0x10000;
    g_dmem_next = (g_dmem_next + align - 1) & ~(align - 1);
    *phys = (i64)g_dmem_next; g_dmem_next += len;
    mj_add(MJ_ALLOC, (u64)__builtin_return_address(0), start, end, len, align, 0, (u64)*phys, (u64)type, 0);
    tr("    direct memory: len "); tr_hex(len); tr(" align "); tr_hex(align); tr(" type "); tr_dec(type); tr("\n");
    return 0;
}
/* game-visible memory ranges (GPU addresses == CPU addresses on PS4); used by the fake GPU to validate writes */
typedef struct { u64 lo, hi; } MemRange;
static MemRange g_mem[4096]; static int g_nmem;
/* t113: names of our own large allocations (by any address inside them), for the memory report's list of the largest
   private allocations; whatever stays unnamed there is the graphics driver's or Windows' (the heaps are named at exit) */
typedef struct { const void *p; const char *name; } MemName;
enum { MEM_NAMES = 48 };
static MemName g_mnames[MEM_NAMES]; static volatile int g_nmnames;
static void mem_name(const void *p, const char *name) {
    if (!p) return;
    int n = __atomic_load_n(&g_nmnames, __ATOMIC_ACQUIRE); if (n > MEM_NAMES) n = MEM_NAMES;
    for (int i = 0; i < n; i++) if (g_mnames[i].p == p) { g_mnames[i].name = name; return; }
    n = __atomic_fetch_add(&g_nmnames, 1, __ATOMIC_ACQ_REL); if (n >= MEM_NAMES) return;
    g_mnames[n].name = 0; g_mnames[n].p = p; __atomic_store_n(&g_mnames[n].name, name, __ATOMIC_RELEASE);
}
static void (*g_mem_new_hook)(u64 lo, u64 len);   /* t107: the renderer imports new game memory before the game uses it (vk_preimport) */
/* t109: a lock that needs no start-up call (the first mapping can come from any thread) */
static void pm_once(PMutex *m, volatile int *st) {
    if (__atomic_load_n(st, __ATOMIC_ACQUIRE) == 2) return;
    int z = 0;
    if (__atomic_compare_exchange_n(st, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { pm_init(m); __atomic_store_n(st, 2, __ATOMIC_RELEASE); return; }
    while (__atomic_load_n(st, __ATOMIC_ACQUIRE) != 2) {}
}
/* t109: mem_ok looks the address up in a sorted list of the ranges with touching ranges joined (the pieces of an arena
   are one stretch of memory for the CPU). The list is rewritten under g_memreg_m whenever memory is registered;
   readers check a sequence number (odd while it is being rewritten) and look again if it changed. Before, mem_ok
   searched g_mem in order, and a range over two adjacent pieces was refused. */
static MemRange g_mems[4096], g_memiv[4096]; static int g_nmems; static volatile u32 g_memiv_n, g_memiv_seq;
static PMutex g_memreg_m; static volatile int g_memreg_st;
static void memiv_add(u64 lo, u64 hi) {   /* (under g_memreg_m) */
    if (g_nmems >= 4096) return;
    int at = g_nmems; while (at > 0 && g_mems[at - 1].lo > lo) { g_mems[at] = g_mems[at - 1]; at--; }
    g_mems[at].lo = lo; g_mems[at].hi = hi; g_nmems++;
    u32 s = g_memiv_seq; __atomic_store_n(&g_memiv_seq, s + 1, __ATOMIC_RELEASE); __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int m = 0;
    for (int i = 0; i < g_nmems; i++) {
        if (m && g_mems[i].lo <= g_memiv[m - 1].hi) { if (g_mems[i].hi > g_memiv[m - 1].hi) g_memiv[m - 1].hi = g_mems[i].hi; }
        else g_memiv[m++] = g_mems[i];
    }
    __atomic_store_n(&g_memiv_n, (u32)m, __ATOMIC_RELAXED);
    __atomic_store_n(&g_memiv_seq, s + 2, __ATOMIC_RELEASE);
}
static void mem_register(void *p, u64 len) {
    if (!p || !len) return;
    pm_once(&g_memreg_m, &g_memreg_st); pm_lock(&g_memreg_m);
    if (g_nmem < 4096) {
        if (g_mem_new_hook) g_mem_new_hook((u64)p, len);
        g_mem[g_nmem].lo = (u64)p; g_mem[g_nmem].hi = (u64)p + len;
        __atomic_store_n(&g_nmem, g_nmem + 1, __ATOMIC_RELEASE);
        memiv_add((u64)p, (u64)p + len);
    }
    pm_unlock(&g_memreg_m);
}
/* t111: register a range that is already imported (lazily committed granules: lz_commit_g imports them itself) */
static void mem_register_noimport(void *p, u64 len) {
    if (!p || !len) return;
    pm_once(&g_memreg_m, &g_memreg_st); pm_lock(&g_memreg_m);
    if (g_nmem < 4096) {
        g_mem[g_nmem].lo = (u64)p; g_mem[g_nmem].hi = (u64)p + len;
        __atomic_store_n(&g_nmem, g_nmem + 1, __ATOMIC_RELEASE);
        memiv_add((u64)p, (u64)p + len);
    } else { static int said; if (!said++) say("    memory: the table of game memory ranges is full (4096); the GPU will not see newer ones\n"); }
    pm_unlock(&g_memreg_m);
}
static int mem_ok(u64 a, u64 n) {
    u64 e = a + n; if (e < a) return 0;
    for (;;) {
        u32 s = __atomic_load_n(&g_memiv_seq, __ATOMIC_ACQUIRE);
        if (s & 1) continue;
        int lo = 0, hi = (int)__atomic_load_n(&g_memiv_n, __ATOMIC_RELAXED) - 1, best = -1;
        if (hi > 4095) hi = 4095;
        while (lo <= hi) { int mid = (lo + hi) >> 1; if (g_memiv[mid].lo <= a) { best = mid; lo = mid + 1; } else hi = mid - 1; }
        int ok = best >= 0 && e <= g_memiv[best].hi;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_memiv_seq, __ATOMIC_RELAXED) == s) return ok;
    }
}
/* bytes of [lo, hi) that lie in game memory (the memory report's walk of the address space) */
static u64 mem_in_game(u64 lo, u64 hi) {
    u64 t = 0; int n = (int)__atomic_load_n(&g_memiv_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) { u64 a = g_memiv[i].lo > lo ? g_memiv[i].lo : lo, b = g_memiv[i].hi < hi ? g_memiv[i].hi : hi; if (b > a) t += b - a; }
    return t;
}
/* Small mappings at fixed addresses (the game grows some heaps 256 KB at a time) are carved out of 1 GB arenas,
   so the GPU layer sees one contiguous region instead of hundreds of tiny ones. */
/* t109: an arena is only reserved address space. Its memory is committed (and so imported and pinned by the GPU
   driver) in runs of 4 MB granules as the game maps into it: t108 measured 58 arenas of 64 MB and more (4.3 GB
   pinned) holding about 700 MB of the game's 256 KB lump-heap mappings. A run is extended ahead by a quarter of what
   the arena has committed (4 MB at least, 16 MB at most), so a heap growing 256 KB at a time becomes a few regions and
   at most one run is unused. (t109 extended by all of it, up to 64 MB: 1,012 MB committed for 698 MB mapped.) */
enum { AR_GRAN_SH = 22 };
typedef struct { u64 lo, hi, cbits[4], committed, grow; } Arena;   /* cbits: committed 4 MB granules (an arena is at most 1 GB) */
enum { ARENA_MAX = 1024 };   /* t130: was 128 - t125 / t126 filled it in 35 minutes (one arena per heap slot); mappings after that were separate 256 KB regions (3,700 of the renderer's 4,000) */
static Arena g_arena[ARENA_MAX]; static int g_narena;
static PMutex g_arena_m; static volatile int g_arena_st;
static u64 g_arena_reserved, g_arena_committed, g_arena_runs, g_arena_commit_fail;
static int ar_bit(const Arena *A, u32 g) { return (int)((A->cbits[g >> 6] >> (g & 63)) & 1); }
static int arena_commit(Arena *A, u64 a, u64 len) {   /* (under g_arena_m) commit the granules of [a, a+len) not committed yet */
    const u64 G = 1ull << AR_GRAN_SH;
    u32 ng = (u32)((A->hi - A->lo + G - 1) >> AR_GRAN_SH);
    u32 g0 = (u32)((a - A->lo) >> AR_GRAN_SH), g1 = (u32)((a + (len ? len : 1) - 1 - A->lo) >> AR_GRAN_SH);
    for (u32 g = g0; g <= g1 && g < ng; g++) {
        if (ar_bit(A, g)) continue;
        u32 e = g; while (e + 1 <= g1 && !ar_bit(A, e + 1)) e++;
        if (e == g1) {   /* the run reaches the end of the request: grow it ahead */
            u64 want = (A->committed > A->grow ? A->committed : A->grow) / 4; if (want < G) want = G; if (want > (16ull << 20)) want = 16ull << 20;   /* t110: a quarter (was all), at most 16 MB (was 64) */
            u32 wg = (u32)(want >> AR_GRAN_SH);
            while (e - g + 1 < wg && e + 1 < ng && !ar_bit(A, e + 1)) e++;
        }
        u64 lo = A->lo + ((u64)g << AR_GRAN_SH), hi = A->lo + ((u64)(e + 1) << AR_GRAN_SH); if (hi > A->hi) hi = A->hi;
        if (!plat_commit((void *)lo, hi - lo)) { g_arena_commit_fail++; tr("    arena: commit failed at "); tr_hex(lo); tr("\n"); return 0; }
        for (u32 k = g; k <= e; k++) A->cbits[k >> 6] |= 1ull << (k & 63);
        A->committed += hi - lo; g_arena_committed += hi - lo; g_arena_runs++;
        mem_register((void *)lo, hi - lo);
        g = e;
    }
    return 1;
}
/* Arenas start at 64 MB; a heap that grows past the end of its arena gets an adjacent one twice as large (up to 1 GB).
   (1 GB per heap pinned ~32 GB of memory in the GPU driver for ~700 MB of actual mappings.) t109: reserved, committed
   as used (arena_commit); zero = the mapping must read as zero (flexible memory) where it was committed before. */
static void *arena_take(u64 a, u64 len, int zero) {
    void *r = 0;
    pm_once(&g_arena_m, &g_arena_st); pm_lock(&g_arena_m);
    {   /* inside existing arenas (also across adjacent ones): commit what is missing */
        u64 p = a, e = a + (len ? len : 1); int cov = 1;
        while (p < e) { int k = -1; for (int i = 0; i < g_narena; i++) if (p >= g_arena[i].lo && p < g_arena[i].hi) { k = i; break; } if (k < 0) { cov = 0; break; } p = g_arena[k].hi; }
        if (cov) {
            int ok = 1; p = a;
            while (ok && p < e) { int k = -1; for (int i = 0; i < g_narena; i++) if (p >= g_arena[i].lo && p < g_arena[i].hi) { k = i; break; }
                u64 q = g_arena[k].hi < e ? g_arena[k].hi : e; ok = arena_commit(&g_arena[k], p, q - p); p = q; }
            if (ok) { r = (void *)a; if (zero && len) { h_memset(r, 0, len); hw_note(a, len, 3); } }
            pm_unlock(&g_arena_m); return r;
        }
    }
    if (len > (64ull << 20) || g_narena >= ARENA_MAX) { pm_unlock(&g_arena_m); return 0; }
    u64 sz = 64ull << 20, grow = 0;
    for (int i = 0; i < g_narena; i++) if (g_arena[i].hi == a) { u64 prev = g_arena[i].hi - g_arena[i].lo; sz = prev * 2 > (1ull << 30) ? (1ull << 30) : prev * 2; if (sz < (64ull << 20)) sz = 64ull << 20; grow = g_arena[i].committed; }
    for (int i = 0; i < g_narena; i++) if (g_arena[i].lo > a && g_arena[i].lo - a < sz) sz = g_arena[i].lo - a;   /* stop at the next arena */
    for (int i = 0; i < g_narena; i++) if (a < g_arena[i].hi && a + len > g_arena[i].lo) { pm_unlock(&g_arena_m); return 0; }   /* straddles an arena */
    u64 minsz = len < (2ull << 20) ? (2ull << 20) : len;
    for (; sz >= minsz; sz /= 2) {
        u64 s = sz & ~0xFFFFull; if (s < len) break;
        void *p = plat_reserve_at((void *)a, s);
        if (!p) continue;
        Arena *A = &g_arena[g_narena]; h_memset(A, 0, sizeof *A); A->lo = a; A->hi = a + s; A->grow = grow;
        g_narena++; g_arena_reserved += s;
        tr("    arena: reserved "); tr_hex(s); tr(" bytes at "); tr_hex(a); tr(" for small fixed-address mappings (committed as used)\n");
        if (arena_commit(A, a, len)) r = p;
        break;
    }
    pm_unlock(&g_arena_m);
    return r;   /* 0: the caller maps exactly the requested range */
}
/* ---- mapping pools: committed chunks at 32..60 GB (inside the renderer page table), best fit with a merging free list, 64 KB granularity.
   t109: 64 MB chunks (were 512 MB: t108 used 56 MB of one, all of it pinned by the GPU import); a free range never
   spans two chunks, so every pool mapping lies in one imported region ---- */
enum { POOL_CHUNK = 64u << 20, POOL_MAX_MAP = 64u << 20, POOL_FREE_MAX = 65536 };
/* 32..60 GB: the game itself asks for fixed addresses at 64 GB (0x10_0000_0000) and up, e.g. 0x2f..0x31_8000_0000 -
   a pool at 0x30_0000_0000 took an address the game wanted (its object at 0x3000000020 then read pool data: crash) */
#define POOL_LO 0x800000000ull
#define POOL_HI 0xF00000000ull
typedef struct { u64 lo, len; } PoolFree;
static PoolFree g_pfree[POOL_FREE_MAX]; static int g_npfree; static u64 g_pool_next = POOL_LO, g_pool_bytes, g_pool_used, g_pool_released;
static PMutex g_pool_lock; static int g_pool_lock_init;
static u64 pool_chunk_of(u64 a) { return (a - POOL_LO) / POOL_CHUNK; }
static void pool_free_add(u64 lo, u64 len) {
    for (int i = 0; i < g_npfree; i++) {   /* merge with a neighbour in the same chunk */
        if (g_pfree[i].lo + g_pfree[i].len == lo && pool_chunk_of(g_pfree[i].lo) == pool_chunk_of(lo)) { g_pfree[i].len += len;
            for (int j = 0; j < g_npfree; j++) if (j != i && g_pfree[j].lo == g_pfree[i].lo + g_pfree[i].len && pool_chunk_of(g_pfree[j].lo) == pool_chunk_of(g_pfree[i].lo)) { g_pfree[i].len += g_pfree[j].len; g_pfree[j] = g_pfree[--g_npfree]; break; }
            return; }
        if (lo + len == g_pfree[i].lo && pool_chunk_of(lo) == pool_chunk_of(g_pfree[i].lo)) { g_pfree[i].lo = lo; g_pfree[i].len += len; return; }
    }
    if (g_npfree < POOL_FREE_MAX) { g_pfree[g_npfree].lo = lo; g_pfree[g_npfree].len = len; g_npfree++; }
}
static void *pool_alloc(u64 len) {
    if (!g_pool_lock_init) { pm_init(&g_pool_lock); g_pool_lock_init = 1; }
    len = (len + 0xFFFF) & ~0xFFFFull;
    pm_lock(&g_pool_lock);
    void *r = 0;
    for (int pass = 0; pass < 2 && !r; pass++) {
        int bi = -1;
        for (int i = 0; i < g_npfree; i++) if (g_pfree[i].len >= len && (bi < 0 || g_pfree[i].len < g_pfree[bi].len)) bi = i;   /* best fit */
        if (bi >= 0) { r = (void *)g_pfree[bi].lo; g_pfree[bi].lo += len; g_pfree[bi].len -= len; if (!g_pfree[bi].len) g_pfree[bi] = g_pfree[--g_npfree]; break; }
        if (pass) break;
        /* a new chunk */
        for (int tries = 0; tries < 16 && g_pool_next + POOL_CHUNK <= POOL_HI; tries++) {
            void *c = plat_alloc_at((void *)g_pool_next, POOL_CHUNK);
            u64 at = g_pool_next; g_pool_next += POOL_CHUNK;
            if (!c) continue;
            mem_register(c, POOL_CHUNK); g_pool_bytes += POOL_CHUNK;
            tr("    mapping pool: new 512 MB chunk at "); tr_hex(at); tr("\n");
            pool_free_add(at, POOL_CHUNK);
            break;
        }
    }
    if (r) { g_pool_used += len; h_memset(r, 0, len); hw_note((u64)r, len, 3); }   /* fresh mappings read as zero, as on the PS4 */
    pm_unlock(&g_pool_lock);
    return r;
}
static int pool_owns(u64 a) { return a >= POOL_LO && a < g_pool_next; }
static void pool_release(u64 a, u64 len) {
    len = (len + 0xFFFF) & ~0xFFFFull;
    pm_lock(&g_pool_lock); pool_free_add(a, len); g_pool_used -= len; g_pool_released += len; pm_unlock(&g_pool_lock);
}
/* ---- t111: the game's large mappings, committed on first use ----
   The game maps nearly all of its 5 GB of direct memory at start (a 3 GB streaming pool, GPU memory for render targets,
   heaps). Committed at once, the GPU import locks all of it in RAM; the t110 captures show 1.7-2.2 GB of it never
   written (the 485 MB GPU region: 96% zero, render targets live in Vulkan images here). Now these mappings (more than
   64 MB) are reserved, and each granule (aligned to its size; 64 MB in mappings of 1 GB and more, 16 MB in smaller
   ones: few, large imported regions, as few buffers as possible over a region end) is committed and registered (so
   imported) when it is first needed:
   - the first touch by any thread faults; lz_fault (called by the exception handler) commits the granule and the
     access is repeated;
   - the renderer calls lz_ensure for memory the GPU writes, or uses as a buffer, before recording that work, and it
     points the page table of memory not committed yet at a zeroed trap page per mapping (reads give zero, and a
     store that lands there makes the whole mapping committed: vk_lz_traps);
   - our own file reads / writes straight into game memory call lz_ensure first (Windows does not fault there).
   The granule is imported right after its commit, by the thread that needs it; threads that touch it meanwhile are
   not held back (other threads first touching the same granule during those milliseconds could in principle meet the
   import's page probing, STATUS 40 / 69: the window t107 closed for new mappings). --lazy-ro-import (experiment):
   commit it read-only, import, then make it writable, so writers in between wait; not the default because a driver
   may give the GPU read-only access to memory imported read-only.
   t112: OFF by default (--lazy-memory turns it on; Windows only). In play t111 cut little (4.3 of 5.0 GB were
   committed by the end) and the lazily committed runs split the game's mappings into many imported regions: the
   renderer's single-region fast paths (GPU copies / fills, vk_locate) failed for work over a region end and fell back
   to compute copies, VRAM copies of GPU-written buffers and full GPU waits (STATUS 116). Not to be re-enabled before
   those paths split their work per region and replays show no difference. */
enum { LZ_MAX = 64 };
typedef struct { u64 lo, hi; u8 *cm; u32 ng, sh; int trapped; } LzReg;   /* cm[g]: granule g committed; granule g = [((lo >> sh) + g) << sh, + 1 << sh) clipped to [lo, hi) */
static LzReg g_lz[LZ_MAX]; static volatile int g_nlz;
static int g_opt_lazy_mem = 0;   /* --lazy-memory (t112: off by default; Linux builds: no fault handling for it) */
static PMutex g_lz_m; static volatile int g_lz_st; static volatile int g_lz_in_crash;
static u64 g_lz_reserved, g_lz_committed, g_lz_runs, g_lz_faults, g_lz_ensured, g_lz_fail, g_lz_ro_ok, g_lz_ro_refused, g_lz_import_fail, g_lz_trap_hits;
static int (*g_mem_import_hook)(u64 lo, u64 len);   /* t111: import now, 1 = done (the renderer's vk_preimport) */
static int g_opt_lazy_ro;   /* --lazy-ro-import */
static int lz_ok(u64 len) { return g_opt_lazy_mem && len > (64ull << 20); }
static void lz_add(void *p, u64 len) {   /* a reserved large mapping: game memory for mem_ok now, committed piece by piece */
    pm_once(&g_lz_m, &g_lz_st); pm_lock(&g_lz_m);
    int n = g_nlz; LzReg *L = &g_lz[n < LZ_MAX ? n : 0];
    u32 sh = len >= (1ull << 30) ? 26 : 24;
    u32 ng = (u32)((((u64)p + len - 1) >> sh) - ((u64)p >> sh) + 1);
    u8 *cm = n < LZ_MAX ? (u8 *)calloc(ng, 1) : 0;
    if (!cm) {   /* table full or no memory: committed at once, as before t111 */
        pm_unlock(&g_lz_m);
        if (!plat_commit(p, len)) { say("    memory: could not commit a large mapping\n"); }
        mem_register(p, len); return;
    }
    L->lo = (u64)p; L->hi = (u64)p + len; L->sh = sh; L->ng = ng; L->cm = cm; L->trapped = 0;
    __atomic_store_n(&g_nlz, n + 1, __ATOMIC_RELEASE); g_lz_reserved += len;
    pm_unlock(&g_lz_m);
    pm_once(&g_memreg_m, &g_memreg_st); pm_lock(&g_memreg_m); memiv_add(L->lo, L->hi); pm_unlock(&g_memreg_m);
}
static int lz_find(u64 a) { int n = __atomic_load_n(&g_nlz, __ATOMIC_ACQUIRE); for (int i = 0; i < n; i++) if (a >= g_lz[i].lo && a < g_lz[i].hi) return i; return -1; }
static int lz_is_lazy(u64 a) { return lz_find(a) >= 0; }
static u32 lz_g(const LzReg *L, u64 a) { return (u32)((a >> L->sh) - (L->lo >> L->sh)); }
static u64 lz_glo(const LzReg *L, u32 g) { u64 x = ((L->lo >> L->sh) + g) << L->sh; return x < L->lo ? L->lo : x; }
static u64 lz_ghi(const LzReg *L, u32 g) { u64 x = ((L->lo >> L->sh) + g + 1) << L->sh; return x > L->hi ? L->hi : x; }
/* (under g_lz_m) commit the granules of L in [g0, g1] not committed yet, consecutive ones as one region; returns how many */
static u32 lz_commit_g(LzReg *L, u32 g0, u32 g1) {
    u32 done = 0;
    for (u32 g = g0; g <= g1 && g < L->ng; g++) {
        if (L->cm[g]) continue;
        u32 e = g; while (e + 1 <= g1 && e + 1 < L->ng && !L->cm[e + 1]) e++;
        u64 lo = lz_glo(L, g), hi = lz_ghi(L, e);
        int ro = g_opt_lazy_ro && g_mem_import_hook && plat_commit_ro((void *)lo, hi - lo);   /* (--lazy-ro-import: read-only while it is imported) */
        if (!ro && !plat_commit((void *)lo, hi - lo)) { g_lz_fail++; return done; }
        if (g_mem_import_hook) {
            int ok = g_mem_import_hook(lo, hi - lo);
            if (ro) { plat_protect_rwx((void *)lo, hi - lo); if (ok) g_lz_ro_ok++; else { g_lz_ro_refused++; ok = g_mem_import_hook(lo, hi - lo); } }
            if (!ok) g_lz_import_fail++;   /* (the renderer imports it at its next look, as before t107) */
        }
        mem_register_noimport((void *)lo, hi - lo);
        for (u32 k = g; k <= e; k++) __atomic_store_n(&L->cm[k], 1, __ATOMIC_RELEASE);
        g_lz_committed += hi - lo; g_lz_runs++; done += e - g + 1;
        g = e;
    }
    return done;
}
/* the exception handler: a read or write of an address that is reserved here and not committed yet (or committed
   read-only while another thread imports it) */
static int lz_fault(u64 a) {
    if (g_lz_in_crash) return 0;   /* (a crash report reading game memory: never commit from there) */
    int i = lz_find(a); if (i < 0) return 0;
    LzReg *L = &g_lz[i]; u32 g = lz_g(L, a);
    pm_lock(&g_lz_m);
    if (!L->cm[g]) { lz_commit_g(L, g, g); g_lz_faults++; }
    int ok = L->cm[g];
    pm_unlock(&g_lz_m);
    return ok;   /* 1: repeat the access (committed now, possibly by another thread); 0: a real crash */
}
/* commit what [a, a+n) needs in large mappings; returns 1 when something was committed (the renderer then imports it) */
static int lz_ensure(u64 a, u64 n) {
    int nl = __atomic_load_n(&g_nlz, __ATOMIC_ACQUIRE); if (!nl || !n) return 0;
    u64 e = a + n; if (e < a) return 0;
    int did = 0;
    for (int i = 0; i < nl; i++) {
        LzReg *L = &g_lz[i]; if (e <= L->lo || a >= L->hi) continue;
        u64 lo = a > L->lo ? a : L->lo, hi = e < L->hi ? e : L->hi;
        u32 g0 = lz_g(L, lo), g1 = lz_g(L, hi - 1), k = g0;
        while (k <= g1 && __atomic_load_n(&L->cm[k], __ATOMIC_ACQUIRE)) k++;
        if (k > g1) continue;
        pm_lock(&g_lz_m); if (lz_commit_g(L, k, g1)) { did = 1; g_lz_ensured++; } pm_unlock(&g_lz_m);
    }
    return did;
}
/* zero [a, a+n) without committing granules that are not committed yet (they read zero anyway) */
static void lz_zero(u64 a, u64 n) {
    u64 e = a + n;
    while (a < e) {
        int i = lz_find(a);
        if (i < 0) { u64 q = e; int nl = __atomic_load_n(&g_nlz, __ATOMIC_ACQUIRE); for (int k = 0; k < nl; k++) if (g_lz[k].lo > a && g_lz[k].lo < q) q = g_lz[k].lo; h_memset((void *)a, 0, q - a); a = q; continue; }
        LzReg *L = &g_lz[i]; u32 g = lz_g(L, a); u64 q = lz_ghi(L, g); if (q > e) q = e;
        if (__atomic_load_n(&L->cm[g], __ATOMIC_ACQUIRE)) h_memset((void *)a, 0, q - a);
        a = q;
    }
}
/* t111 diagnostic: 1 when [a, a+n) runs past the end of the mapping it starts in (a large mapping, an arena, a pool
   chunk, or another registered range) into the next one. Since t109 mem_ok accepts such ranges (touching ranges are
   one stretch of memory); before, reads over them were refused (texture levels were dropped). */
static int mem_spans(u64 a, u64 n) {
    int i = lz_find(a); if (i >= 0) return a + n > g_lz[i].hi;
    for (int k = 0; k < g_narena; k++) if (a >= g_arena[k].lo && a < g_arena[k].hi) return a + n > g_arena[k].hi;
    if (pool_owns(a)) return pool_chunk_of(a) != pool_chunk_of(a + n - 1);
    int nm = __atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE);
    for (int k = 0; k < nm; k++) if (a >= g_mem[k].lo && a < g_mem[k].hi) return a + n > g_mem[k].hi;
    return 0;
}
static u64 g_fixed_inplace, g_fixed_reused_zeroed;
static int dmap_live_over(u64 va, u64 len);
static void *map_at_hint(void *hint, u64 len, int fixed, int zero) {
    void *p = 0;
    /* MAP_FIXED over a range that is already mapped (the game re-maps pieces of its own 3 GB streaming region):
       like FreeBSD, the new mapping replaces the old one at that same address - the game keeps using its address */
    if (hint && fixed && mem_ok((u64)hint, len) && !pool_owns((u64)hint)) {
        /* t134: a range the game released earlier (a heap slot reused since t131, or any released mapping) is a fresh
           mapping to the game: it reads as zero, like a pool mapping. A range still mapped keeps its bytes (the
           streaming region's own re-mapping of pieces it uses). */
        if (zero) { lz_zero((u64)hint, len); hw_note((u64)hint, len, 3); }   /* t111: (granules not committed yet read zero already) */
        else if (!dmap_live_over((u64)hint, len)) { lz_zero((u64)hint, len); hw_note((u64)hint, len, 3); g_fixed_reused_zeroed++; }
        g_fixed_inplace++;
        return hint;
    }
    if (hint && (p = arena_take((u64)hint, len, fixed && zero)) != 0) {   /* (zeroed as in the in-place case above: fixed flexible mappings) */ tr("    mapped "); tr_hex(len); tr(" bytes at "); tr_hex((u64)p); tr(" (arena)\n"); return p; }
    int lazy = lz_ok(len);   /* t111: reserved, committed on first use */
    if (hint) {
        p = lazy ? plat_reserve_at(hint, len) : plat_alloc_at(hint, len);
        if (!p) { tr("    WARNING: could not map at requested address "); tr_hex((u64)hint); tr(", using any address\n"); }
    }
    /* small mappings without a usable address come from shared pools (one registered / GPU-imported range each).
       Before, each got its own 2 MB-aligned allocation and its own memory region; the region table holds 256, and the
       game makes thousands of such mappings while it streams - everything mapped after the 256th was invisible to the
       renderer (textures never uploaded: black clothes, eyes, mouths; vertex/constant reads of zeros). */
    if (!p && len <= POOL_MAX_MAP) { p = pool_alloc(len); if (p) { tr("    mapped "); tr_hex(len); tr(" bytes at "); tr_hex((u64)p); tr(" (pool)\n"); return p; } }
    /* the PS4 GPU sees 40-bit addresses: keep every game mapping below 1 TB; like the PS4, unhinted mappings go to 8 GB..64 GB, below the fixed regions games request */
    static u64 low_next = 0x200000000ull;
    for (int tries = 0; !p && tries < 64 && low_next + len < 0x1000000000ull; tries++) {
        u64 at = (low_next + 0x1FFFFF) & ~0x1FFFFFull;
        if (at + len > POOL_LO && at < POOL_HI) at = POOL_HI;   /* skip the pool range */
        p = lazy ? plat_reserve_at((void *)at, len) : plat_alloc_at((void *)at, len);
        low_next = at + ((len + 0x1FFFFF) & ~0x1FFFFFull) + (p ? 0 : 0x40000000ull);
    }
    if (!p) { lazy = 0; p = plat_alloc_rwx(len); tr("    WARNING: mapping above the GPU's 40-bit range\n"); }
    if (lazy) lz_add(p, len); else mem_register(p, len);
    tr("    mapped "); tr_hex(len); tr(" bytes at "); tr_hex((u64)p); tr(" (requested "); tr_hex((u64)hint); tr(lazy ? "; committed as used)\n" : ")\n");
    return p;
}
/* Direct memory is "physical": the PS4 can map one physical range at two addresses and both show the same bytes.
   E6 gives every mapping its own memory, so record the physical ranges and report any second mapping of one. */
typedef struct { i64 phys; u64 len, va; int prot; u64 flip, caller; } DMap;
static DMap g_dmap[65536]; static int g_ndmap, g_dalias, g_dfixed_moved, g_dmaps_total; static u64 g_dreleases, g_vq_calls, g_vq_hits;
static PMutex g_dmap_lock;
static int dmap_live_over(u64 va, u64 len) {   /* t134: is any direct-memory mapping still recorded over [va, va+len)? */
    int r = 0; pm_lock(&g_dmap_lock);
    for (int i = 0; i < g_ndmap && !r; i++) if (g_dmap[i].va < va + len && va < g_dmap[i].va + g_dmap[i].len) r = 1;
    pm_unlock(&g_dmap_lock); return r;
}
/* the last 1024 direct-memory mappings that went away (released, or replaced by a new fixed mapping), with the flip
   and the caller: a crash report names the history of the memory a bad pointer points into */
typedef struct { u64 va, len, flip, caller, map_flip, map_caller; i64 phys; u8 why; } DRel;
static DRel g_drel[1024]; static u64 g_ndrel;
static void drel_add(const DMap *d, u64 va, u64 len, u8 why, u64 caller) {
    DRel *r = &g_drel[g_ndrel++ % 1024]; r->va = va; r->len = len; r->phys = d->phys + (i64)(va - d->va); r->flip = g_plat_frame; r->caller = caller;
    r->map_flip = d->flip; r->map_caller = d->caller; r->why = why;
}
static u64 g_dmap_caller;   /* caller of the mapping call in progress (for drel_add from dmap_unmap_va) */
/* drop (or trim) the recorded mappings that cover virtual range [va, va+len): a new FIXED mapping replaces them, as on the PS4 */
static void dmap_unmap_va(u64 va, u64 len) {
    for (int i = 0; i < g_ndmap; i++) {
        DMap *d = &g_dmap[i]; u64 lo = d->va, hi = d->va + d->len;
        if (hi <= va || lo >= va + len) continue;
        drel_add(d, lo > va ? lo : va, (hi < va + len ? hi : va + len) - (lo > va ? lo : va), 2, g_dmap_caller);
        if (lo >= va && hi <= va + len) { g_dmap[i] = g_dmap[--g_ndmap]; i--; continue; }
        if (lo < va && hi > va + len) {   /* split: keep both ends */
            if (g_ndmap < 65536) { DMap t = *d; t.va = va + len; t.phys += (i64)(va + len - lo); t.len = hi - (va + len); g_dmap[g_ndmap++] = t; }
            d->len = va - lo; continue;
        }
        if (lo < va) d->len = va - lo;                                              /* keep the front */
        else { u64 cut = va + len - lo; d->va += cut; d->phys += (i64)cut; d->len -= cut; }   /* keep the back */
    }
}
/* t130: the game's "lump" heaps. The game keeps a table of 468 heap address slots starting at 0x1200000000, 2 GB apart
   (eboot+0x94c5c0 / +0x94c660 build it), and takes them round-robin: each new heap gets the next slot, so over a
   session the heaps move up through 72 GB .. 1008 GB. The renderer's GPU page table covers 8 GB .. 256 GB; beyond it
   a shader finds memory only through a list of 64 regions, so after slot 92 (about 25-30 minutes of play) the GPU
   could not read most new heaps: vertex / index / constant data read as zeros (characters "exploding", black mouths,
   UI damage late in a session; t125 / t126 reached slots 138 / 199). The loader now patches the slot stride (default
   512 MB: 468 slots end at 0x4C80000000, inside the enlarged page-table window; heap_slot_mb = 2048 restores the
   game's own layout) and reports the slots here. A heap larger than the stride would run into the next slot: the
   largest seen so far is about 370 MB; such a mapping is counted and reported ("HEAP SLOT OVERFLOW"). */
#define HEAP_SLOT_BASE 0x1200000000ull
#define HEAP_SLOTS 468
static u32 g_heap_slot_mb = 512;   /* --heap-slot-mb N (2048 = the game's own) */
static u64 g_heap_largest, g_heap_overflow; static int g_heap_hi_slot = -1; static u64 g_heap_overflow_at;
static int heap_slot_of(u64 a) { u64 st = (u64)g_heap_slot_mb << 20; if (a < HEAP_SLOT_BASE || a >= HEAP_SLOT_BASE + HEAP_SLOTS * st) return -1; return (int)((a - HEAP_SLOT_BASE) / st); }
static void heap_slot_note(u64 a, u64 len) {
    int s = heap_slot_of(a); if (s < 0) return;
    u64 st = (u64)g_heap_slot_mb << 20, off = a + len - (HEAP_SLOT_BASE + (u64)s * st);
    if (off > g_heap_largest) g_heap_largest = off;
    if (s > g_heap_hi_slot) g_heap_hi_slot = s;
    if (off > st) { if (!g_heap_overflow++) g_heap_overflow_at = a; static int said; if (said++ < 10) { say("  HEAP SLOT OVERFLOW (t130): a mapping at "); say_hex(a); say(" + "); say_hex(len); say(" reaches past its "); say_dec(g_heap_slot_mb); say(" MB heap slot (slot "); say_dec(s); say(")\n"); } }
}
static SYSV int h_map_direct(void **addr, u64 len, int prot, int flags, i64 phys, u64 align) {
    TRACE("sceKernelMapDirectMemory", len, prot, phys);
    (void)align;
    void *want = *addr;
    void *p = map_at_hint(*addr, len, (flags & 0x10) != 0, 0);
    mj_add(MJ_MAPD, (u64)__builtin_return_address(0), (u64)want, len, (u64)(u32)prot | ((u64)(u32)flags << 32), (u64)phys, p ? 0 : (i64)0x8002000C, (u64)p, 0, 0);
    if (!p) return (int)0x8002000C;
    for (int i = 0; i < g_ndmap; i++)
        if (phys < g_dmap[i].phys + (i64)g_dmap[i].len && g_dmap[i].phys < phys + (i64)len) {
            if (g_dalias++ < 20) { say("  ALIAS: direct memory phys "); say_hex((u64)phys); say(" (+"); say_hex(len); say(") mapped again at "); say_hex((u64)p);
                say("; first mapped at "); say_hex(g_dmap[i].va); say(" - E6 gives it separate memory\n"); }
            break;
        }
    g_dmaps_total++;
    if (want && (flags & 0x10)) heap_slot_note((u64)want, len);   /* t130 */
    pm_lock(&g_dmap_lock);
    g_dmap_caller = (u64)__builtin_return_address(0);
    dmap_unmap_va((u64)p, len);
    if (g_ndmap < 65536) { g_dmap[g_ndmap].phys = phys; g_dmap[g_ndmap].len = len; g_dmap[g_ndmap].va = (u64)p; g_dmap[g_ndmap].prot = prot;
        g_dmap[g_ndmap].flip = g_plat_frame; g_dmap[g_ndmap].caller = g_dmap_caller; g_ndmap++; }
    pm_unlock(&g_dmap_lock);
    if (want && p != want && (flags & 0x10)) {   /* MAP_FIXED, but E6 could not give that address */
        if (g_dfixed_moved++ < 20) { say("  FIXED mapping moved: wanted "); say_hex((u64)want); say(", got "); say_hex((u64)p); say("\n"); }
    }
    *addr = p; return 0;
}
/* sceKernelReleaseDirectMemory: the physical range goes back; mappings of it that came from a pool are freed */
static SYSV int h_release_direct(i64 start, u64 len) {
    TRACE("sceKernelReleaseDirectMemory", start, len, 0);
    g_dreleases++;
    mj_add(MJ_REL, (u64)__builtin_return_address(0), (u64)start, len, 0, 0, 0, 0, 0, 0);
    if ((i64)len <= 0 || len > (64ull << 30)) {   /* a non-positive length (the game's release loop can compute one when the
                                                     next mapping lies past the range it frees): nothing to release */
        static int warned; if (warned++ < 5) { tr("    ReleaseDirectMemory: ignored length "); tr_hex(len); tr("\n"); }
        return 0;
    }
    pm_lock(&g_dmap_lock);
    for (int i = 0; i < g_ndmap; i++) {
        DMap *d = &g_dmap[i];
        i64 lo = d->phys, hi = d->phys + (i64)d->len, rlo = start, rhi = start + (i64)len;
        if (hi <= rlo || lo >= rhi) continue;
        if (lo >= rlo && hi <= rhi) {
            drel_add(d, d->va, d->len, 1, (u64)__builtin_return_address(0));
            if (pool_owns(d->va)) pool_release(d->va, d->len);
            g_dmap[i] = g_dmap[--g_ndmap]; i--;
        } else {   /* partly released: the mapping of the released part goes away */
            u64 va = d->va + (u64)((lo < rlo ? rlo : lo) - lo), n = (u64)((hi < rhi ? hi : rhi) - (lo < rlo ? rlo : lo));
            g_dmap_caller = (u64)__builtin_return_address(0);
            dmap_unmap_va(va, n); i = -1;
        }
    }
    pm_unlock(&g_dmap_lock);
    return 0;
}
static SYSV int h_map_flexible(void **addr, u64 len, int prot, int flags) {
    TRACE("sceKernelMapFlexibleMemory", len, prot, flags);
    void *want = *addr, *p = map_at_hint(*addr, len, (flags & 0x10) != 0, 1);
    mj_add(MJ_MAPF, (u64)__builtin_return_address(0), (u64)want, len, (u64)(u32)prot, (u64)(u32)flags, p ? 0 : (i64)0x8002000C, (u64)p, 0, 0);
    if (!p) return (int)0x8002000C;
    *addr = p; return 0;
}
/* sceKernelVirtualQuery over the direct-memory mappings E6 records. Info layout (0x48 bytes): start, end, offset (physical),
   i32 protection, i32 memory type, flags byte (bit 1 = direct memory, bit 4 = committed), name[32].
   flags bit 0 (FIND_NEXT): the first mapping at or above addr; otherwise only the mapping containing addr.
   The game frees streamed lumps with this: query each mapping of the range, release its physical memory. It used to fail
   always, so no lump memory was ever released and remapped ranges kept stale records. */
static SYSV int h_virtual_query(void *addr, int flags, void *info, u64 sz) {
    TRACE("sceKernelVirtualQuery", addr, flags, sz);
    u64 a = (u64)addr; int best = -1;
    __atomic_add_fetch(&g_vq_calls, 1, __ATOMIC_RELAXED);
    if (!info || sz < 0x21) return (int)0x80020016;
    pm_lock(&g_dmap_lock);
    for (int i = 0; i < g_ndmap; i++) {
        DMap *d = &g_dmap[i];
        if (a >= d->va && a < d->va + d->len) { best = i; break; }
        if ((flags & 1) && d->va > a && (best < 0 || d->va < g_dmap[best].va)) best = i;
    }
    if (best < 0) { pm_unlock(&g_dmap_lock); mj_add(MJ_VQ, (u64)__builtin_return_address(0), a, (u64)flags, 0, 0, (i64)0x80020016, 0, 0, 0); return (int)0x80020016; }   /* EACCES: nothing mapped there */
    DMap d = g_dmap[best];
    pm_unlock(&g_dmap_lock);
    mj_add(MJ_VQ, (u64)__builtin_return_address(0), a, (u64)flags, 0, 0, 0, d.va, d.va + d.len, (u64)d.phys);
    u8 *o = (u8 *)info; h_memset(o, 0, sz < 0x48 ? sz : 0x48);
    *(u64 *)(o + 0x00) = d.va; *(u64 *)(o + 0x08) = d.va + d.len; *(i64 *)(o + 0x10) = d.phys;
    *(i32 *)(o + 0x18) = d.prot; *(i32 *)(o + 0x1c) = 0; o[0x20] = 0x02 | 0x10;
    __atomic_add_fetch(&g_vq_hits, 1, __ATOMIC_RELAXED);
    return 0;
}
static SYSV u64 h_direct_size(void) { return 0x13C000000ull; }   /* 5,056 MB as on retail PS4 */

/* ---------------- kernel: time ---------------- */
static u64 g_tsc_freq = 1594000000ull;   /* the measured host TSC rate */
static SYSV u64 h_tsc_freq(void) { return g_opt_host_tsc ? g_tsc_freq : PS4_TSC_HZ; }
static SYSV u64 h_read_tsc(void) { return game_tsc(); }
static SYSV u64 h_process_time(void) { return plat_ticks() / (plat_ticks_per_sec() / 1000000ull); }
/* CLOCK_REALTIME (0) is wall-clock time since 1970 UTC, as on the PS4; every other clock counts from host boot.
   (Before, every clock - and time()/gettimeofday() - counted from host boot, so the game's date was in 1970.) */
static i64 g_wall_reads;
static void wall_ts(i64 *ts) { u64 n = plat_wall_ns(); ts[0] = (i64)(n / 1000000000ull); ts[1] = (i64)(n % 1000000000ull); __atomic_add_fetch(&g_wall_reads, 1, __ATOMIC_RELAXED); }
static SYSV int h_clock_gettime(int clk, i64 *ts) {
    if (!ts) return (int)0x80020016;
    if (clk == 0) { wall_ts(ts); return 0; }
    u64 t = plat_ticks(), f = plat_ticks_per_sec();
    ts[0] = (i64)(t / f); ts[1] = (i64)((t % f) * 1000000000ull / f); return 0;
}
static SYSV int h_gettimeofday(i64 *tv, void *tz) { (void)tz; i64 ts[2]; wall_ts(ts); tv[0] = ts[0]; tv[1] = ts[1] / 1000; return 0; }
static SYSV int h_usleep(u32 us) { plat_sleep_ms(us / 1000 ? us / 1000 : 1); return 0; }
static SYSV i64 h_time(i64 *t) { i64 ts[2]; wall_ts(ts); if (t) *t = ts[0]; return ts[0]; }

/* ---------------- kernel: threads / sync (startup is single-threaded) ---------------- */
static u8 g_main_pthread[256];
static u64 g_sync_obj[4096]; static int g_sync_n;
static SYSV int h_obj_init(u64 *obj) { if (obj) *obj = (u64)&g_sync_obj[(g_sync_n++) & 4095]; return 0; }
static SYSV int h_ok(void) { return 0; }
static SYSV void *h_pthread_self(void) { return g_main_pthread; }
static SYSV int h_pthread_equal(u64 a, u64 b) { return a == b; }
static void *g_spec[512]; static int g_nkeys = 1;
static SYSV int h_key_create(int *k, void *d) { (void)d; *k = g_nkeys++; return 0; }
static SYSV void *h_getspecific(int k) { return (k > 0 && k < 512) ? g_spec[k] : 0; }
static SYSV int h_setspecific(int k, void *v) { if (k > 0 && k < 512) g_spec[k] = v; return 0; }
static SYSV int h_thread_create(u64 *t, void *attr, void *entry, void *arg, const char *name) {
    TRACE("scePthreadCreate", entry, arg, name);
    (void)attr;
    tr("    thread NOT started: \""); tr(name ? name : "?"); tr("\" entry eboot+"); tr_hex((u64)entry - (u64)g_eboot.base); tr("\n");
    if (t) *t = (u64)&g_sync_obj[(g_sync_n++) & 4095];
    return 0;
}
static SYSV int h_eventflag_create(u64 *o, const char *n, u32 a, u64 init, void *opt) { (void)n; (void)a; (void)init; (void)opt; return h_obj_init(o); }
static SYSV int h_sema_create(u64 *o, const char *n, u32 a, int init, int max, void *opt) { (void)n; (void)a; (void)init; (void)max; (void)opt; return h_obj_init(o); }
static SYSV int h_equeue_create(u64 *o, const char *n) { (void)n; return h_obj_init(o); }

#include "e5_nid.c"
/* ---------------- modules ---------------- */
static SYSV void prx_cxa_finalize(void *p) { (void)p; }
static SYSV int prx_module_startstop(u64 a, const void *b) { (void)a; (void)b; return 0; }
static void *prx_import(const char *nm) {
    if (str_eq(nm, "__cxa_finalize")) return (void *)prx_cxa_finalize;
    if (str_eq(nm, "module_start") || str_eq(nm, "module_stop")) return (void *)prx_module_startstop;
    return 0;
}
static void rsq_arm_module(int mod); static void probe_arm(void);
static SYSV int h_load_start_module(const char *path, u64 argc, void *argv, u32 flags, void *opt, int *res) {
    TRACE("sceKernelLoadStartModule", path, argc, flags);
    (void)argv; (void)opt;
    say("  sceKernelLoadStartModule(\""); say(path); say("\")\n");
    char hp[1100];
    if (!map_path(hp, path)) return (int)0x80020002;
    if (g_prx_loaded) return 2;
    if (load_module(&g_prx, hp)) return (int)0x80020002;
    relocate(&g_prx, 0);
    g_prx_loaded = 1;
    rsq_arm_module(1);   /* t149 --rsqrt-trap */
    probe_arm();   /* t150 --prop-probe */
    if (res) *res = 0;
    return 2;   /* module handle */
}
static int g_dlsym_ok, g_dlsym_fail;
static SYSV int h_dlsym(int handle, const char *name, void **out) {
    TRACE("sceKernelDlsym", handle, name, out);
    if (handle != 2 || !g_prx_loaded) return (int)0x80020002;
    char nidbuf[12]; nid_of(name, nidbuf);
    const char *nid = nidbuf;
    for (u64 i = 0; i < g_prx.nsym; i++) {
        const char *s = g_prx.strs + g_prx.syms[i].name;
        int k = 0; while (k < 11 && s[k] == nid[k]) k++;
        if (k == 11 && g_prx.syms[i].shndx) { *out = g_prx.base + g_prx.syms[i].value; g_dlsym_ok++; return 0; }
    }
    g_dlsym_fail++;
    tr("    dlsym(\""); tr(name); tr("\") NOT FOUND\n");
    return (int)0x80020002;
}

/* ---------------- system services ---------------- */
static SYSV int h_get_initial_user(int *u) { TRACE("sceUserServiceGetInitialUser", u, 0, 0); if (u) *u = 1; return 0; }
static SYSV int h_sysmodule_is_loaded(u32 id) { (void)id; return 0; }

/* ---------------- lookup table ---------------- */
typedef struct { const char *name; void *fn; } HleEntry;
static const HleEntry g_hle[] = {
    {"memcpy", (void *)h_memcpy}, {"memmove", (void *)h_memmove}, {"memset", (void *)h_memset}, {"memcmp", (void *)h_memcmp},
    {"memchr", (void *)h_memchr}, {"strlen", (void *)h_strlen}, {"strcmp", (void *)h_strcmp}, {"strncmp", (void *)h_strncmp},
    {"strcasecmp", (void *)h_strcasecmp}, {"strncasecmp", (void *)h_strncasecmp}, {"strcpy", (void *)h_strcpy},
    {"strncpy", (void *)h_strncpy}, {"strcat", (void *)h_strcat}, {"strncat", (void *)h_strncat}, {"strchr", (void *)h_strchr},
    {"strrchr", (void *)h_strrchr}, {"strstr", (void *)h_strstr}, {"strpbrk", (void *)h_strpbrk}, {"strcspn", (void *)h_strcspn},
    {"strtok", (void *)h_strtok}, {"strtol", (void *)h_strtol}, {"strcoll", (void *)h_strcmp}, {"wcslen", (void *)h_wcslen},
    {"wcsncpy", (void *)h_wcsncpy}, {"rand", (void *)h_rand}, {"srand", (void *)h_srand}, {"qsort", (void *)h_qsort},
    {"malloc", (void *)h_malloc}, {"free", (void *)h_free}, {"calloc", (void *)h_calloc}, {"realloc", (void *)h_realloc},
    {"memalign", (void *)h_memalign},
    {"__cxa_guard_acquire", (void *)h_cxa_guard_acquire}, {"__cxa_guard_release", (void *)h_cxa_guard_release},
    {"__cxa_atexit", (void *)h_cxa_atexit}, {"atexit", (void *)h_atexit}, {"__cxa_pure_virtual", (void *)h_pure_virtual},
    {"_ZSt9terminatev", (void *)h_terminate}, {"__cxa_throw", (void *)h_cxa_throw}, {"__cxa_allocate_exception", (void *)h_cxa_alloc_exc},
    {"_Unwind_Resume", (void *)h_unwind_resume}, {"__stack_chk_fail", (void *)h_stack_chk_fail}, {"_Assert", (void *)h_assert},
    {"exit", (void *)h_exit}, {"_Exit", (void *)h_exit}, {"__error", (void *)h_error},
    {"sinf", (void *)h_sinf}, {"cosf", (void *)h_cosf}, {"tanf", (void *)h_tanf}, {"asinf", (void *)h_asinf},
    {"acosf", (void *)h_acosf}, {"atanf", (void *)h_atanf}, {"atan2f", (void *)h_atan2f}, {"expf", (void *)h_expf},
    {"exp2f", (void *)h_exp2f}, {"powf", (void *)h_powf}, {"fmodf", (void *)h_fmodf}, {"modff", (void *)h_modff},
    {"sin", (void *)h_sin}, {"cos", (void *)h_cos}, {"tan", (void *)h_tan}, {"asin", (void *)h_asin}, {"acos", (void *)h_acos},
    {"atan", (void *)h_atan}, {"atan2", (void *)h_atan2}, {"exp", (void *)h_exp}, {"exp2", (void *)h_exp2}, {"pow", (void *)h_pow},
    {"fmod", (void *)h_fmod}, {"fmax", (void *)h_fmax}, {"fmin", (void *)h_fmin}, {"ldexp", (void *)h_ldexp}, {"modf", (void *)h_modf},
    {"hypot", (void *)h_hypot}, {"_Sin", (void *)h__Sin}, {"_FSin", (void *)h__FSin}, {"_Log", (void *)h__Log}, {"_FLog", (void *)h__FLog},
    {"_FSinh", (void *)h__FSinh},
    {"printf", (void *)vt_printf}, {"sprintf", (void *)vt_sprintf}, {"fprintf", (void *)vt_fprintf}, {"snprintf", (void *)vt_snprintf},
    {"vsnprintf", (void *)h_vsnprintf}, {"vsprintf", (void *)h_vsprintf}, {"vprintf", (void *)h_vprintf}, {"vfprintf", (void *)h_vfprintf},
    {"puts", (void *)h_puts}, {"putchar", (void *)h_putchar}, {"perror", (void *)h_perror}, {"swprintf", (void *)h_swprintf},
    {"sscanf", (void *)h_sscanf},
    {"fopen", (void *)h_fopen}, {"fclose", (void *)h_fclose}, {"fread", (void *)h_fread}, {"fwrite", (void *)h_fwrite},
    {"fseek", (void *)h_fseek}, {"ftell", (void *)h_ftell}, {"fflush", (void *)h_fflush}, {"fputs", (void *)h_fputs},
    {"fputc", (void *)h_fputc}, {"getc", (void *)h_getc}, {"fgets", (void *)h_fgets}, {"feof", (void *)h_feof}, {"ferror", (void *)h_ferror},
    {"sceKernelOpen", (void *)h_kopen}, {"sceKernelClose", (void *)h_kclose}, {"sceKernelRead", (void *)h_kread},
    {"sceKernelLseek", (void *)h_klseek}, {"sceKernelPread", (void *)h_kpread}, {"sceKernelWrite", (void *)h_kwrite},
    {"sceKernelStat", (void *)h_kstat}, {"sceKernelFstat", (void *)h_kfstat},
    {"sceKernelPwrite", (void *)h_kpwrite}, {"sceKernelMkdir", (void *)h_kmkdir}, {"sceKernelUnlink", (void *)h_kunlink},
    {"sceKernelRename", (void *)h_krename}, {"sceKernelRmdir", (void *)h_krmdir}, {"sceKernelFtruncate", (void *)h_kftruncate},
    {"sceKernelTruncate", (void *)h_ktruncate}, {"sceKernelChmod", (void *)h_kchmod}, {"sceKernelUtimes", (void *)h_kutimes},
    {"sceKernelUuidCreate", (void *)h_kuuid}, {"rename", (void *)h_rename},
    {"sceKernelFsync", (void *)h_kfsync},
    {"open", (void *)h_kopen}, {"close", (void *)h_kclose}, {"read", (void *)h_kread}, {"write", (void *)h_kwrite},
    {"lseek", (void *)h_klseek}, {"stat", (void *)h_kstat}, {"fstat", (void *)h_kfstat},
    {"sceKernelAllocateDirectMemory", (void *)h_alloc_direct}, {"sceKernelReleaseDirectMemory", (void *)h_release_direct}, {"sceKernelMapDirectMemory", (void *)h_map_direct},
    {"sceKernelMapFlexibleMemory", (void *)h_map_flexible}, {"sceKernelGetDirectMemorySize", (void *)h_direct_size}, {"sceKernelVirtualQuery", (void *)h_virtual_query},
    {"sceKernelGetTscFrequency", (void *)h_tsc_freq}, {"sceKernelReadTsc", (void *)h_read_tsc},
    {"sceKernelGetProcessTime", (void *)h_process_time}, {"sceKernelClockGettime", (void *)h_clock_gettime},
    {"clock_gettime", (void *)h_clock_gettime}, {"sceKernelGettimeofday", (void *)h_gettimeofday}, {"gettimeofday", (void *)h_gettimeofday},
    {"sceKernelUsleep", (void *)h_usleep}, {"usleep", (void *)h_usleep}, {"time", (void *)h_time},
    {"scePthreadMutexInit", (void *)h_obj_init}, {"scePthreadCondInit", (void *)h_obj_init}, {"scePthreadRwlockInit", (void *)h_obj_init},
    {"scePthreadMutexattrInit", (void *)h_obj_init}, {"scePthreadCondattrInit", (void *)h_obj_init}, {"scePthreadAttrInit", (void *)h_obj_init},
    {"pthread_mutex_init", (void *)h_obj_init}, {"pthread_cond_init", (void *)h_obj_init}, {"pthread_mutexattr_init", (void *)h_obj_init},
    {"pthread_attr_init", (void *)h_obj_init},
    {"scePthreadSelf", (void *)h_pthread_self}, {"scePthreadEqual", (void *)h_pthread_equal}, {"pthread_equal", (void *)h_pthread_equal}, {"pthread_self", (void *)h_pthread_self},
    {"scePthreadKeyCreate", (void *)h_key_create}, {"scePthreadGetspecific", (void *)h_getspecific}, {"scePthreadSetspecific", (void *)h_setspecific},
    {"scePthreadCreate", (void *)h_thread_create},
    {"sceKernelCreateEventFlag", (void *)h_eventflag_create}, {"sceKernelCreateSema", (void *)h_sema_create},
    {"sceKernelCreateEqueue", (void *)h_equeue_create},
    {"sceKernelLoadStartModule", (void *)h_load_start_module}, {"__tls_get_addr", (void *)h_tls_get_addr}, {"sceKernelDlsym", (void *)h_dlsym},
    {"sceUserServiceGetInitialUser", (void *)h_get_initial_user}, {"sceSysmoduleIsLoaded", (void *)h_sysmodule_is_loaded},
    {0, 0}};

#include "e4_threads.c"   /* E4: real threads + sync objects (checked before g_hle) */
/* t108: memory in the log (RAM use on 16 GB PCs): this process's private commit and working set (with peaks), the game
   memory it has mapped, and what the system has left. Every 10 s of play, at exit and in crash reports.
   t109: every 60 s and at exit / crash also a breakdown: game memory by kind, the renderer's memory and the GPU
   driver's figures (g_mem_report_gpu), and the address space walked by kind of committed memory. */
static void (*g_mem_report_gpu)(void); static int g_memrep_crash;   /* t110: in a crash report: no calls that take locks (heaps, the driver) */
static PlatHeapWalk g_hwk; static int g_hwk_ok;   /* t113: the heaps, walked at exit */
static const char *mem_alloc_name(u64 ab) {
    int n = __atomic_load_n(&g_nmnames, __ATOMIC_ACQUIRE); if (n > MEM_NAMES) n = MEM_NAMES;
    for (int i = 0; i < n; i++) { const char *nm = __atomic_load_n(&g_mnames[i].name, __ATOMIC_ACQUIRE); if (nm && plat_alloc_base(g_mnames[i].p) == ab) return nm; }
    if (g_hwk_ok) {
        for (int i = 0; i < g_hwk.nr; i++) if (g_hwk.r[i].abase == ab) {
            static const char *const seg[3] = {"C runtime heap", "process heap: graphics driver / Windows", "another heap"};
            static const char *const blk[3] = {"C runtime heap, one large block", "process heap, one large block", "another heap, one large block"};
            return g_hwk.r[i].big ? blk[g_hwk.r[i].kind] : seg[g_hwk.r[i].kind]; }
        return "unnamed: graphics driver or Windows";
    }
    return 0;
}
static void mem_report(const char *why) {
    static u32 ncall; int detail = !str_eq(why, "every 10 s") || ncall++ % 6 == 0;
    PlatProcMem pm; int ok = plat_proc_mem(&pm);
    int nm = (int)__atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE);
    u64 game = 0; for (int i = 0; i < nm; i++) game += g_mem[i].hi - g_mem[i].lo;
    say("    memory ("); say(why); say("): ");
    if (ok) { say("this program "); say_dec((i64)(pm.priv >> 20)); say(" MB committed (peak "); say_dec((i64)(pm.priv_peak >> 20)); say("), "); say_dec((i64)(pm.ws >> 20)); say(" MB in RAM (peak "); say_dec((i64)(pm.ws_peak >> 20)); say("); "); }
    say("game memory mapped "); say_dec((i64)(game >> 20)); say(" MB in "); say_dec(nm); say(" ranges (pools "); say_dec((i64)(g_pool_bytes >> 20)); say(" MB, "); say_dec((i64)(g_pool_used >> 20)); say(" MB used)");
    if (pm.sys_total) { say("; system: RAM free "); say_dec((i64)(pm.sys_avail >> 20)); say(" of "); say_dec((i64)(pm.sys_total >> 20)); say(" MB, commit free "); say_dec((i64)(pm.commit_avail >> 20)); say(" of "); say_dec((i64)(pm.commit_total >> 20)); say(" MB"); }
    say("\n");
    if (pm.sys_total && pm.sys_avail < (1ull << 30)) { static int nlow; if (nlow++ < 20) say("    LOW MEMORY: less than 1 GB of RAM free on this PC (Windows may be paging; other programs closed would help)\n"); }
    if (!detail) return;
    /* game memory by kind: arenas (reserved / committed / mapped by the game), pools, the rest (the game's large mappings) */
    u64 dm_live = 0, dm_arena = 0; int ndm = 0;
    if (pm_trylock(&g_dmap_lock) == 0) {   /* (not in a crash report while another thread holds it) */
        for (int i = 0; i < g_ndmap; i++) { DMap *d = &g_dmap[i]; dm_live += d->len; ndm++;
            for (int k = 0; k < g_narena; k++) if (d->va >= g_arena[k].lo && d->va < g_arena[k].hi) { dm_arena += d->len; break; } }
        pm_unlock(&g_dmap_lock);
    }
    {   /* t130: the game's heap slots in use (distinct slots with live direct-memory mappings) */
        static u8 live[HEAP_SLOTS]; h_memset(live, 0, sizeof live); int nlive = 0, hi = -1;
        if (pm_trylock(&g_dmap_lock) == 0) { for (int i = 0; i < g_ndmap; i++) { int sl = heap_slot_of(g_dmap[i].va); if (sl >= 0 && sl < HEAP_SLOTS && !live[sl]) { live[sl] = 1; nlive++; if (sl > hi) hi = sl; } } pm_unlock(&g_dmap_lock); }
        say("      heap slots (t130, "); say_dec(g_heap_slot_mb); say(" MB each): in use "); say_dec(nlive); say(", highest in use "); say_dec(hi); say(", highest ever "); say_dec(g_heap_hi_slot);
        say(" of "); say_dec(HEAP_SLOTS); say(", largest heap "); say_dec((i64)(g_heap_largest >> 20)); say(" MB");
        if (g_heap_overflow) { say(", OVERFLOWING THE SLOT "); say_dec((i64)g_heap_overflow); say(" (first at "); say_hex(g_heap_overflow_at); say(")"); }
        say("\n");
    }
    say("      game memory: arenas "); say_dec(g_narena); say(" reserving "); say_dec((i64)(g_arena_reserved >> 20)); say(" MB, committed "); say_dec((i64)(g_arena_committed >> 20));
    say(" MB in "); say_dec((i64)g_arena_runs); say(" runs, mapped there by the game "); say_dec((i64)(dm_arena >> 20)); say(" MB");
    if (g_arena_commit_fail) { say(" (commit failures "); say_dec((i64)g_arena_commit_fail); say(")"); }
    say("; pools "); say_dec((i64)(g_pool_bytes >> 20)); say(" MB; other mappings "); { u64 o = game - g_arena_committed - g_pool_bytes; if (o > game) o = 0; say_dec((i64)(o >> 20)); }
    say(" MB; direct-memory mappings live "); say_dec(ndm); say(" ("); say_dec((i64)(dm_live >> 20)); say(" MB)\n");
    if (g_lz_reserved) { say("      large mappings committed as used (t111): "); say_dec(g_nlz); say(" reserving "); say_dec((i64)(g_lz_reserved >> 20)); say(" MB, committed ");
        say_dec((i64)(g_lz_committed >> 20)); say(" MB in "); say_dec((i64)g_lz_runs); say(" runs (first touches "); say_dec((i64)g_lz_faults); say(", for the GPU "); say_dec((i64)g_lz_ensured);
        if (g_lz_fail) { say(", commit failures "); say_dec((i64)g_lz_fail); }
        say("; imported while read-only "); say_dec((i64)g_lz_ro_ok); say(", the driver wanted them writable "); say_dec((i64)g_lz_ro_refused); say(", import failures "); say_dec((i64)g_lz_import_fail);
        say("; GPU stores caught in a trap page "); say_dec((i64)g_lz_trap_hits); say(")\n"); }
    g_memrep_crash = str_eq(why, "at the crash");
    if (g_mem_report_gpu) g_mem_report_gpu();
    if (str_eq(why, "at exit") && !g_hwk_ok) g_hwk_ok = plat_heap_walk(&g_hwk);   /* t113 (exit only: the walk holds each heap's lock) */
    PlatMemWalk w; if (plat_mem_walk(&w, mem_in_game)) {
        say("      address space: committed private "); say_dec((i64)(w.priv >> 20)); say(" MB (game memory "); say_dec((i64)(w.priv_game >> 20)); say(", other "); say_dec((i64)((w.priv - w.priv_game) >> 20));
        say(" of which in RAM about "); say_dec((i64)(w.other_res >> 20)); say("), mapped "); say_dec((i64)(w.mapped >> 20)); say(" MB, program files "); say_dec((i64)(w.image >> 20));
        say(" MB (in RAM about "); say_dec((i64)(w.image_res >> 20)); say("); ");
        if (ok && pm.priv > w.priv) { say("charged outside the address space "); say_dec((i64)((pm.priv - w.priv) >> 20)); say(" MB"); }
        say("; "); say_dec((i64)w.nreg); say(" regions\n");
        /* t110: the largest private allocations outside game memory (committed / in RAM), to name them against the renderer's figures */
        say("      largest other allocations (MB committed / in RAM):");
        for (int i = 0; i < 8 && w.top[i].bytes; i++) { say(" "); say_hex(w.top[i].abase); say(" "); say_dec((i64)(w.top[i].bytes >> 20)); say("/"); say_dec((i64)(w.top[i].res >> 20));
            const char *nm = mem_alloc_name(w.top[i].abase); if (nm) { say(" ("); say(nm); say(")"); } if (i < 7 && w.top[i + 1].bytes) say(","); }
        say("\n");
    }
    PlatHeaps hp; if (!g_memrep_crash && plat_heaps(&hp)) {
        say("      heaps: "); say_dec((i64)hp.n); say(", committed "); say_dec((i64)(hp.committed >> 20)); say(" MB (C runtime heap "); say_dec((i64)(hp.crt >> 20)); say(" MB; largest");
        for (int i = 0; i < 4 && hp.top[i]; i++) { say(" "); say_dec((i64)(hp.top[i] >> 20)); }
        say(" MB)\n");
    }
    if (g_hwk_ok && str_eq(why, "at exit")) {   /* t113 */
        static const char *const hn[3] = {"C runtime heap (ours: translator, renderer tables)", "process heap (graphics driver, Windows)", "other heaps"};
        say("      heaps walked at exit (t113), MB in use / blocks / of that in blocks of 1 MB and up / free but committed:");
        for (int k = 0; k < 3; k++) if (g_hwk.walked[k]) { say(k ? "; " : " "); say(hn[k]); say(" "); say_dec((i64)(g_hwk.busy[k] >> 20)); say(" / "); say_dec((i64)g_hwk.nbusy[k]);
            say(" / "); say_dec((i64)(g_hwk.big[k] >> 20)); say(" in "); say_dec((i64)g_hwk.nbig[k]); say(" / "); say_dec((i64)(g_hwk.freeb[k] >> 20)); }
        say("\n");
    }
    if (str_eq(why, "at exit")) {   /* t110: game memory pages that hold only zeros (never written, or cleared): what committing on first use could save */
        say("      game memory pages holding only zeros (MB zero / MB):");
        u64 zs = 0, ts = 0; int n = (int)__atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE);   /* (committed ranges only: reading reserved ones would commit them) */
        for (int i = 0; i < n; i++) {
            u64 lo = (g_mem[i].lo + 0xFFF) & ~0xFFFull, hi = g_mem[i].hi & ~0xFFFull, z = 0;
            for (u64 p = lo; p < hi; p += 4096) { const u64 *q = (const u64 *)p; u64 x = 0; for (int k = 0; k < 512 && !x; k += 8) x = q[k] | q[k + 1] | q[k + 2] | q[k + 3] | q[k + 4] | q[k + 5] | q[k + 6] | q[k + 7]; if (!x) z += 4096; }
            if (hi - lo >= (64ull << 20)) { say(" "); say_hex(lo); say(" "); say_dec((i64)(z >> 20)); say("/"); say_dec((i64)((hi - lo) >> 20)); }
            else { zs += z; ts += hi > lo ? hi - lo : 0; }
        }
        say("; smaller ranges "); say_dec((i64)(zs >> 20)); say("/"); say_dec((i64)(ts >> 20)); say("\n");
    }
}
#include "e5_gpu.c"       /* E5: video output, Gnm driver calls, PM4 capture, fake GPU (checked first) */
#include "e5_savedata.c"  /* E5: save data / temporary data backed by host folders */
#include "e5_np.c"        /* E5: trophies and PSN state */
#include "e5_fiber.c"     /* E5: libSceFiber */
#include "e5_audio.c"     /* E5: libSceAudioOut -> PC sound device */
#include "e5_libc.c"      /* E5: remaining libc: ctype tables, number parsing, time, setjmp */
#include "e5_system.c"    /* E5: system/user service, controller */

static void *hle_lookup(const char *name, int idx) {
    (void)idx;
    for (int i = 0; g_hle_sys[i].name; i++) if (str_eq(g_hle_sys[i].name, name)) return g_hle_sys[i].fn;
    for (int i = 0; g_hle_libc2[i].name; i++) if (str_eq(g_hle_libc2[i].name, name)) return g_hle_libc2[i].fn;
    for (int i = 0; g_hle_audio[i].name; i++) if (str_eq(g_hle_audio[i].name, name)) return g_hle_audio[i].fn;
    for (int i = 0; g_hle_fiber[i].name; i++) if (str_eq(g_hle_fiber[i].name, name)) return g_hle_fiber[i].fn;
    for (int i = 0; g_hle_np[i].name; i++) if (str_eq(g_hle_np[i].name, name)) return g_hle_np[i].fn;
    for (int i = 0; g_hle_save[i].name; i++) if (str_eq(g_hle_save[i].name, name)) return g_hle_save[i].fn;
    for (int i = 0; g_hle_gpu[i].name; i++) if (str_eq(g_hle_gpu[i].name, name)) return g_hle_gpu[i].fn;
    for (int i = 0; g_hle_thr[i].name; i++) if (str_eq(g_hle_thr[i].name, name)) return g_hle_thr[i].fn;
    if (str_eq(name, "__stack_chk_guard")) return &g_stack_guard;
    if (str_eq(name, "_Stdin")) return g_stdin_obj;
    if (str_eq(name, "_Stdout")) return g_stdout_obj;
    if (str_eq(name, "_Stderr")) return g_stderr_obj;
    if (str_eq(name, "_ZTVN10__cxxabiv117__class_type_infoE")) return g_typeinfo_vt[0];
    if (str_eq(name, "_ZTVN10__cxxabiv120__si_class_type_infoE")) return g_typeinfo_vt[1];
    if (str_eq(name, "_ZTVN10__cxxabiv121__vmi_class_type_infoE")) return g_typeinfo_vt[2];
    if (str_eq(name, "_Flt")) return &g_flt_props;   /* t152 */
    if (str_eq(name, "_Dbl")) return &g_dbl_props;
    if (str_eq(name, "_Locale") || str_eq(name, "Need_sceLibc") || str_eq(name, "_ZTIi"))
        return g_zero_blob;
    if (str_eq(name, "__gxx_personality_v0")) return (void *)h_terminate;
    for (int i = 0; g_hle[i].name; i++) if (str_eq(g_hle[i].name, name)) return g_hle[i].fn;
    return 0;
}

static void e3_fatal_summary(void);
static void hle_init(void) {
    fd_locks_init();
    /* measure the host TSC frequency (game code also uses raw rdtsc) */
    u64 t0 = plat_ticks(), c0 = __builtin_ia32_rdtsc();
    plat_sleep_ms(100);
    u64 t1 = plat_ticks(), c1 = __builtin_ia32_rdtsc();
    g_tsc_freq = (u64)((double)(c1 - c0) * (double)plat_ticks_per_sec() / (double)(t1 - t0));
    if (!g_opt_host_tsc) g_tsc_mul = (u64)(((unsigned __int128)PS4_TSC_HZ << 32) / g_tsc_freq);
    say("  host TSC frequency: "); say_dec((i64)(g_tsc_freq / 1000000)); say(" MHz; the game's clock runs at ");
    say(g_opt_host_tsc ? "the host rate (--host-tsc: game time runs fast)\n" : "1594 MHz like a PS4\n");
    g_on_fatal = e3_fatal_summary;
    pm_init(&g_reg_lock); pm_init(&g_dmap_lock);
}

/* crash report: what the direct-memory records say about address v (current mapping, and the last mappings of it
   that went away) */
static void eb_off(Sb *t, u64 a) { if (a >= (u64)g_eboot.base && a < (u64)g_eboot.base + g_eboot.size) { sb_s(t, "eboot+"); sb_x(t, a - (u64)g_eboot.base); } else sb_x(t, a); }
static void vm_describe(u64 v, void *f) {
    char b[300];
    for (int i = 0; i < g_ndmap; i++) { const DMap *d = &g_dmap[i]; if (v < d->va || v >= d->va + d->len) continue;
        Sb t = {b, 0, sizeof b}; sb_s(&t, "      direct memory mapping "); sb_x(&t, d->va); sb_s(&t, "+"); sb_x(&t, d->len); sb_s(&t, " phys "); sb_x(&t, (u64)d->phys);
        sb_s(&t, ", mapped at flip "); sb_d(&t, (i64)d->flip); sb_s(&t, " by "); eb_off(&t, d->caller); sb_s(&t, "\n"); plat_write(f, b); break; }
    int shown = 0;
    for (u64 k = g_ndrel; k-- > (g_ndrel > 1024 ? g_ndrel - 1024 : 0) && shown < 4; ) { const DRel *r = &g_drel[k % 1024];
        if (v < r->va || v >= r->va + r->len) continue;
        Sb t = {b, 0, sizeof b}; sb_s(&t, r->why == 1 ? "      earlier mapping released at flip " : "      earlier mapping replaced at flip "); sb_d(&t, (i64)r->flip);
        sb_s(&t, " by "); eb_off(&t, r->caller); sb_s(&t, " (it was mapped at flip "); sb_d(&t, (i64)r->map_flip); sb_s(&t, " by "); eb_off(&t, r->map_caller);
        sb_s(&t, ", phys "); sb_x(&t, (u64)r->phys); sb_s(&t, ")\n"); plat_write(f, b); shown++; }
}
/* the memory-call journal, oldest first (crash report) */
static void mj_dump(void *f) {
    static const char *kn[] = {"?", "AllocateDirectMemory", "MapDirectMemory", "ReleaseDirectMemory", "VirtualQuery", "MapFlexibleMemory"};
    static const char *an[][4] = {{0}, {"start", "end", "len", "align"}, {"want", "len", "prot|flags", "phys"}, {"phys", "len", 0, 0}, {"addr", "flags", 0, 0}, {"want", "len", "prot", "flags"}};
    u64 n = __atomic_load_n(&g_mj_n, __ATOMIC_ACQUIRE), from = n > MJ_N ? n - MJ_N : 0;
    char b[400]; Sb s = {b, 0, sizeof b};
    sb_s(&s, "\nmemory calls, last "); sb_d(&s, (i64)(n - from)); sb_s(&s, " of "); sb_d(&s, (i64)n); sb_s(&s, " (oldest first):\n"); plat_write(f, b);
    for (u64 i = from; i < n; i++) { const MjEnt *e = &g_mj[i % MJ_N]; if (e->kind < 1 || e->kind > 5) continue;
        Sb t = {b, 0, sizeof b}; sb_s(&t, "  "); sb_s(&t, kn[e->kind]); sb_s(&t, " ["); sb_s(&t, e->tn ? e->tn : "?"); sb_s(&t, "] from ");
        if (e->caller >= (u64)g_eboot.base && e->caller < (u64)g_eboot.base + g_eboot.size) { sb_s(&t, "eboot+"); sb_x(&t, e->caller - (u64)g_eboot.base); } else sb_x(&t, e->caller);
        const u64 v[4] = {e->a, e->b, e->c, e->d};
        for (int k = 0; k < 4; k++) if (an[e->kind][k]) { sb_s(&t, " "); sb_s(&t, an[e->kind][k]); sb_s(&t, "="); sb_x(&t, v[k]); }
        if (e->r) { sb_s(&t, " -> error "); sb_x(&t, (u64)e->r); }
        else if (e->kind == MJ_ALLOC) { sb_s(&t, " -> phys "); sb_x(&t, e->o1); }
        else if (e->kind == MJ_MAPD || e->kind == MJ_MAPF) { sb_s(&t, " -> "); sb_x(&t, e->o1); }
        else if (e->kind == MJ_VQ) { sb_s(&t, " -> "); sb_x(&t, e->o1); sb_s(&t, "-"); sb_x(&t, e->o2); sb_s(&t, " phys "); sb_x(&t, e->o3); }
        sb_s(&t, "\n"); plat_write(f, b); }
}

#include "e6_heapwatch.c"   /* --watch-heap: stray-write detector for the streaming crash (STATUS 40) */
