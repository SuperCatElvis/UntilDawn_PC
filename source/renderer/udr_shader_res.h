/* udr_shader_res.h - where every descriptor a game program uses comes from. Ours.
 *
 * Purpose: state-independent shader compilation (RENDERER_PLAN stage 2). E6's translator finds each image, buffer and
 * sampler descriptor by reading game memory at translation time, so its output depends on the frame's state. The
 * native path instead binds every descriptor to a Vulkan binding chosen from where the program itself takes it:
 *   UDR_SRC_SLOT   the descriptor is in user SGPRs that a signature slot names (udr_shader_sig.h);
 *   UDR_SRC_EUD    it is loaded from the extended-user-data table, at a dword offset a signature slot names;
 *   UDR_SRC_TABLE  it is loaded from a table whose pointer a signature slot names (pointer kinds 0x12-0x1A), at a
 *                  constant dword offset (table entry). In the 6,778 programs of the game files, image entries of
 *                  pointer kind 0x13 sit at multiples of 8 dwords, sampler entries of kind 0x15 at multiples of 4,
 *                  constant-buffer entries of kind 0x16 at multiples of 4 (STATUS 22).
 *   UDR_SRC_CODE   a buffer V# the program builds itself over data in its own code: base = S_GETPC_B64 + a constant
 *                  (S_ADD_U32 / S_ADDC_U32 with a literal and 0), dwords 2 and 3 literals (S_MOV_B32 / S_MOVK_I32).
 *                  Seen in the lighting pixel shaders of capture 15284 (tables of 16 to 288 bytes after the code).
 * The producer of the IR (Gnm hooks, or the capture converter) reads each descriptor from the same place at draw time
 * and binds the resource it describes.
 *
 * Method: one linear pass over the code tracking the origin of every scalar register (user SGPR at entry, or loaded by
 * s_load from a pointer pair at a constant offset, or copied by s_mov). Any other write makes the register unknown.
 * Branches are not followed separately (descriptor set-up sits before the first branch in the programs checked);
 * a use whose descriptor cannot be traced is reported with the reason, never guessed. */
#ifndef UDR_SHADER_RES_H
#define UDR_SHADER_RES_H
#include "udr_shader_sig.h"

enum { UDR_USE_IMAGE = 1, UDR_USE_SAMPLER, UDR_USE_BUFFER, UDR_USE_CBUF, UDR_USE_TABLE, UDR_USE_GDS };   /* MIMG resource, MIMG sampler, MUBUF/MTBUF, S_BUFFER_LOAD,
                                                                                    S_LOAD through a pointer (only with udr_res_record_tables) */
enum { UDR_SRC_NONE = 0, UDR_SRC_SLOT, UDR_SRC_EUD, UDR_SRC_TABLE, UDR_SRC_CODE, UDR_SRC_GDS };   /* GDS: DS_APPEND / DS_CONSUME counters (the global data share, one per GPU) */
enum { UDR_RES_READ = 1, UDR_RES_WRITE = 2, UDR_RES_ATOMIC = 4 };
enum {
    UDR_WHY_OK = 0,
    UDR_WHY_UNKNOWN_ORIGIN,         /* first descriptor dword written by an operation other than a load or a move */
    UDR_WHY_USER_NOT_CONSECUTIVE,   /* dwords from user SGPRs, but not in order */
    UDR_WHY_USER_NO_SLOT,           /* user SGPRs that no slot starts at */
    UDR_WHY_USER_INSIDE_SLOT,       /* user SGPRs inside a slot that starts lower */
    UDR_WHY_LOAD_NOT_CONSECUTIVE,   /* dwords loaded, but not in order from one pointer */
    UDR_WHY_COMPUTED_POINTER,       /* loaded through a pointer that is not a user SGPR pair */
    UDR_WHY_POINTER_NO_SLOT,        /* loaded through user SGPRs that no slot names */
    UDR_WHY_EUD_NO_SLOT,            /* loaded from the extended-user-data table at an offset no slot names */
    UDR_WHY_POINTER_KIND,           /* loaded through a slot that is not a pointer kind */
    UDR_WHY_CODE_NOT_CONSTANT,      /* based on the program counter, but the other dwords are not literals (or not a buffer V#) */
    UDR_WHY_COUNT
};
typedef struct {
    udr_u32 pc;             /* byte offset of the instruction in the code */
    udr_u8 use, dwords, src, why;
    udr_u8 kind;            /* SLOT / EUD: the slot's kind; TABLE: the pointer slot's kind */
    udr_u8 api_slot;        /* SLOT / EUD: the slot's API slot */
    udr_u8 reg;             /* SLOT: first user SGPR; EUD: 16 + table dword; TABLE: the pointer's first user SGPR */
    udr_u8 access;          /* UDR_RES_READ / _WRITE / _ATOMIC bits; 0 = the instruction names a resource but does not access
                               its memory (buffer cache invalidation) */
    udr_u16 table_dword;    /* TABLE: dword offset of the descriptor in the table; CODE: byte offset of the data from the code start */
    udr_u16 extent;         /* TABLE (udr_res_record_tables): dwords from the pointer up to the end of this load, 0xFFFF when not constant */
    udr_u32 lit[2];         /* CODE: dwords 2 and 3 of the V# (the literals the program moves there) */
} UdrResUse;
#define UDR_RES_MAX 1024
typedef struct {
    udr_u32 nuse, overflow, ninst;
    int decode_error;       /* 1: an instruction encoding was not recognised (the walk stopped there) */
    udr_u32 n_unresolved;
    udr_u32 unreached;      /* _cfg: basic blocks no path reaches (their uses are not listed) */
    UdrResUse use[UDR_RES_MAX];
} UdrResMap;

/* code = the program from its first instruction (udr_shader_code_start), code_bytes from the signature. Returns 1 when
   every resource use was traced (n_unresolved == 0 and no decode error), else 0; out is filled either way. */
int udr_shader_resources(const void *code, udr_u32 code_bytes, const UdrShaderSig *sig, UdrResMap *out);
/* the same, control-flow aware: origins are followed along branches and merged where paths join (STATUS 22). The
   callbacks provide working memory (about 416 bytes per basic block). */
int udr_shader_resources_cfg(const void *code, udr_u32 code_bytes, const UdrShaderSig *sig, UdrResMap *out,
                             void *(*alloc)(udr_u64 bytes), void (*fre)(void *p));
const char *udr_res_why_name(udr_u32 why);
const char *udr_res_use_name(udr_u32 use);
/* 1: also record every S_LOAD_DWORD* as a UDR_USE_TABLE use of the pointer pair it reads through (dwords = 2), so the
   native compiler can bind the table it reads (default 0: the corpus statistics count descriptors only) */
extern int udr_res_record_tables;
/* the fetch shader of the next analysis (0 = none): walked at the program's S_SWAPPC_B64, uses at 0x100000 + offset */
void udr_res_set_fetch(const void *code, udr_u32 bytes);
/* the fetch shader call of a vertex program (S_SWAPPC_B64 before the first branch, address in a user SGPR pair) */
int udr_res_find_fetch_call(const void *code, udr_u32 code_bytes, udr_u32 *pc, udr_u32 *reg);
udr_u32 udr_res_fetch_length(const void *code, udr_u32 max);   /* bytes up to and including S_SETPC_B64, 0 if none */
#endif
