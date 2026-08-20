#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic single-producer/single-consumer ring over a caller-owned memory
 * block (the userland-allocated block the kernel maps with
 * ksceKernelProcUserMap). The header lives at the start of that block,
 * fixed-size element slots follow it. The producer never blocks: a push
 * against a full ring increments `dropped` and is discarded.
 *
 * Field access must go through vita_trace_ring_* only: write_pos/read_pos
 * are updated with atomic release stores and read with atomic acquire loads
 * so the ring is safe to share between a kernel producer and a userland
 * consumer without locks. */
typedef struct VitaTraceRingHeader {
    uint32_t magic;
    uint32_t element_size;
    uint32_t capacity; /* slot count, power of two */
    uint32_t capacity_mask;
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t dropped;
} VitaTraceRingHeader;

#define VITA_TRACE_RING_MAGIC 0x56545241u /* "VTRA" */

int vita_trace_is_pow2(uint32_t value);

/* Total bytes vita_trace_ring_init needs for `capacity` slots of
 * `element_size` bytes each, header included. */
size_t vita_trace_ring_layout_size(uint32_t capacity, uint32_t element_size);

/* Initializes the ring header at the start of `mem`. `capacity` must be a
 * power of two and `mem_size` must be at least vita_trace_ring_layout_size.
 * Returns 1 on success, 0 on invalid arguments. */
int vita_trace_ring_init(void *mem, size_t mem_size, uint32_t capacity, uint32_t element_size);

/* Copies `element` (element_size bytes) into the ring. Returns 1 on
 * success, 0 if the ring was full (and increments dropped). */
int vita_trace_ring_try_push(void *mem, const void *element);

/* Copies the oldest element out of the ring into `out_element`. Returns 1
 * on success, 0 if the ring was empty. */
int vita_trace_ring_try_pop(void *mem, void *out_element);

uint32_t vita_trace_ring_dropped(const void *mem);
uint32_t vita_trace_ring_pending(const void *mem);

#ifdef __cplusplus
}
#endif
