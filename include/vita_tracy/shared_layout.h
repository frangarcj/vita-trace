#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vita_tracy/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Layout of the single memory block the application allocates and the
 * kernel maps: a header, one SPSC sample ring per CPU, and a control ring
 * for lifecycle/module/PMU events.
 *
 * The kernel sees this block at a different virtual address than the
 * process does, so the header stores byte offsets from the start of the
 * block and never pointers. Each side resolves them against its own base. */

#define VITA_TRACE_SHARED_MAGIC 0x56545348u /* "VTSH" */

/* Written into the header by the kernel while it services a register call.
 *
 * The kernel backend is optional, so the client links its control ABI as
 * weak imports and the module may simply not be loaded. What an unresolved
 * weak import returns is not documented, so a zero return is not proof the
 * call reached the kernel; this acknowledgement is, because only the kernel
 * can write into the mapping. */
#define VITA_TRACE_KERNEL_ACK 0x4B41434Bu /* "KACK" */

/* Rings are separated by a cache line so producers on different cores do
 * not share one. */
#define VITA_TRACE_SHARED_ALIGN 64u

typedef struct VitaTraceSharedHeader {
    uint32_t magic;
    uint32_t kernel_ack;
    uint32_t abi_version;
    uint32_t target_pid;
    uint32_t timebase_hz;
    uint32_t enabled_features;
    uint32_t core_count;
    uint32_t sample_capacity;
    uint32_t control_capacity;
    uint32_t core_ring_offset[VITA_TRACE_CORE_COUNT];
    uint32_t control_ring_offset;
} VitaTraceSharedHeader;

/* Bytes needed for a block holding `sample_capacity` samples per core and
 * `control_capacity` control records. Returns 0 if either capacity is not a
 * non-zero power of two. */
size_t vita_trace_shared_layout_size(uint32_t sample_capacity, uint32_t control_capacity);

/* Lays out and initializes the header and every ring. Returns 1 on
 * success, 0 on invalid arguments or an undersized block. */
int vita_trace_shared_init(void *mem, size_t mem_size, uint32_t target_pid, uint32_t timebase_hz,
                           uint32_t sample_capacity, uint32_t control_capacity);

/* Checks magic and ABI version before a consumer trusts the block. */
int vita_trace_shared_is_valid(const void *mem);

/* Records that the kernel has taken the block. Called from the kernel side
 * of a register call. */
void vita_trace_shared_acknowledge(void *mem);

/* True once the kernel has acknowledged this block. */
int vita_trace_shared_is_acknowledged(const void *mem);

/* Ring accessors. `mem` is whichever mapping the caller holds; both sides
 * pass their own base. Return NULL for an out-of-range cpu or an invalid
 * block. */
void *vita_trace_shared_core_ring(void *mem, uint32_t cpu);
void *vita_trace_shared_control_ring(void *mem);

#ifdef __cplusplus
}
#endif
