/* imports_host_test.c - the import table read from the game's own executable (imports_scan, e3_loader.c):
 * every imported symbol "<NID>#<lib>#<module>" found once, its library named, and named after a function this
 * program knows exactly when that name's NID is the import's NID. Run by tests/run_host_tests.sh with your own
 * eboot.elf (UD_ELF_DIR); the HleEntry tables' names and the listed names come from the sources (imports_names.h,
 * written by run_host_tests.sh), the code under test from e3_loader.c / e5_nid.c (imports_part.h, nid_part.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
static int str_len(const char *s) { return (int)strlen(s); }
static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static void mem_copy(void *d, const void *s, u64 n) { memcpy(d, s, n); }
#pragma pack(push, 1)
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { i64 tag; u64 val; } Dyn;
typedef struct { u32 name; u8 info, other; u16 shndx; u64 value, size; } Sym;
typedef struct { u64 offset, info; i64 addend; } Rela;
#pragma pack(pop)
typedef struct { u8 *dl; Dyn *dyn; u64 ndyn; Sym *syms; u64 nsym; const char *strs; } Module;
typedef struct { const char *name; void *fn; } HleEntry;
#include "nid_part.h"
#include "imports_names.h"   /* g_hle_sys .. g_hle (names only), g_stub_ok, g_stub_known */
#define IMP_MAX 1024
typedef struct { const char *raw, *name, *lib; char nid[12]; } ImportInfo;
static ImportInfo g_imports[IMP_MAX];
static int g_nimports;
#define N_IMPORTS g_nimports
#include "imports_part.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot read %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); u8 *d = malloc((size_t)n); if (fread(d, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    Ehdr *eh = (Ehdr *)d; Phdr *ph = (Phdr *)(d + eh->phoff); Module m = {0};
    for (int i = 0; i < eh->phnum; i++) { if (ph[i].type == 0x61000000u) m.dl = d + ph[i].offset; if (ph[i].type == 2) { m.dyn = (Dyn *)(d + ph[i].offset); m.ndyn = ph[i].filesz / sizeof(Dyn); } }
    u64 strtab = 0, symtab = 0, symsz = 0;
    for (u64 i = 0; i < m.ndyn && m.dyn[i].tag; i++) { if (m.dyn[i].tag == 0x61000035) strtab = m.dyn[i].val; if (m.dyn[i].tag == 0x61000039) symtab = m.dyn[i].val; if (m.dyn[i].tag == 0x6100003F) symsz = m.dyn[i].val; }
    m.syms = (Sym *)(m.dl + symtab); m.nsym = symsz / sizeof(Sym); m.strs = (const char *)(m.dl + strtab);
    imports_scan(&m);
    int named = 0, nolib = 0;
    for (int i = 0; i < g_nimports; i++) { if (strncmp(g_imports[i].name, "nid:", 4)) named++; if (str_eq(g_imports[i].lib, "?")) nolib++;
        printf("%s %s %s\n", g_imports[i].raw, g_imports[i].name, g_imports[i].lib); }
    fprintf(stderr, "imports %d, named %d, shown by NID %d, without a library name %d\n", g_nimports, named, g_nimports - named, nolib);
    return g_nimports > 0 && nolib == 0 ? 0 : 1;
}
