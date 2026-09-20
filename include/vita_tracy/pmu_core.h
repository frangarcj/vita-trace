#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_PMU_EVENTS 6u
#define VITA_PMU_CYCLE_BIT 0x80000000u
#define VITA_PMU_MAX_INTERVAL_US 1000000u
#define VITA_PMU_DELTA_GAP 1u
#define VITA_PMU_ERROR_ARGS (-1)
#define VITA_PMU_ERROR_BUSY (-2)
#define VITA_PMU_ERROR_OWNERSHIP (-3)

/* Logical register operations keep the ownership/delta algorithm host-testable.
 * The caller serializes access on the owning CPU and masks local interrupts. */
typedef enum VitaPmuRegister {
    VITA_PMU_PMCR, VITA_PMU_CNTEN, VITA_PMU_CNTCLR, VITA_PMU_INTEN, VITA_PMU_INTCLR,
    VITA_PMU_OVSR, VITA_PMU_SELR, VITA_PMU_CYCLES, VITA_PMU_TYPE, VITA_PMU_VALUE
} VitaPmuRegister;
typedef struct VitaPmuIo {
    void *context;
    uint32_t (*read)(void *context, VitaPmuRegister reg);
    void (*write)(void *context, VitaPmuRegister reg, uint32_t value);
} VitaPmuIo;
typedef struct VitaPmuPlan {
    uint32_t count;
    uint32_t counters[VITA_PMU_EVENTS];
    uint32_t events[VITA_PMU_EVENTS];
} VitaPmuPlan;
typedef struct VitaPmuDelta {
    uint64_t timestamp;
    uint32_t elapsed_us;
    uint32_t flags;
    uint32_t cycles;
    uint32_t values[VITA_PMU_EVENTS];
} VitaPmuDelta;
typedef struct VitaPmuCore {
    VitaPmuPlan plan;
    uint32_t acquired, have_previous, mask;
    uint32_t saved_pmcr, saved_select, saved_overflow, saved_cycles;
    uint32_t saved_types[VITA_PMU_EVENTS], saved_values[VITA_PMU_EVENTS];
    uint32_t previous[VITA_PMU_EVENTS + 1];
    uint64_t previous_time;
} VitaPmuCore;

int vita_pmu_plan_valid(const VitaPmuPlan *plan, uint32_t available);
/* Refuses any existing enabled counter or PMU interrupt. Never opens PL0 access. */
int vita_pmu_acquire(VitaPmuCore *core, const VitaPmuIo *io, const VitaPmuPlan *plan);
/* Returns 1 for a record, 0 for the initial baseline, or a negative error.
 * A gap record has zero deltas; it must not be interpreted as zero CPU usage. */
int vita_pmu_read(VitaPmuCore *core, const VitaPmuIo *io, uint64_t now, VitaPmuDelta *delta);
/* Returns OWNERSHIP without overwriting a configuration changed by someone else. */
int vita_pmu_release(VitaPmuCore *core, const VitaPmuIo *io);

#ifdef __cplusplus
}
#endif
