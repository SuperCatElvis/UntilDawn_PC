/* udr_bindplan.h - the Vulkan binding layout of one game program, derived from where its descriptors come from. Ours.
 *
 * This is the contract between the offline shader compiler (udshaderc, RENDERER_PLAN stage 2) and the IR producer
 * (the capture converter now, the Gnm hooks later):
 *   - the compiler gives every resource use of the program the binding of its origin, so the SPIR-V does not depend
 *     on descriptor values (E6 resolves them from memory while translating; STATUS 24c is what that costs);
 *   - the producer, at draw time, reads the descriptor at each binding's origin (user SGPRs of a signature slot, a dword
 *     of the extended-user-data table, or an entry of a table a pointer slot names), creates or finds the resource it
 *     describes, and binds it there.
 *
 * Uses with the same origin share one binding: an image read and written becomes one storage image, a V# read by
 * scalar loads and by buffer instructions becomes one storage buffer. Bindings are numbered in a fixed order of their
 * origins, so the same program always gets the same layout. Uses whose origin was not traced get no binding and are
 * counted; a program with such uses cannot be compiled in this mode (they need composite descriptors or bind-time
 * evaluation, RENDERER_PLAN 8a). */
#ifndef UDR_BINDPLAN_H
#define UDR_BINDPLAN_H
#include "udr_shader_res.h"
#include "udr_ir.h"

typedef struct {
    udr_u32 binding;            /* Vulkan binding number (descriptor set 0) */
    udr_u32 type;               /* UDR_BIND_SAMPLED_IMAGE, _STORAGE_IMAGE, _SAMPLER, _STORAGE_BUFFER */
    udr_u32 access;             /* UDR_RES_* bits over all its uses */
    udr_u8 src, kind, api_slot, reg;   /* origin, as in UdrResUse */
    udr_u16 table_dword; udr_u8 dwords, nuses;
    udr_u32 lit[2];             /* UDR_SRC_CODE: dwords 2 and 3 of the V# */
} UdrPlanBinding;
#define UDR_PLAN_MAX 64
#define UDR_PLAN_NONE 0xFFFFu
typedef struct {
    udr_u32 nbind, n_unresolved, n_no_access, overflow;
    UdrPlanBinding b[UDR_PLAN_MAX];
    udr_u16 use_binding[UDR_RES_MAX];   /* per entry of the UdrResMap: index into b, or UDR_PLAN_NONE */
} UdrBindPlan;

/* builds the plan from a resource map (best from udr_shader_resources_cfg). Returns 1 when every use that accesses a
   resource has a binding. */
int udr_bind_plan(const UdrResMap *m, UdrBindPlan *p);
const char *udr_bind_type_name(udr_u32 type);
#endif
