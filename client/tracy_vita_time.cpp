#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include "vita_tracy/timebase.h"

extern "C" {

uint32_t PlatformGetThreadId(void) {
    return (uint32_t)sceKernelGetThreadId();
}

/* Measures the timebase against the process clock, which is documented in
 * microseconds and does not depend on ScePamgr. */
static uint32_t MeasureTimebaseFrequency(void) {
    uint64_t ticks_before = scePerfGetTimebaseValue();
    SceUInt64 micros_before = sceKernelGetProcessTimeWide();

    sceKernelDelayThread(20000);

    uint64_t ticks_after = scePerfGetTimebaseValue();
    SceUInt64 micros_after = sceKernelGetProcessTimeWide();

    return vita_trace_timebase_calibrate(ticks_after - ticks_before,
                                         (uint64_t)(micros_after - micros_before));
}

uint32_t vita_tracy_timebase_frequency(void) {
    static uint32_t cached_hz = 0;
    if (cached_hz != 0) {
        return cached_hz;
    }

    /* Only believed when it looks like a frequency: on retail firmware this
     * call reaches an unresolved ScePamgr import and returns -1. */
    uint32_t reported = scePerfGetTimebaseFrequency();
    if (vita_trace_timebase_hz_is_plausible(reported)) {
        cached_hz = reported;
        return cached_hz;
    }

    uint32_t measured = MeasureTimebaseFrequency();
    if (vita_trace_timebase_hz_is_plausible(measured)) {
        cached_hz = measured;
    }
    return cached_hz;
}

uint64_t vita_tracy_timebase_value(void) {
    return scePerfGetTimebaseValue();
}

/* Tracy consumes timestamps in nanoseconds when it has no hardware timer of
 * its own, so the raw timebase is converted here rather than reported with a
 * multiplier. */
int64_t tracy_vita_get_time(void) {
    return (int64_t)vita_trace_ticks_to_ns(scePerfGetTimebaseValue(), vita_tracy_timebase_frequency());
}

} // extern "C"
