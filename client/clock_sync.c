#include "vita_tracy/clock_sync.h"

void vita_tracy_clock_sync_set(VitaTracyClockSync *sync, int64_t tracy_before_ns,
                               int64_t tracy_after_ns, uint64_t kernel_us) {
    if (sync == NULL) {
        return;
    }
    /* Halving the span rather than the sum keeps the midpoint exact for
     * timestamps large enough that the sum would overflow. */
    sync->tracy_ref_ns = tracy_before_ns + (tracy_after_ns - tracy_before_ns) / 2;
    sync->kernel_ref_us = kernel_us;
}

int64_t vita_tracy_kernel_us_to_tracy_ns(const VitaTracyClockSync *sync, uint64_t kernel_us) {
    if (sync == NULL) {
        return 0;
    }
    int64_t delta_us = (int64_t)(kernel_us - sync->kernel_ref_us);
    return sync->tracy_ref_ns + delta_us * 1000ll;
}
