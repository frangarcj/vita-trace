#pragma once

#include <stdint.h>

#include "vita_tracy/pmu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VitaPmuOverflow {
    uint32_t acquired;
    uint32_t preload;
    uint32_t saved_pmcr;
    uint32_t saved_cycles;
} VitaPmuOverflow;

/* Takes exclusive ownership of the cycle counter and its overflow interrupt.
 * Existing counter/interrupt users are refused rather than reprogrammed. */
int vita_pmu_overflow_acquire(VitaPmuOverflow *overflow, const VitaPmuIo *io,
                              uint32_t period_cycles);

/* Returns 1 when this IRQ was caused by the owned cycle-counter overflow,
 * 0 for an unrelated IRQ, or a negative ownership/argument error. */
int vita_pmu_overflow_service(VitaPmuOverflow *overflow, const VitaPmuIo *io);

/* Restores the pre-session cycle count and PMCR only while ownership is
 * still observable. Another owner's changed PMU is never overwritten. */
int vita_pmu_overflow_release(VitaPmuOverflow *overflow, const VitaPmuIo *io);

#ifdef __cplusplus
}
#endif
