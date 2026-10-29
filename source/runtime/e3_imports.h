/* e3_imports.h - the PS4 system functions the game imports. Ours.
 *
 * The table is filled at load time from the game's own executable (imports_scan in e3_loader.c): every symbol the
 * eboot imports is named "<NID>#<library id>#<module id>", where the NID is a hash of the function's name (SHA-1 of the
 * name and a fixed suffix, e5_nid.c). An import gets its name when this program knows it - any name it implements
 * (the HleEntry tables), lists or traces - by comparing NIDs; the others are shown as "nid:<NID>". No list of the
 * game's imports and no third-party name table is part of this program. */
#define IMP_MAX 1024   /* the game imports 748 */
typedef struct { const char *raw, *name, *lib; char nid[12]; } ImportInfo;
static ImportInfo g_imports[IMP_MAX];
static int g_nimports;
#define N_IMPORTS g_nimports
#include "e3_stub_list.h"   /* STUB_LIST(X): X(0) .. X(IMP_MAX - 1), one logging stub per import slot */
