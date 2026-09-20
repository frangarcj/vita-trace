#include "vita_tracy/pmu_core.h"

/* Only called on the owning core with local interrupts disabled. No reset
 * commands or user-access enable operations are part of this interface. */
static uint32_t read_register(void *context, VitaPmuRegister reg) {
    (void)context;
    uint32_t value = 0;
    switch (reg) {
    case VITA_PMU_PMCR: __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(value)); break;
    case VITA_PMU_CNTEN: __asm__ volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(value)); break;
    case VITA_PMU_INTEN: __asm__ volatile("mrc p15, 0, %0, c9, c14, 1" : "=r"(value)); break;
    case VITA_PMU_OVSR: __asm__ volatile("mrc p15, 0, %0, c9, c12, 3" : "=r"(value)); break;
    case VITA_PMU_SELR: __asm__ volatile("mrc p15, 0, %0, c9, c12, 5" : "=r"(value)); break;
    case VITA_PMU_CYCLES: __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(value)); break;
    case VITA_PMU_TYPE: __asm__ volatile("mrc p15, 0, %0, c9, c13, 1" : "=r"(value)); break;
    case VITA_PMU_VALUE: __asm__ volatile("mrc p15, 0, %0, c9, c13, 2" : "=r"(value)); break;
    default: break;
    }
    return value;
}

static void write_register(void *context, VitaPmuRegister reg, uint32_t value) {
    (void)context;
    switch (reg) {
    case VITA_PMU_PMCR: __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(value) : "memory"); break;
    case VITA_PMU_CNTEN: __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(value) : "memory"); break;
    case VITA_PMU_CNTCLR: __asm__ volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(value) : "memory"); break;
    case VITA_PMU_INTEN: __asm__ volatile("mcr p15, 0, %0, c9, c14, 1" :: "r"(value) : "memory"); break;
    case VITA_PMU_INTCLR: __asm__ volatile("mcr p15, 0, %0, c9, c14, 2" :: "r"(value) : "memory"); break;
    case VITA_PMU_OVSR: __asm__ volatile("mcr p15, 0, %0, c9, c12, 3" :: "r"(value) : "memory"); break;
    case VITA_PMU_SELR: __asm__ volatile("mcr p15, 0, %0, c9, c12, 5" :: "r"(value) : "memory"); break;
    case VITA_PMU_CYCLES: __asm__ volatile("mcr p15, 0, %0, c9, c13, 0" :: "r"(value) : "memory"); break;
    case VITA_PMU_TYPE: __asm__ volatile("mcr p15, 0, %0, c9, c13, 1" :: "r"(value) : "memory"); break;
    case VITA_PMU_VALUE: __asm__ volatile("mcr p15, 0, %0, c9, c13, 2" :: "r"(value) : "memory"); break;
    default: break;
    }
    /* In particular, PMSELR must take effect before accessing PMXEV*. */
    __asm__ volatile("isb" ::: "memory");
}

const VitaPmuIo *vita_tracy_pmu_io(void) {
    static const VitaPmuIo io = {0, read_register, write_register};
    return &io;
}
