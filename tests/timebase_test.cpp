#include <doctest/doctest.h>

#include "vita_tracy/timebase.h"

namespace {
/* Representative Vita timebase; the real value is read from
 * scePerfGetTimebaseFrequency() at runtime. */
constexpr uint32_t kVitaHz = 40961000u;
} // namespace

TEST_CASE("zero ticks is zero nanoseconds") {
    CHECK(vita_trace_ticks_to_ns(0, kVitaHz) == 0);
}

TEST_CASE("a zero frequency yields zero instead of dividing by zero") {
    CHECK(vita_trace_ticks_to_ns(12345, 0) == 0);
}

TEST_CASE("exactly one second of ticks is one billion nanoseconds") {
    CHECK(vita_trace_ticks_to_ns(kVitaHz, kVitaHz) == 1000000000ull);
}

TEST_CASE("whole seconds scale linearly") {
    CHECK(vita_trace_ticks_to_ns(kVitaHz * 5ull, kVitaHz) == 5000000000ull);
}

TEST_CASE("sub-second remainders are not truncated away") {
    // Half a second of ticks must land near 5e8 ns, not at 0.
    uint64_t ns = vita_trace_ticks_to_ns(kVitaHz / 2, kVitaHz);
    CHECK(ns > 499999000ull);
    CHECK(ns < 500001000ull);
}

TEST_CASE("a simple round frequency converts exactly") {
    // 1 MHz timebase: 1 tick == 1000 ns.
    CHECK(vita_trace_ticks_to_ns(1, 1000000u) == 1000ull);
    CHECK(vita_trace_ticks_to_ns(1500, 1000000u) == 1500000ull);
}

TEST_CASE("long sessions do not overflow") {
    // A naive (ticks * 1e9) / hz overflows uint64 past roughly 1.8e10 ticks,
    // which at the Vita timebase is about 7.5 minutes of capture. One hour
    // must still convert to exactly 3600 seconds.
    uint64_t one_hour_ticks = (uint64_t)kVitaHz * 3600ull;
    CHECK(vita_trace_ticks_to_ns(one_hour_ticks, kVitaHz) == 3600ull * 1000000000ull);
}

TEST_CASE("conversion stays monotonic across the overflow threshold") {
    uint64_t base = 20000000000ull; // past where the naive product wraps
    uint64_t a = vita_trace_ticks_to_ns(base, kVitaHz);
    uint64_t b = vita_trace_ticks_to_ns(base + kVitaHz, kVitaHz);
    CHECK(b > a);
    CHECK(b - a == 1000000000ull);
}
