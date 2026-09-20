#include "internal.h"
#include "vita_tracy/kernel_abi.h"

int vita_tracy_sampler_start(VitaTracyKernelState *st) {
    if (st->sampler_backend != VITA_TRACY_SAMPLER_NONE) return VITA_TRACY_ERROR_BUSY;
    if (st->sampling_flags & VITA_TRACY_SAMPLING_PMU_IRQ) {
        /* Record ownership BEFORE startup: failed rollback still needs stop. */
        st->sampler_backend = VITA_TRACY_SAMPLER_PMU_IRQ;
        return vita_tracy_sampler_irq_start(st);
    }

    int ret = vita_tracy_sampler_pamgr_start(st);
    if (ret == VITA_TRACY_OK) {
        st->sampler_backend = VITA_TRACY_SAMPLER_PAMGR;
        return ret;
    }
    /* The present pamgr placeholder owns no resources. Do not hide a future
     * real backend failure by silently switching to thread suspension. */
    if (ret != VITA_TRACY_ERROR_UNSUPPORTED) return ret;
    if (!(st->sampling_flags & VITA_TRACY_SAMPLING_ALLOW_SUSPEND)) return ret;
    st->sampler_backend = VITA_TRACY_SAMPLER_SUSPEND;
    return vita_tracy_sampler_diagnostic_start(st);
}

int vita_tracy_sampler_stop(VitaTracyKernelState *st) {
    int ret;
    switch (st->sampler_backend) {
    case VITA_TRACY_SAMPLER_NONE:
        return VITA_TRACY_OK;
    case VITA_TRACY_SAMPLER_PMU_IRQ:
        ret = vita_tracy_sampler_irq_stop(st);
        break;
    case VITA_TRACY_SAMPLER_SUSPEND:
        ret = vita_tracy_sampler_diagnostic_stop(st);
        break;
    case VITA_TRACY_SAMPLER_PAMGR:
        vita_tracy_sampler_pamgr_stop(st);
        ret = VITA_TRACY_OK;
        break;
    default:
        return VITA_TRACY_ERROR_STATE;
    }
    if (ret >= 0) st->sampler_backend = VITA_TRACY_SAMPLER_NONE;
    return ret;
}
