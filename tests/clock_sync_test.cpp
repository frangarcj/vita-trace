#include <doctest/doctest.h>

#include "vita_tracy/clock_sync.h"

namespace {

VitaTracyClockSync make_sync(int64_t before, int64_t after, uint64_t kernel_us) {
    VitaTracyClockSync sync{};
    vita_tracy_clock_sync_set(&sync, before, after, kernel_us);
    return sync;
}

} // namespace

TEST_CASE("the reference tracy time is the midpoint of the bracket") {
    auto sync = make_sync(1000, 2000, 500);
    CHECK(sync.tracy_ref_ns == 1500);
    CHECK(sync.kernel_ref_us == 500);
}

TEST_CASE("a bracket of zero width is its own midpoint") {
    auto sync = make_sync(4242, 4242, 7);
    CHECK(sync.tracy_ref_ns == 4242);
}

TEST_CASE("the midpoint does not overflow on large timestamps") {
    int64_t big = 9000000000000000000ll;
    auto sync = make_sync(big, big + 1000, 1);
    CHECK(sync.tracy_ref_ns == big + 500);
}

TEST_CASE("the reference kernel timestamp maps to the reference tracy time") {
    auto sync = make_sync(1000, 2000, 500);
    CHECK(vita_tracy_kernel_us_to_tracy_ns(&sync, 500) == 1500);
}

TEST_CASE("kernel microseconds scale to nanoseconds") {
    auto sync = make_sync(1000, 1000, 500);
    // 3 us after the reference is 3000 ns after it.
    CHECK(vita_tracy_kernel_us_to_tracy_ns(&sync, 503) == 1000 + 3000);
}

TEST_CASE("timestamps before the reference map earlier, not to a huge positive") {
    // kernel_us is unsigned; subtracting past the reference must not wrap.
    auto sync = make_sync(1000000, 1000000, 500);
    CHECK(vita_tracy_kernel_us_to_tracy_ns(&sync, 400) == 1000000 - 100000);
}

TEST_CASE("an event well before the reference can land before zero") {
    auto sync = make_sync(0, 0, 1000);
    CHECK(vita_tracy_kernel_us_to_tracy_ns(&sync, 0) == -1000000);
}

TEST_CASE("conversion stays monotonic and correctly spaced") {
    auto sync = make_sync(500, 500, 100);
    int64_t a = vita_tracy_kernel_us_to_tracy_ns(&sync, 1000);
    int64_t b = vita_tracy_kernel_us_to_tracy_ns(&sync, 2000);
    CHECK(b > a);
    CHECK(b - a == 1000000); // 1000 us apart
}

TEST_CASE("a long uptime converts without overflowing") {
    // Ten hours of uptime in microseconds, well past 32 bits.
    uint64_t ten_hours_us = 10ull * 3600ull * 1000000ull;
    auto sync = make_sync(0, 0, 0);
    CHECK(vita_tracy_kernel_us_to_tracy_ns(&sync, ten_hours_us) ==
          (int64_t)ten_hours_us * 1000ll);
}
