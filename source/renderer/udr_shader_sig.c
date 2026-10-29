/* udr_shader_sig.c - parser for the program footers and input usage slots (see udr_shader_sig.h). Ours. */
#include "udr_shader_sig.h"

static udr_u32 rd32(const udr_u8 *p) { return (udr_u32)p[0] | ((udr_u32)p[1] << 8) | ((udr_u32)p[2] << 16) | ((udr_u32)p[3] << 24); }

int udr_shader_sig_parse(const void *bytes, udr_u64 n, UdrShaderSig *sig) {
    const udr_u8 *d = (const udr_u8 *)bytes;
    if (n < 28) return 0;
    /* the footer follows the code; the code length is in the footer, so find the last "OrbShdr" */
    udr_u64 k = n - 7 + 1;
    while (k-- > 0) {
        if (d[k] == 'O' && d[k + 1] == 'r' && d[k + 2] == 'b' && d[k + 3] == 'S' && d[k + 4] == 'h' && d[k + 5] == 'd' && d[k + 6] == 'r') break;
        if (k == 0) return 0;
    }
    if (k + 28 > n) return 0;
    const udr_u8 *h = d + k;
    udr_u32 bits = rd32(h + 8);
    sig->version = h[7];
    sig->program_type = (bits >> 2) & 15;
    sig->code_bytes = bits >> 8;
    sig->footer_pos = (udr_u32)k;
    if (sig->code_bytes > k) return 0;
    udr_u32 base_dw = h[12], ns = h[13];
    sig->hash0 = rd32(h + 16); sig->hash1 = rd32(h + 20);
    if (ns > 64 || (udr_u64)4 * (base_dw + ns) > k) return 0;
    const udr_u8 *s = h - 4 * base_dw - 4 * ns;
    sig->nslots = ns; sig->chunk_base_dw = base_dw;
    for (udr_u32 i = 0; i < ns; i++) { sig->slot[i].kind = s[4 * i]; sig->slot[i].api_slot = s[4 * i + 1]; sig->slot[i].start_reg = s[4 * i + 2]; sig->slot[i].flags = s[4 * i + 3]; }
    return 1;
}
/* The footer does not follow the code directly: between them sits a metadata block of 0-92 bytes (seen in the 641
   programs of capture 16029) and the slot table. The code start is the nearest position before the slot table at
   which a program of code_bytes ends with s_endpgm (0xBF810000). Checked on all 641 programs of capture 16029: the
   rule finds the real start in every case (531 with a single candidate, 110 where it is the nearest of two). */
int udr_shader_code_start(const void *bytes, udr_u64 n, const UdrShaderSig *sig, udr_u64 *start) {
    const udr_u8 *d = (const udr_u8 *)bytes;
    udr_u64 table = (udr_u64)sig->footer_pos - 4ull * (sig->chunk_base_dw + sig->nslots);
    for (udr_u64 e = 0; e <= 256 && e + sig->code_bytes <= table; e += 4) {
        udr_u64 p = table - e - sig->code_bytes;
        if (sig->code_bytes >= 4 && p + sig->code_bytes <= n && rd32(d + p + sig->code_bytes - 4) == 0xBF810000u) { *start = p; return 1; }
    }
    return 0;
}
udr_u32 udr_sig_slot_dwords(const UdrSigSlot *s) {
    switch (s->kind) {
    case UDR_SIG_IMAGE_OR_BUFFER: case UDR_SIG_RW_RESOURCE: return (s->flags & 1) ? 8 : 4;
    case UDR_SIG_SAMPLER: case UDR_SIG_CONST_BUFFER: return 4;
    case UDR_SIG_PTR_EXTENDED_USER_DATA: return 2;
    default: return 0;
    }
}
const char *udr_sig_kind_name(udr_u32 kind) {
    switch (kind) {
    case UDR_SIG_IMAGE_OR_BUFFER: return "resource";
    case UDR_SIG_SAMPLER: return "sampler";
    case UDR_SIG_CONST_BUFFER: return "constant-buffer";
    case UDR_SIG_RW_RESOURCE: return "rw-resource";
    case UDR_SIG_PTR_EXTENDED_USER_DATA: return "ptr-extended-user-data";
    default: return "?";
    }
}
