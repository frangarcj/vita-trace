#include "vita_tracy/pmu_core.h"
#include <string.h>

#define PMCR_RESETS 6u
#define PMCR_CONTROL 0x39u /* E, D, X, DP; P/C are commands, not saved state. */

static uint32_t read_reg(const VitaPmuIo *io, VitaPmuRegister reg) {
    return io->read(io->context, reg);
}
static void write_reg(const VitaPmuIo *io, VitaPmuRegister reg, uint32_t value) {
    io->write(io->context, reg, value);
}

int vita_pmu_plan_valid(const VitaPmuPlan *plan, uint32_t available) {
    if (!plan || plan->count > VITA_PMU_EVENTS || available > 31u) return 0;
    uint32_t used = 0;
    for (uint32_t i = 0; i < plan->count; ++i) {
        uint32_t slot = plan->counters[i];
        if (slot >= available || slot >= VITA_PMU_EVENTS || plan->events[i] > 255u) return 0;
        if (used & (1u << slot)) return 0;
        used |= 1u << slot;
    }
    return 1;
}

static int owns_registers(const VitaPmuCore *core, const VitaPmuIo *io) {
    if ((read_reg(io, VITA_PMU_PMCR) & PMCR_CONTROL) != 1u ||
        read_reg(io, VITA_PMU_CNTEN) != core->mask || read_reg(io, VITA_PMU_INTEN)) return 0;
    uint32_t selector = read_reg(io, VITA_PMU_SELR);
    int owned = 1;
    for (uint32_t i = 0; i < core->plan.count; ++i) {
        write_reg(io, VITA_PMU_SELR, core->plan.counters[i]);
        if (read_reg(io, VITA_PMU_TYPE) != core->plan.events[i]) owned = 0;
    }
    write_reg(io, VITA_PMU_SELR, selector);
    return owned;
}

int vita_pmu_acquire(VitaPmuCore *core, const VitaPmuIo *io, const VitaPmuPlan *plan) {
    if (!core || !io || !io->read || !io->write || !vita_pmu_plan_valid(plan, VITA_PMU_EVENTS))
        return VITA_PMU_ERROR_ARGS;
    if (core->acquired) return VITA_PMU_ERROR_BUSY;
    uint32_t pmcr = read_reg(io, VITA_PMU_PMCR);
    if (!vita_pmu_plan_valid(plan, (pmcr >> 11) & 31u)) return VITA_PMU_ERROR_ARGS;
    if (read_reg(io, VITA_PMU_CNTEN) || read_reg(io, VITA_PMU_INTEN)) return VITA_PMU_ERROR_BUSY;

    memset(core, 0, sizeof(*core));
    core->plan = *plan;
    core->saved_pmcr = pmcr & ~PMCR_RESETS;
    core->saved_select = read_reg(io, VITA_PMU_SELR);
    core->saved_overflow = read_reg(io, VITA_PMU_OVSR);
    core->saved_cycles = read_reg(io, VITA_PMU_CYCLES);
    core->mask = VITA_PMU_CYCLE_BIT;
    for (uint32_t i = 0; i < plan->count; ++i) {
        write_reg(io, VITA_PMU_SELR, plan->counters[i]);
        core->saved_types[i] = read_reg(io, VITA_PMU_TYPE);
        core->saved_values[i] = read_reg(io, VITA_PMU_VALUE);
        write_reg(io, VITA_PMU_TYPE, plan->events[i]);
        core->mask |= 1u << plan->counters[i];
    }
    write_reg(io, VITA_PMU_SELR, core->saved_select);
    write_reg(io, VITA_PMU_PMCR, (core->saved_pmcr & ~PMCR_CONTROL) | 1u);
    write_reg(io, VITA_PMU_CNTEN, core->mask);
    core->acquired = 1;
    return 0;
}

int vita_pmu_read(VitaPmuCore *core, const VitaPmuIo *io, uint64_t now, VitaPmuDelta *delta) {
    if (!core || !core->acquired || !io || !delta) return VITA_PMU_ERROR_ARGS;
    if (!owns_registers(core, io)) return VITA_PMU_ERROR_OWNERSHIP;
    uint32_t values[VITA_PMU_EVENTS + 1] = {0};
    uint32_t selector = read_reg(io, VITA_PMU_SELR);
    values[0] = read_reg(io, VITA_PMU_CYCLES);
    for (uint32_t i = 0; i < core->plan.count; ++i) {
        write_reg(io, VITA_PMU_SELR, core->plan.counters[i]);
        values[i + 1] = read_reg(io, VITA_PMU_VALUE);
    }
    write_reg(io, VITA_PMU_SELR, selector);
    memset(delta, 0, sizeof(*delta));
    delta->timestamp = now;
    int result = core->have_previous != 0;
    if (result) {
        uint64_t elapsed = now - core->previous_time;
        if (now <= core->previous_time || elapsed > VITA_PMU_MAX_INTERVAL_US) {
            delta->flags = VITA_PMU_DELTA_GAP;
        } else {
            delta->elapsed_us = (uint32_t)elapsed;
            delta->cycles = values[0] - core->previous[0];
            for (uint32_t i = 0; i < core->plan.count; ++i)
                delta->values[i] = values[i + 1] - core->previous[i + 1];
        }
    }
    memcpy(core->previous, values, sizeof(values));
    core->previous_time = now;
    core->have_previous = 1;
    return result;
}

int vita_pmu_release(VitaPmuCore *core, const VitaPmuIo *io) {
    if (!core || !io) return VITA_PMU_ERROR_ARGS;
    if (!core->acquired) return 0;
    if (!owns_registers(core, io)) {
        /* Do not destroy someone else's new PMU configuration on detach. */
        core->acquired = 0;
        return VITA_PMU_ERROR_OWNERSHIP;
    }
    write_reg(io, VITA_PMU_CNTCLR, core->mask);
    write_reg(io, VITA_PMU_CYCLES, core->saved_cycles);
    for (uint32_t i = 0; i < core->plan.count; ++i) {
        write_reg(io, VITA_PMU_SELR, core->plan.counters[i]);
        write_reg(io, VITA_PMU_TYPE, core->saved_types[i]);
        write_reg(io, VITA_PMU_VALUE, core->saved_values[i]);
    }
    /* PMOVSR is W1C. Never clear a flag that existed before our session. */
    write_reg(io, VITA_PMU_OVSR, (read_reg(io, VITA_PMU_OVSR) & core->mask) & ~core->saved_overflow);
    write_reg(io, VITA_PMU_SELR, core->saved_select);
    write_reg(io, VITA_PMU_PMCR, core->saved_pmcr);
    core->acquired = 0;
    return 0;
}
