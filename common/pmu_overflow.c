#include "vita_tracy/pmu_overflow.h"

#include <string.h>

#define PMCR_RESETS 6u
#define PMCR_CONTROL 0x39u

static uint32_t read_reg(const VitaPmuIo *io, VitaPmuRegister reg) {
    return io->read(io->context, reg);
}

static void write_reg(const VitaPmuIo *io, VitaPmuRegister reg, uint32_t value) {
    io->write(io->context, reg, value);
}

static int owns_registers(const VitaPmuOverflow *overflow, const VitaPmuIo *io) {
    if ((read_reg(io, VITA_PMU_PMCR) & PMCR_CONTROL) != 1u) return 0;
    if (overflow->armed)
        return read_reg(io, VITA_PMU_CNTEN) == VITA_PMU_CYCLE_BIT &&
               read_reg(io, VITA_PMU_INTEN) == VITA_PMU_CYCLE_BIT;
    return read_reg(io, VITA_PMU_CNTEN) == 0 && read_reg(io, VITA_PMU_INTEN) == 0;
}

int vita_pmu_overflow_prepare(VitaPmuOverflow *overflow, const VitaPmuIo *io,
                              uint32_t period_cycles) {
    if (!overflow || !io || !io->read || !io->write || period_cycles == 0)
        return VITA_PMU_ERROR_ARGS;
    if (overflow->acquired) return VITA_PMU_ERROR_BUSY;

    const uint32_t pmcr = read_reg(io, VITA_PMU_PMCR);
    if (read_reg(io, VITA_PMU_CNTEN) || read_reg(io, VITA_PMU_INTEN) ||
        (read_reg(io, VITA_PMU_OVSR) & VITA_PMU_CYCLE_BIT))
        return VITA_PMU_ERROR_BUSY;

    memset(overflow, 0, sizeof(*overflow));
    overflow->saved_pmcr = pmcr & ~PMCR_RESETS;
    overflow->saved_cycles = read_reg(io, VITA_PMU_CYCLES);
    overflow->preload = 0u - period_cycles;

    /* Run the cycle counter undivided. Nothing counts until arm(). */
    write_reg(io, VITA_PMU_PMCR, (overflow->saved_pmcr & ~PMCR_CONTROL) | 1u);
    write_reg(io, VITA_PMU_CYCLES, overflow->preload);
    overflow->acquired = 1;
    return 0;
}

int vita_pmu_overflow_arm(VitaPmuOverflow *overflow, const VitaPmuIo *io) {
    if (!overflow || !overflow->acquired || !io || !io->read || !io->write)
        return VITA_PMU_ERROR_ARGS;
    if (overflow->armed) return 0;
    if (!owns_registers(overflow, io)) {
        overflow->acquired = 0;
        return VITA_PMU_ERROR_OWNERSHIP;
    }
    write_reg(io, VITA_PMU_INTEN, VITA_PMU_CYCLE_BIT);
    write_reg(io, VITA_PMU_CNTEN, VITA_PMU_CYCLE_BIT);
    overflow->armed = 1;
    return 0;
}

int vita_pmu_overflow_acquire(VitaPmuOverflow *overflow, const VitaPmuIo *io,
                              uint32_t period_cycles) {
    int ret = vita_pmu_overflow_prepare(overflow, io, period_cycles);
    return ret < 0 ? ret : vita_pmu_overflow_arm(overflow, io);
}

int vita_pmu_overflow_service(VitaPmuOverflow *overflow, const VitaPmuIo *io) {
    if (!overflow || !overflow->acquired || !overflow->armed || !io || !io->read || !io->write)
        return VITA_PMU_ERROR_ARGS;
    /* This observer sees every IRQ while active. PMOVSR is the cheap source
     * discriminator; only an actual cycle overflow pays the ownership checks. */
    if (!(read_reg(io, VITA_PMU_OVSR) & VITA_PMU_CYCLE_BIT)) return 0;
    /* The firmware disables every counter on IRQ entry and around thread
     * switches and re-enables them from the resumed thread's context. A
     * pending overflow observed while the counters are off is not a loss of
     * ownership; leave it for an entry where the cycle counter is enabled. */
    if (!(read_reg(io, VITA_PMU_CNTEN) & VITA_PMU_CYCLE_BIT)) return 0;
    if (!owns_registers(overflow, io)) {
        overflow->acquired = 0;
        return VITA_PMU_ERROR_OWNERSHIP;
    }

    /* PMOVSR is W1C. Reload after clearing so the next period starts at this
     * handler rather than accumulating an unknown amount of handler latency. */
    write_reg(io, VITA_PMU_OVSR, VITA_PMU_CYCLE_BIT);
    write_reg(io, VITA_PMU_CYCLES, overflow->preload);
    return 1;
}

int vita_pmu_overflow_release(VitaPmuOverflow *overflow, const VitaPmuIo *io) {
    if (!overflow || !io || !io->read || !io->write) return VITA_PMU_ERROR_ARGS;
    if (!overflow->acquired) return 0;
    if (!owns_registers(overflow, io)) {
        overflow->acquired = 0;
        return VITA_PMU_ERROR_OWNERSHIP;
    }

    if (overflow->armed) {
        write_reg(io, VITA_PMU_INTCLR, VITA_PMU_CYCLE_BIT);
        write_reg(io, VITA_PMU_CNTCLR, VITA_PMU_CYCLE_BIT);
    }
    if (read_reg(io, VITA_PMU_OVSR) & VITA_PMU_CYCLE_BIT)
        write_reg(io, VITA_PMU_OVSR, VITA_PMU_CYCLE_BIT);
    write_reg(io, VITA_PMU_CYCLES, overflow->saved_cycles);
    write_reg(io, VITA_PMU_PMCR, overflow->saved_pmcr);
    overflow->armed = 0;
    overflow->acquired = 0;
    return 0;
}
