/* host (Linux) shim for ud_langscan.c tests: the small file / string layer it uses, on libc. Not part of any game build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
static void mem_copy(void *d, const void *s, u64 n) { memmove(d, s, n); }
static int str_len(const char *s) { return (int)strlen(s); }
static int str_eq(const char *a, const char *b) { return !strcmp(a, b); }
static int str_starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static void fmt_dec(char *b, i64 v) { sprintf(b, "%lld", v); }
typedef struct { char *b; int n, cap; } Sb;
static void sb_s(Sb *s, const char *t) { while (*t && s->n < s->cap - 1) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void path_join(char *out, const char *dir, const char *rest) { char t[1100]; snprintf(t, sizeof t, "%s/%s", dir, rest); strcpy(out, t); }
static void plat_write(void *h, const char *s) { fputs(s, h ? (FILE *)h : stdout); }
static void *plat_open_write(const char *p) { return fopen(p, "wb"); }
static void plat_close(void *h) { fclose((FILE *)h); }
static void plat_mkdir(const char *p) { mkdir(p, 0755); }
static i64 plat_file_size(const char *p) { struct stat st; if (stat(p, &st) || S_ISDIR(st.st_mode)) return -1; return (i64)st.st_size; }
static void *plat_read_file(const char *p, u64 *size) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, 2); long n = ftell(f); fseek(f, 0, 0); u8 *b = malloc((size_t)n + 1); *size = fread(b, 1, (size_t)n, f); b[*size] = 0; fclose(f); return b; }
static void *hf_open_rb(const char *p) { return fopen(p, "rb"); }
static void *hf_open_rw(const char *p, int create, int trunc) { (void)create; return fopen(p, trunc ? "wb" : "r+b"); }
static void hf_close(void *f) { fclose((FILE *)f); }
static u64 hf_read(void *f, void *b, u64 n) { return fread(b, 1, n, (FILE *)f); }
static u64 hf_write(void *f, const void *b, u64 n) { return fwrite(b, 1, n, (FILE *)f); }
static i64 hf_seek(void *f, i64 o, int w) { if (fseeko((FILE *)f, o, w)) return -1; return ftello((FILE *)f); }
static int ud_key_is(const char *a, const char *b) { while (*b) { if ((*a | 0x20) != (*b | 0x20)) return 0; a++; b++; } return *a == 0; }
static u64 str_to_u64(const char *s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0'); return v; }
static int plat_is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int plat_replace_file(const char *a, const char *b) { return rename(a, b) == 0; }
static int plat_unlink(const char *p) { return unlink(p); }
static int plat_rmdir(const char *p) { return rmdir(p); }
static void plat_exe_dir(char *out, u32 n) { if (!getcwd(out, n)) { out[0] = '.'; out[1] = 0; } }
static u64 plat_free_bytes(const char *d) { struct statvfs v; if (statvfs(d, &v)) { char p[1100]; snprintf(p, sizeof p, "%s/..", d); if (statvfs(p, &v)) return (u64)-1; } return (u64)v.f_bavail * v.f_frsize; }
static void lp_list_any(const char *dir, int dirs, void (*cb)(const char *, void *), void *ctx) { DIR *d = opendir(dir); struct dirent *e; char p[1100]; if (!d) return;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); if (plat_is_dir(p) == dirs) cb(e->d_name, ctx); } closedir(d); }
static void plat_list_files(const char *dir, void (*cb)(const char *, void *), void *ctx) { lp_list_any(dir, 0, cb, ctx); }
static void plat_list_dirs(const char *dir, void (*cb)(const char *, void *), void *ctx) { lp_list_any(dir, 1, cb, ctx); }
#include "langscan_host_extract.h"
#include "ud_manifest_v103.h"   /* generated: SHA-256 + sha_file + sfo_get copied from ud_launch.c */
int e3_main(int argc, char **argv);
int main(int argc, char **argv) { return e3_main(argc, argv); }
