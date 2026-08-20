#include "internal.h"
#include "vita_tracy/kernel_abi.h"

/* Placeholder for the non-intrusive sampler.
 *
 * ScePamgr exposes a trace buffer plus ARM, counter and GPU trace sources,
 * which is the most likely path to PC samples that do not require stopping
 * the target. What that buffer contains, whether it identifies CPU and
 * thread, what it costs, and whether retail firmware permits it at all are
 * all unknown; the Vita3K checkout implements none of it, so the questions
 * can only be settled on a CEX. Anexo B of the design lists the experiments
 * that decide between this path and a kernel hook.
 *
 * Reporting unsupported here keeps the caller on the bring-up sampler
 * instead of silently producing an empty profile. */

int vita_tracy_sampler_pamgr_start(VitaTracyKernelState *st) {
    (void)st;
    return VITA_TRACY_ERROR_UNSUPPORTED;
}

void vita_tracy_sampler_pamgr_stop(VitaTracyKernelState *st) {
    (void)st;
}
