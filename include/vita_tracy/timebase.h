#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Converts a timebase tick count to nanoseconds.
 *
 * Computed as (ticks / hz) * 1e9 + ((ticks % hz) * 1e9) / hz rather than
 * (ticks * 1e9) / hz: at the Vita timebase frequency the naive product
 * overflows 64 bits a few minutes into a session, which would wrap the
 * timeline mid-capture. Returns 0 when hz is 0. */
uint64_t vita_trace_ticks_to_ns(uint64_t ticks, uint32_t hz);

#ifdef __cplusplus
}
#endif
