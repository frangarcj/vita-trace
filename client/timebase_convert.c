#include "vita_tracy/timebase.h"

uint64_t vita_trace_ticks_to_ns(uint64_t ticks, uint32_t hz) {
    if (hz == 0) {
        return 0;
    }

    uint64_t seconds = ticks / hz;
    uint64_t remainder = ticks % hz;
    return seconds * 1000000000ull + (remainder * 1000000000ull) / hz;
}
