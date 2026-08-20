#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include "vita_tracy/timebase.h"

extern "C" {

uint32_t PlatformGetThreadId(void) {
    return (uint32_t)sceKernelGetThreadId();
}

uint32_t vita_tracy_timebase_frequency(void) {
    static uint32_t cached_hz = 0;
    if (cached_hz == 0) {
        cached_hz = scePerfGetTimebaseFrequency();
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
