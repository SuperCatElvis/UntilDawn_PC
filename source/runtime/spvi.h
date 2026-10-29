/* spvi.h - a small SPIR-V interpreter for testing E6's translated shaders on the CPU (Linux tools only, not the runtime).
 *
 * Why: the CPU Vulkan device available in the workspace (SwiftShader) has no 64-bit integers and no
 * ConvertUToPtr, which every E6-mode shader uses (memory goes through a page table of device addresses). This runs
 * one invocation at a time, sequentially, with the semantics of the SPIR-V subset gcn2spv emits (no subgroup
 * operations, no images; barriers are no-ops: workgroups whose invocations must interleave cannot be run).
 *
 * Memory: PhysicalStorageBuffer pointers are device addresses translated by the host's dev_ptr callback; Uniform /
 * PushConstant / StorageBuffer variables point at host memory the caller binds (set 0, by binding number); Private,
 * Function, Input and Output variables live in per-invocation storage. Explicit layouts (Offset, ArrayStride) are
 * honoured; other types are laid out naturally. */
#ifndef SPVI_H
#define SPVI_H
#include <stdint.h>

typedef struct SpvModule SpvModule;
typedef struct {
    void *(*dev_ptr)(void *ctx, uint64_t addr, uint32_t bytes);   /* device address -> host memory, 0 = unmapped */
    void *ctx;
    uint8_t *binding[128];        /* set 0 buffers by binding number */
    uint8_t push[256];
    /* built-in inputs of the invocation */
    uint32_t vertex_index, instance_index, local_id[3], workgroup_id[3], num_workgroups[3];
    float frag_coord[4]; int front_facing;
    uint64_t max_steps;           /* per invocation; 0 = 50 million */
    /* compute workgroups (spvi_start / spvi_resume): the workgroup's shared memory (Workgroup variables, laid out in
       declaration order, 16-byte aligned; the caller clears it once per workgroup) and LocalInvocationIndex */
    uint8_t *workgroup; uint32_t workgroup_bytes; uint32_t local_index;
    /* images: OpImageFetch on the image bound at 'binding' (set 0) -> four 32-bit words; 0 = not available */
    int (*img_fetch)(void *ctx, int binding, int x, int y, int z, int lod, uint32_t out[4]);
} SpvEnv;

SpvModule *spvi_load(const uint32_t *words, uint32_t nwords, char *err, int errn);
void spvi_free(SpvModule *m);
/* run the entry point once; returns 1, or 0 with an error message (unsupported instruction, bad memory access, step limit) */
int spvi_run(SpvModule *m, SpvEnv *env, char *err, int errn);
/* a workgroup: each invocation has its own module instance (spvi_load of the same words); spvi_start runs one until it
   finishes (1), stops at an OpControlBarrier in the entry function (2), or fails (0); spvi_resume continues it.
   The caller runs every invocation to the barrier before resuming any (barriers in called functions: failure). */
int spvi_start(SpvModule *m, SpvEnv *env, char *err, int errn);
int spvi_resume(SpvModule *m);
uint32_t spvi_barriers(SpvModule *m);
/* outputs of the last run: by Location (vec4 of 32-bit words), or built-in (Position = 0, FragDepth = 22) */
int spvi_output_loc(SpvModule *m, uint32_t location, uint32_t out[4]);
int spvi_output_builtin(SpvModule *m, uint32_t builtin, uint32_t out[4]);
int spvi_killed(SpvModule *m);
uint64_t spvi_steps(SpvModule *m);
#endif
