/* udr_shaderscan <outdir> <files...> - extracts every GPU program of the game from Decima .core / .corestream files
 * (and from plain files such as executables) into <outdir>/t<type>_<hash>.bin. Ours.
 * Second version of tools/shaderscan.c: the code now starts where udr_shader_code_start finds it (the footer does not
 * follow the code; the first version cut 0-92 bytes off the start of most programs, STATUS 19d).
 * File layout: the program's code, then everything up to and including the 28-byte footer (metadata block, input usage
 * slots, footer), so udr_shader_sig_parse works on the file. hash = FNV-1a 64 of the code only (bit 0 set), the same as
 * the runtime check in udr_corpus_check.py. Also writes <outdir>/signatures.txt: hash, type, code bytes, slots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../udr_shader_sig.h"
static size_t lz4(const uint8_t *s, size_t n, uint8_t *d, size_t dn) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned tok = s[i++]; size_t ll = tok >> 4;
        if (ll == 15) { unsigned b; do { b = s[i++]; ll += b; } while (b == 255 && i < n); }
        if (o + ll > dn || i + ll > n) return o;
        memcpy(d + o, s + i, ll); o += ll; i += ll;
        if (i >= n || o >= dn) break;
        size_t off = s[i] | (s[i + 1] << 8); i += 2;
        size_t ml = tok & 15;
        if (ml == 15) { unsigned b; do { b = s[i++]; ml += b; } while (b == 255 && i < n); }
        ml += 4;
        if (off == 0 || off > o || o + ml > dn) return o;
        for (size_t k = 0; k < ml; k++) d[o + k] = d[o + k - off];
        o += ml;
    }
    return o;
}
/* Decima chunked container: u32 magic 0xCB10C183, u32 block size, u64 data length, then block sizes, then blocks
   (stored or LZ4) */
static uint8_t *load_core(const char *path, uint64_t *outlen) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t h[32]; if (fread(h, 1, 32, f) != 32) { fclose(f); return 0; }
    uint32_t magic, bsz; uint64_t dlen; memcpy(&magic, h, 4); memcpy(&bsz, h + 4, 4); memcpy(&dlen, h + 8, 8);
    if (magic != 0xCB10C183u || !bsz || bsz > (1u << 22) || dlen > (8ull << 30)) { fclose(f); return 0; }
    uint32_t nb = (uint32_t)((dlen + bsz - 1) / bsz); uint32_t *cs = malloc(4ull * (nb + 1));
    uint8_t *out = malloc(dlen + 16), *cb = malloc(1 << 22); uint64_t o = 0;
    if (fread(cs, 4, nb, f) != nb) nb = 0;
    for (uint32_t b = 0; b < nb; b++) {
        size_t want = (b + 1 < nb) ? bsz : (size_t)(dlen - (uint64_t)b * bsz);
        if (cs[b] > (1u << 22) || fread(cb, 1, cs[b], f) != cs[b]) break;
        size_t got = cs[b] == want ? (memcpy(out + o, cb, want), want) : lz4(cb, cs[b], out + o, want);
        o += got;
    }
    free(cs); free(cb); fclose(f); *outlen = o; return out;
}
#define HN (1 << 22)
static uint64_t g_seen[HN];
static int seen_add(uint64_t h) { uint64_t i = h & (HN - 1); while (g_seen[i]) { if (g_seen[i] == h) return 0; i = (i + 1) & (HN - 1); } g_seen[i] = h; return 1; }
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: udr_shaderscan <outdir> <files...> (or @list)\n"); return 2; }
    const char *outdir = argv[1]; long total = 0, uniq = 0, bad = 0, nostart = 0;
    char sp[600]; snprintf(sp, sizeof sp, "%s/signatures.txt", outdir); FILE *sigf = fopen(sp, "a");   /* appended: the scan may run in parts */
    for (int a = 2; a < argc; a++) {
        char **list = 0; int nl = 0;
        if (argv[a][0] == '@') { FILE *lf = fopen(argv[a] + 1, "r"); char l[1024]; list = malloc(sizeof(char *) * 200000);
            while (lf && fgets(l, sizeof l, lf) && nl < 200000) { l[strcspn(l, "\r\n")] = 0; if (*l) list[nl++] = strdup(l); } if (lf) fclose(lf); }
        else { list = malloc(sizeof(char *)); list[0] = argv[a]; nl = 1; }
        for (int li = 0; li < nl; li++) {
            const char *path = list[li];
            uint64_t n = 0; uint8_t *d = load_core(path, &n);
            if (!d) { FILE *f = fopen(path, "rb"); if (!f) continue; fseek(f, 0, SEEK_END); n = (uint64_t)ftell(f); fseek(f, 0, SEEK_SET);
                d = malloc(n + 16); if (fread(d, 1, n, f) != n) n = 0; fclose(f); }
            long here = 0, newhere = 0;
            for (uint64_t p = 0; p + 40 <= n; p++) {
                if (d[p] != 'O' || memcmp(d + p, "OrbShdr", 7)) continue;
                UdrShaderSig s; udr_u64 start;
                if (!udr_shader_sig_parse(d, p + 28, &s) || s.footer_pos != p || !s.code_bytes || (s.code_bytes & 3)) { bad++; continue; }
                if (!udr_shader_code_start(d, p + 28, &s, &start)) { nostart++; continue; }
                const uint8_t *code = d + start; uint32_t len = s.code_bytes;
                uint64_t h = 1469598103934665603ull; for (uint32_t k = 0; k < len; k++) { h ^= code[k]; h *= 1099511628211ull; }
                h |= 1; total++; here++;
                if (seen_add(h)) {
                    uniq++; newhere++;
                    char fn[700]; snprintf(fn, sizeof fn, "%s/t%u_%016llx.bin", outdir, s.program_type, (unsigned long long)h);
                    FILE *o = fopen(fn, "wb"); if (o) { fwrite(code, 1, p + 28 - start, o); fclose(o); }
                    if (sigf) { fprintf(sigf, "%016llx t%u code %u slots %u:", (unsigned long long)h, s.program_type, len, s.nslots);
                        for (unsigned i = 0; i < s.nslots; i++) fprintf(sigf, " %02x/%u@%u%s", s.slot[i].kind, s.slot[i].api_slot, s.slot[i].start_reg, (s.slot[i].flags & 1) ? "x8" : "");
                        fprintf(sigf, "\n"); }
                }
            }
            if (here) printf("%s: %ld programs, %ld new\n", path, here, newhere);
            free(d);
        }
    }
    if (sigf) fclose(sigf);
    fflush(stdout); printf("SUMMARY programs found %ld, unique %ld, bad footers %ld, code start not found %ld\n", total, uniq, bad, nostart);
    return 0;
}
