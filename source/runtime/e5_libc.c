/* e5_libc.c - the remaining PS4 libc (Dinkumware) functions the game imports: character tables,
 * number parsing, time conversion, setjmp/longjmp and C++ runtime error paths. Included by e3_hle.c. */

/* ---- character classification tables (Dinkumware layout: 257 shorts, entry 0 is EOF) ---- */
#define C_XD 0x01
#define C_UP 0x02
#define C_SP 0x04
#define C_PU 0x08
#define C_LO 0x10
#define C_DI 0x20
#define C_CN 0x40
#define C_BB 0x80
static short g_ctype_tab[257], g_tolower_tab[257], g_toupper_tab[257];
static void libc_tables_init(void) {
    for (int c = 0; c < 256; c++) {
        short v = 0;
        if (c >= '0' && c <= '9') v |= C_DI | C_XD;
        if (c >= 'A' && c <= 'Z') v |= C_UP;
        if (c >= 'a' && c <= 'z') v |= C_LO;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) v |= C_XD;
        if (c == ' ') v |= C_SP;
        if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') v |= C_CN;
        if ((c < 32 && !(v & C_CN)) || c == 127) v |= C_BB;
        if (c > 32 && c < 127 && !(v & (C_DI | C_UP | C_LO))) v |= C_PU;
        g_ctype_tab[c + 1] = v;
        g_tolower_tab[c + 1] = (short)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        g_toupper_tab[c + 1] = (short)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    g_ctype_tab[0] = 0; g_tolower_tab[0] = -1; g_toupper_tab[0] = -1;
}
static SYSV const short *lc_getpctype(void) { return g_ctype_tab + 1; }
static SYSV const short *lc_getptolower(void) { return g_tolower_tab + 1; }
static SYSV const short *lc_getptoupper(void) { return g_toupper_tab + 1; }

/* ---- number parsing ---- */
static int lc_space(char c) { return c == ' ' || (c >= 9 && c <= 13); }
static int lc_digit(char c, int base) {
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
}
static u64 lc_parse_u(const char *s, char **end, int base, int *neg, int *any) {
    const char *p = s; *neg = 0; *any = 0;
    while (lc_space(*p)) p++;
    if (*p == '-') { *neg = 1; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && lc_digit(p[2], 16) >= 0) { p += 2; base = 16; }
    else if (base == 0) base = (p[0] == '0') ? 8 : 10;
    u64 v = 0; int d;
    while ((d = lc_digit(*p, base)) >= 0) { v = v * (u64)base + (u64)d; p++; *any = 1; }
    if (end) *end = (char *)(*any ? p : s);
    return v;
}
static SYSV u64 lc_stoul(const char *s, char **end, int base) { int n, a; u64 v = lc_parse_u(s, end, base, &n, &a); return n ? (u64)(-(i64)v) : v; }
static SYSV i64 lc_stoll(const char *s, char **end, int base) { int n, a; u64 v = lc_parse_u(s, end, base, &n, &a); return n ? -(i64)v : (i64)v; }
static double lc_pow10(int e) { double r = 1, b = 10; int n = e < 0 ? -e : e; while (n) { if (n & 1) r *= b; b *= b; n >>= 1; } return e < 0 ? 1 / r : r; }
static SYSV double lc_stod(const char *s, char **end, i64 pten) {
    const char *p = s; int neg = 0, any = 0, ex = 0;
    while (lc_space(*p)) p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') { p += 3; if (end) *end = (char *)p; return neg ? -__builtin_inf() : __builtin_inf(); }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') { p += 3; if (end) *end = (char *)p; return __builtin_nan(""); }
    double v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ex--; p++; any = 1; } }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1; int en = 0, es = 0;
        if (*q == '-') { es = 1; q++; } else if (*q == '+') q++;
        if (*q >= '0' && *q <= '9') { while (*q >= '0' && *q <= '9') { en = en * 10 + (*q - '0'); q++; } ex += es ? -en : en; p = q; }
    }
    if (end) *end = (char *)(any ? p : s);
    if (!any) return 0;
    v *= lc_pow10(ex + (int)pten);
    return neg ? -v : v;
}

/* ---- time (UTC; the host time zone is not modelled) ---- */
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; } LcTm;
static LcTm g_tm_static;
static void lc_gmtime(i64 t, LcTm *tm) {
    i64 days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->hour = (int)(rem / 3600); tm->min = (int)(rem % 3600 / 60); tm->sec = (int)(rem % 60);
    tm->wday = (int)((days + 4) % 7); if (tm->wday < 0) tm->wday += 7;
    i64 z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    tm->year = (int)(y - 1900); tm->mon = (int)(m - 1); tm->mday = (int)d; tm->isdst = 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    tm->yday = cum[tm->mon] + tm->mday - 1 + (leap && tm->mon > 1);
}
static SYSV LcTm *lc_gmtime_r(const i64 *t, LcTm *tm) { if (!t || !tm) return 0; lc_gmtime(*t, tm); return tm; }
static SYSV LcTm *lc_gmtime_p(const i64 *t) { return lc_gmtime_r(t, &g_tm_static); }
static SYSV int lc_gmtime_s(LcTm *tm, const i64 *t) { return lc_gmtime_r(t, tm) ? 0 : 22; }
static SYSV int lc_localtime_s(LcTm *tm, const i64 *t) { return lc_gmtime_s(tm, t); }
static SYSV i64 lc_mktime(LcTm *tm) {
    i64 y = tm->year + 1900LL + tm->mon / 12, m = tm->mon % 12; if (m < 0) { m += 12; y--; }
    i64 yy = y - (m < 2), era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400;
    i64 doy = (153 * (m > 1 ? m - 2 : m + 10) + 2) / 5 + tm->mday - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + doe - 719468;
    i64 t = days * 86400 + tm->hour * 3600LL + tm->min * 60LL + tm->sec;
    lc_gmtime(t, tm);
    return t;
}
static char g_asctime_buf[32];
static SYSV char *lc_asctime(const LcTm *tm) {
    static const char *dn = "SunMonTueWedThuFriSat", *mn = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char *b = g_asctime_buf; int k = 0;
    for (int i = 0; i < 3; i++) { b[k++] = dn[tm->wday % 7 * 3 + i]; }
    b[k++] = 0x20;
    for (int i = 0; i < 3; i++) { b[k++] = mn[tm->mon % 12 * 3 + i]; }
    b[k++] = 0x20;
    b[k++] = (char)('0' + tm->mday / 10); b[k++] = (char)('0' + tm->mday % 10); b[k++] = ' ';
    b[k++] = (char)('0' + tm->hour / 10); b[k++] = (char)('0' + tm->hour % 10); b[k++] = ':';
    b[k++] = (char)('0' + tm->min / 10); b[k++] = (char)('0' + tm->min % 10); b[k++] = ':';
    b[k++] = (char)('0' + tm->sec / 10); b[k++] = (char)('0' + tm->sec % 10); b[k++] = ' ';
    int y = tm->year + 1900; b[k++] = (char)('0' + y / 1000 % 10); b[k++] = (char)('0' + y / 100 % 10); b[k++] = (char)('0' + y / 10 % 10); b[k++] = (char)('0' + y % 10);
    b[k++] = '\n'; b[k] = 0; return b;
}
static SYSV char *lc_ctime(const i64 *t) { return lc_asctime(lc_gmtime_p(t)); }

/* ---- setjmp / longjmp (the game uses them only with its own jmp_buf) ---- */
/* jmp_buf: [0]=rbx [1]=rbp [2]=r12 [3]=r13 [4]=r14 [5]=r15 [6]=rsp after return [7]=return address [8]=mxcsr|fcw */
__attribute__((naked)) static SYSV int lc_setjmp(u64 *env) {
    __asm__ volatile(
        "mov %rbx, 0(%rdi)\n mov %rbp, 8(%rdi)\n mov %r12, 16(%rdi)\n mov %r13, 24(%rdi)\n mov %r14, 32(%rdi)\n mov %r15, 40(%rdi)\n"
        "lea 8(%rsp), %rdx\n mov %rdx, 48(%rdi)\n mov (%rsp), %rdx\n mov %rdx, 56(%rdi)\n"
        "stmxcsr 64(%rdi)\n fnstcw 68(%rdi)\n xor %eax, %eax\n ret\n");
}
__attribute__((naked)) static SYSV void lc_longjmp(u64 *env, int val) {
    __asm__ volatile(
        "mov %esi, %eax\n test %eax, %eax\n jnz 1f\n inc %eax\n1:\n"
        "mov 0(%rdi), %rbx\n mov 8(%rdi), %rbp\n mov 16(%rdi), %r12\n mov 24(%rdi), %r13\n mov 32(%rdi), %r14\n mov 40(%rdi), %r15\n"
        "ldmxcsr 64(%rdi)\n fldcw 68(%rdi)\n mov 48(%rdi), %rsp\n jmp *56(%rdi)\n");
}

/* ---- C++ runtime error paths: report clearly instead of returning garbage ---- */
static SYSV void lc_xlength(const char *m) { say("\nstd::length_error: "); say(m ? m : "?"); say("\n"); fatal_from_game("std::_Xlength_error"); }
static SYSV void lc_xrange(const char *m) { say("\nstd::out_of_range: "); say(m ? m : "?"); say("\n"); fatal_from_game("std::_Xout_of_range"); }
static SYSV void lc_xbadalloc(void) { fatal_from_game("std::_Xbad_alloc (out of memory)"); }
static SYSV void lc_cxx_catch(void) { fatal_from_game("a C++ exception handler (exceptions are not supported yet)"); }

/* ---- small stdio/misc ---- */
static SYSV char *lc_strerror(int e) { (void)e; return (char *)"PS4 error"; }
static SYSV u64 lc_wcsrtombs(char *dst, const u16 **src, u64 len, void *ps) {
    (void)ps; if (!src || !*src) return 0;
    const u16 *s = *src; u64 n = 0;
    while (*s && (!dst || n < len)) { u16 c = *s++; if (dst) dst[n] = (char)(c < 128 ? c : '?'); n++; }
    if (dst) { if (n < len) { dst[n] = 0; *src = 0; } else *src = s; }
    return n;
}
static SYSV int lc_remove(const char *p) { char hp[1100]; if (map_path(hp, p) != 2) return -1; return plat_unlink(hp); }
static SYSV int lc_zero(void) { return 0; }
static SYSV int lc_neg1(void) { return -1; }
/* struct lconv in the PS4 libc (Dinkumware) layout: 7 monetary strings (0x00-0x30), 14 chars (0x38-0x45),
   then decimal_point at 0x48, grouping 0x50, thousands_sep 0x58, followed by wide-character copies.
   Every string pointer must be valid: the game reads lconv->decimal_point[0] when parsing numbers. */
static u8 g_lconv[256];
static SYSV void *lc_localeconv(void) {
    static const char empty[8] = {0}, dot[8] = {'.', 0, 0, 0, 0, 0, 0, 0};   /* also valid as 16/32-bit wide strings */
    for (int off = 0; off < 0x38; off += 8) *(const char **)(g_lconv + off) = empty;
    for (int off = 0x38; off < 0x48; off++) g_lconv[off] = 127;                 /* CHAR_MAX: "not available" */
    *(const char **)(g_lconv + 0x48) = dot;
    *(const char **)(g_lconv + 0x50) = empty;
    *(const char **)(g_lconv + 0x58) = empty;
    for (int off = 0x60; off < 256; off += 8) *(const char **)(g_lconv + off) = empty;
    *(const char **)(g_lconv + 0x60 + 0x48) = dot;                               /* wide decimal point, same layout */
    return g_lconv;
}

static const HleEntry g_hle_libc2[] = {
    {"_Getpctype", (void *)lc_getpctype}, {"_Getptolower", (void *)lc_getptolower}, {"_Getptoupper", (void *)lc_getptoupper},
    {"_Stoul", (void *)lc_stoul}, {"_Stoull", (void *)lc_stoul}, {"_Stoll", (void *)lc_stoll}, {"_Stod", (void *)lc_stod},
    {"gmtime", (void *)lc_gmtime_p}, {"gmtime_s", (void *)lc_gmtime_s}, {"localtime", (void *)lc_gmtime_p},
    {"localtime_s", (void *)lc_localtime_s}, {"mktime", (void *)lc_mktime}, {"asctime", (void *)lc_asctime}, {"ctime", (void *)lc_ctime},
    {"setjmp", (void *)lc_setjmp}, {"longjmp", (void *)lc_longjmp},
    {"_ZSt14_Xlength_errorPKc", (void *)lc_xlength}, {"_ZSt14_Xout_of_rangePKc", (void *)lc_xrange}, {"_ZSt11_Xbad_allocv", (void *)lc_xbadalloc},
    {"__cxa_begin_catch", (void *)lc_cxx_catch}, {"__cxa_rethrow", (void *)lc_cxx_catch}, {"__cxa_call_unexpected", (void *)lc_cxx_catch},
    {"__cxa_end_catch", (void *)lc_zero},
    {"strerror", (void *)lc_strerror}, {"wcsrtombs", (void *)lc_wcsrtombs}, {"remove", (void *)lc_remove},
    {"setvbuf", (void *)lc_zero}, {"clearerr", (void *)lc_zero}, {"rewind", (void *)lc_zero}, {"ungetc", (void *)lc_neg1},
    {"fscanf", (void *)lc_neg1}, {"localeconv", (void *)lc_localeconv},
    {0, 0}};
