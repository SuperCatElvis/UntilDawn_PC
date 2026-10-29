/* udr_bindplan.c - binding layout of a game program from its descriptor origins (see udr_bindplan.h). Ours. */
#include "udr_bindplan.h"

/* resource class: images and buffers are separate even with the same origin register (a T# and a V# can start at the
   same user SGPR only in different programs, but the class keeps the key unambiguous) */
/* An image descriptor that the program both reads (sample / load / query) and writes (store) gets two bindings: a
   sampled image for the reads and a storage image for the writes, views of the same image (as E6 groups them by the
   store flag). One storage binding for both would need reads without a format (shaderStorageImageReadWithoutFormat),
   and sampling or IMAGE_LOAD_MIP cannot use a storage view at all. */
enum { CL_IMAGE = 0, CL_SAMPLER = 1, CL_BUFFER = 2, CL_TABLE = 3, CL_IMAGE_W = 4, CL_GDS = 5 };
static int cls_of(const UdrResUse *u) {
    if (u->use == UDR_USE_IMAGE) return (u->access & (UDR_RES_WRITE | UDR_RES_ATOMIC)) ? CL_IMAGE_W : CL_IMAGE;
    return u->use == UDR_USE_SAMPLER ? CL_SAMPLER : u->use == UDR_USE_TABLE ? CL_TABLE : u->use == UDR_USE_GDS ? CL_GDS : CL_BUFFER;
}
/* order key: origin first (slot < eud < table), then register / table position, then class */
static udr_u64 key(const UdrResUse *u) {
    return ((udr_u64)u->src << 48) | ((udr_u64)u->reg << 40) | ((udr_u64)u->table_dword << 16) | ((udr_u64)u->kind << 8) | (udr_u64)cls_of(u);
}
int udr_bind_plan(const UdrResMap *m, UdrBindPlan *p) {
    p->nbind = p->n_unresolved = p->n_no_access = p->overflow = 0;
    udr_u64 k[UDR_PLAN_MAX];
    for (udr_u32 i = 0; i < m->nuse && i < UDR_RES_MAX; i++) {
        const UdrResUse *u = &m->use[i];
        p->use_binding[i] = UDR_PLAN_NONE;
        if (!u->access) { p->n_no_access++; continue; }
        if (u->why) { p->n_unresolved++; continue; }
        udr_u64 kk = key(u); udr_u32 j = 0;
        while (j < p->nbind && (k[j] != kk || (u->src == UDR_SRC_CODE && (p->b[j].lit[0] != u->lit[0] || p->b[j].lit[1] != u->lit[1])))) j++;
        if (j == p->nbind) {
            if (p->nbind >= UDR_PLAN_MAX) { p->overflow++; continue; }
            UdrPlanBinding *b = &p->b[j]; k[j] = kk; p->nbind++;
            b->src = u->src; b->kind = u->kind; b->api_slot = u->api_slot; b->reg = u->reg; b->table_dword = u->table_dword;
            b->dwords = u->dwords; b->access = 0; b->nuses = 0; b->type = 0; b->lit[0] = u->lit[0]; b->lit[1] = u->lit[1];
        }
        UdrPlanBinding *b = &p->b[j];
        b->access |= u->access; if (b->nuses < 255) b->nuses++;
        if (u->dwords > b->dwords) b->dwords = u->dwords;
        p->use_binding[i] = (udr_u16)j;
    }
    /* types from the classes and accesses */
    for (udr_u32 j = 0; j < p->nbind; j++) {
        int c = (int)(k[j] & 0xFF);
        UdrPlanBinding *b = &p->b[j];
        b->type = c == CL_SAMPLER ? UDR_BIND_SAMPLER : (c == CL_BUFFER || c == CL_TABLE || c == CL_GDS) ? UDR_BIND_STORAGE_BUFFER
                : c == CL_IMAGE_W ? UDR_BIND_STORAGE_IMAGE : UDR_BIND_SAMPLED_IMAGE;
    }
    /* binding numbers in key order: the same program always gets the same layout */
    udr_u32 ord[UDR_PLAN_MAX];
    for (udr_u32 j = 0; j < p->nbind; j++) ord[j] = j;
    for (udr_u32 a = 1; a < p->nbind; a++) { udr_u32 t = ord[a], q = a; while (q > 0 && k[ord[q - 1]] > k[t]) { ord[q] = ord[q - 1]; q--; } ord[q] = t; }
    for (udr_u32 r = 0; r < p->nbind; r++) p->b[ord[r]].binding = r;
    return p->n_unresolved == 0 && p->overflow == 0;
}
const char *udr_bind_type_name(udr_u32 type) {
    switch (type) {
    case UDR_BIND_SAMPLED_IMAGE: return "sampled image"; case UDR_BIND_STORAGE_IMAGE: return "storage image";
    case UDR_BIND_SAMPLER: return "sampler"; case UDR_BIND_STORAGE_BUFFER: return "storage buffer";
    case UDR_BIND_UNIFORM_BUFFER: return "uniform buffer"; case UDR_BIND_COMBINED: return "combined image sampler";
    default: return "?";
    }
}
