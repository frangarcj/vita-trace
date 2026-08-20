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

/* Whether a reported timebase frequency can be believed.
 *
 * scePerfGetTimebaseFrequency() is a tail-call into ScePamgr, and no retail
 * firmware module provides ScePamgr, so on a CEX the call lands on an
 * unresolved import stub that returns 0xFFFFFFFF. Dividing by that collapses
 * every timestamp to zero, which looks like a working profiler producing an
 * empty timeline rather than like a failure. */
int vita_trace_timebase_hz_is_plausible(uint32_t hz);

/* Derives the frequency from a measured interval: how many timebase ticks
 * elapsed over a known number of microseconds. Returns 0 if the measurement
 * is unusable. */
uint32_t vita_trace_timebase_calibrate(uint64_t ticks_elapsed, uint64_t micros_elapsed);

#ifdef __cplusplus
}
#endif
