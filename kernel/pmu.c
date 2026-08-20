#include "internal.h"
#include "vita_tracy/kernel_abi.h"

/* ScePerf's ARM PMON counters are a userland library, so a kernel module
 * cannot import them; the client drives them per thread and feeds the
 * values straight to Tracy as plots, which needs no ring.
 *
 * This entry point stays in the ABI for the privileged path: reaching the
 * PMU from kernel means driving the CP15 performance monitor registers
 * directly, and whether Sony's scheduler saves and restores them per
 * thread, how many counters retail firmware leaves programmable, and
 * whether ScePamgr already owns them are open questions that need a CEX to
 * answer. Until then this reports unsupported rather than programming
 * counters whose behaviour under context switches is unknown. */

int vita_tracy_pmu_configure(VitaTracyKernelState *st, const VitaTracyPmuConfig *cfg) {
    (void)st;
    (void)cfg;
    return VITA_TRACY_ERROR_UNSUPPORTED;
}
