#include <doctest/doctest.h>

#include "vita_tracy/pmu.h"

TEST_CASE("a simple increase is the plain difference") {
    CHECK(vita_tracy_pmu_delta(100, 350) == 250);
}

TEST_CASE("no change between reads is a zero delta") {
    CHECK(vita_tracy_pmu_delta(4242, 4242) == 0);
}

TEST_CASE("a counter that wrapped past zero still reports forward progress") {
    // The cycle counter wraps every few seconds on this hardware; a widened
    // subtraction would report a large negative spike at each wrap.
    CHECK(vita_tracy_pmu_delta(0xFFFFFFF0u, 0x0000000Fu) == 31u);
}

TEST_CASE("a wrap of exactly one step is one") {
    CHECK(vita_tracy_pmu_delta(0xFFFFFFFFu, 0x00000000u) == 1u);
}

TEST_CASE("starting from zero counts up normally") {
    CHECK(vita_tracy_pmu_delta(0, 1000) == 1000);
}

TEST_CASE("the largest representable delta is handled") {
    CHECK(vita_tracy_pmu_delta(1, 0) == 0xFFFFFFFFu);
}
