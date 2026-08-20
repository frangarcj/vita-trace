#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_TRACY_PMU_MAX_PROBE 8u

/* Difference between two readings of a 32-bit performance counter.
 *
 * The counters wrap, and at Vita clock rates the cycle counter wraps every
 * few seconds, so a plain subtraction on widened values would produce a
 * huge negative spike once per wrap. Truncated 32-bit arithmetic gives the
 * right delta as long as fewer than 2^32 events passed between reads. */
uint32_t vita_tracy_pmu_delta(uint32_t previous, uint32_t current);

/* Programs the given event codes onto consecutive counters of the calling
 * thread and starts them.
 *
 * The number of usable counters is discovered by programming them one at a
 * time and stopping at the first rejection, rather than assuming a count
 * that may differ by firmware. Returns how many were programmed. */
uint32_t vita_tracy_pmu_begin(const uint8_t *event_codes, uint32_t count);

/* Reads the counters and emits one Tracy plot per configured event,
 * carrying the delta since the previous call. */
void vita_tracy_pmu_sample(void);

void vita_tracy_pmu_end(void);

#ifdef __cplusplus
}
#endif
