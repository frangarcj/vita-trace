#include "vita_tracy/timebase.h"

/* A timebase slower than this cannot resolve the events a profiler cares
 * about, and one faster than this is beyond anything the hardware runs at,
 * so either means the reported value is not a frequency at all. */
#define VITA_TRACE_TIMEBASE_HZ_MIN 1000u
#define VITA_TRACE_TIMEBASE_HZ_MAX 1000000000u

int vita_trace_timebase_hz_is_plausible(uint32_t hz) {
    return hz >= VITA_TRACE_TIMEBASE_HZ_MIN && hz <= VITA_TRACE_TIMEBASE_HZ_MAX;
}

uint32_t vita_trace_timebase_calibrate(uint64_t ticks_elapsed, uint64_t micros_elapsed) {
    if (ticks_elapsed == 0 || micros_elapsed == 0) {
        return 0;
    }

    /* Split rather than ticks * 1000000: a measurement window of a second or
     * more would overflow the product at these tick rates. */
    uint64_t whole = ticks_elapsed / micros_elapsed;
    uint64_t remainder = ticks_elapsed % micros_elapsed;
    uint64_t hz = whole * 1000000ull + (remainder * 1000000ull) / micros_elapsed;

    if (hz > 0xFFFFFFFFull) {
        return 0;
    }
    return (uint32_t)hz;
}

uint64_t vita_trace_ticks_to_ns(uint64_t ticks, uint32_t hz) {
    if (hz == 0) {
        return 0;
    }

    uint64_t seconds = ticks / hz;
    uint64_t remainder = ticks % hz;
    return seconds * 1000000000ull + (remainder * 1000000000ull) / hz;
}
