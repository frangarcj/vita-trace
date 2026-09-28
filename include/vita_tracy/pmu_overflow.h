#pragma once

#include <stdint.h>

#include "vita_tracy/pmu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VitaPmuOverflow {
    uint32_t acquired;
    uint32_t armed;
    uint32_t preload;
    uint32_t saved_pmcr;
    uint32_t saved_cycles;
} VitaPmuOverflow;

/* Configures the cycle counter but leaves both count and interrupt disabled.
 * This lets a platform install its IRQ handler only after every requested
 * core has proved that its PMU is available. */
int vita_pmu_overflow_prepare(VitaPmuOverflow *overflow, const VitaPmuIo *io,
                              uint32_t period_cycles);
int vita_pmu_overflow_arm(VitaPmuOverflow *overflow, const VitaPmuIo *io);

/* Takes exclusive ownership of the cycle counter and its overflow interrupt.
 * Convenience wrapper around prepare + arm. */
int vita_pmu_overflow_acquire(VitaPmuOverflow *overflow, const VitaPmuIo *io,
                              uint32_t period_cycles);

/* A counter found wrapped without a pending overflow: reloaded; the wrap
 * was less than one period ago (still a valid sample) or longer (stale). */
#define VITA_PMU_OVERFLOW_MISSED_RECENT 2
#define VITA_PMU_OVERFLOW_MISSED_STALE 3

/* Returns 1 when this IRQ was caused by the owned cycle-counter overflow,
 * a VITA_PMU_OVERFLOW_MISSED_* code after recovering a lost one, 0 for an
 * unrelated IRQ, or a negative ownership/argument error. */
int vita_pmu_overflow_service(VitaPmuOverflow *overflow, const VitaPmuIo *io);

/* Restores the pre-session cycle count and PMCR only while ownership is
 * still observable. Another owner's changed PMU is never overwritten. */
int vita_pmu_overflow_release(VitaPmuOverflow *overflow, const VitaPmuIo *io);

#ifdef __cplusplus
}
#endif
