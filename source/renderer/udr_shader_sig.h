/* udr_shader_sig.h - resource signatures of the game's GPU programs. Ours.
 *
 * Every program binary of the game (6,727 in the game files, 100% parse) ends with a footer that starts with the
 * 7 bytes "OrbShdr". The footer gives the code length and the number of "input usage slots". The slots sit just
 * before the footer and say, for each resource the program uses, what it is, its API slot (the slot number the game
 * binds it under), and in which user-data register it arrives. Registers 0-15 are the user SGPRs; registers from 16
 * on are dwords of a table that a pointer slot (kind PTR_EXTENDED_USER_DATA) points to.
 *
 * The footer does not sit right after the code (a metadata block and the slot table are between them); use
 * udr_shader_code_start to find the code. NOTE: the shader corpus extracted in October (shader_corpus*) assumed the
 * code ends at the footer, so its files miss the first 0-92 bytes of most programs (STATUS 19); it must be extracted
 * again before the offline compiler uses it.
 *
 * Checked against a running frame (STATUS 19): the tiled-light producer pixel shader 0x24f35cd00 has
 *   image slot 0 in s0-s7, read/write buffer slot 0 in s8-s11, extended-user-data pointer in s12-s13,
 *   read/write buffer slot 1 at register 16 (table dword 0), constant buffer slot 12 at register 20 (table dword 4),
 * and its code loads exactly table offsets 0 and 16 bytes from s[12:13].
 *
 * This is what makes state-independent shader compilation possible (RENDERER_PLAN stage 2): the compiler binds
 * each resource to a Vulkan binding derived from (kind, API slot), and the IR producer finds each descriptor at the
 * register the signature names. Kind names are ours, inferred from how each kind is used (register count, the code
 * that reads it); codes not yet confirmed are reported as such, never guessed. */
#ifndef UDR_SHADER_SIG_H
#define UDR_SHADER_SIG_H
#include "udr_ir.h"

enum {
    UDR_SIG_IMAGE_OR_BUFFER = 0x00,        /* read-only resource: 8 dwords (image, flags & 1) or 4 dwords (buffer) */
    UDR_SIG_SAMPLER = 0x01,                /* 4 dwords */
    UDR_SIG_CONST_BUFFER = 0x02,           /* 4 dwords */
    UDR_SIG_RW_RESOURCE = 0x04,            /* read/write resource: 4 or 8 dwords */
    UDR_SIG_PTR_EXTENDED_USER_DATA = 0x1B, /* 2 dwords: pointer to the table that holds registers 16 and up */
};

typedef struct { udr_u8 kind, api_slot, start_reg, flags; } UdrSigSlot;
typedef struct {
    udr_u32 code_bytes;          /* length of the program's code */
    udr_u32 program_type;        /* footer type field (values seen: 0-8; meaning per stage not yet mapped) */
    udr_u32 version;             /* footer version (7 in every program of the game) */
    udr_u32 nslots; UdrSigSlot slot[64];
    udr_u32 hash0, hash1;        /* footer hashes */
    udr_u32 footer_pos;          /* byte offset of "OrbShdr" in the bytes given */
    udr_u32 chunk_base_dw;       /* footer field: dwords between the slot table and the footer */
} UdrShaderSig;

/* bytes = the program binary (code followed by the footer). Returns 1 and fills sig, or 0 if it is not one. */
int udr_shader_sig_parse(const void *bytes, udr_u64 n, UdrShaderSig *sig);
/* where the code starts in the bytes given (see udr_shader_sig.c): 1 and *start, or 0 if no position fits */
int udr_shader_code_start(const void *bytes, udr_u64 n, const UdrShaderSig *sig, udr_u64 *start);
/* dwords a slot occupies (0 = unknown kind) */
udr_u32 udr_sig_slot_dwords(const UdrSigSlot *s);
const char *udr_sig_kind_name(udr_u32 kind);   /* "?" for codes not yet confirmed */
#endif
