#include <doctest/doctest.h>
#include "vita_tracy/thread_registry.h"

TEST_CASE("only explicitly registered profiler PUIDs are excluded") {
    VitaTraceThreadRegistry registry{};
    CHECK(vita_trace_thread_add(&registry, 101));
    CHECK(vita_trace_thread_contains(&registry, 101));
    CHECK_FALSE(vita_trace_thread_contains(&registry, 102));
    CHECK_FALSE(vita_trace_thread_contains(&registry, 0));
    CHECK(vita_trace_thread_registry_complete(&registry));
}

TEST_CASE("duplicate registration does not consume another slot") {
    VitaTraceThreadRegistry registry{};
    for (unsigned i = 0; i < 100; ++i) CHECK(vita_trace_thread_add(&registry, 17));
    vita_trace_thread_remove(&registry, 17);
    CHECK_FALSE(vita_trace_thread_contains(&registry, 17));
    CHECK(vita_trace_thread_registry_complete(&registry));
}

TEST_CASE("worker exit removes its ID before it can be reused by application code") {
    VitaTraceThreadRegistry registry{};
    REQUIRE(vita_trace_thread_add(&registry, 27));
    vita_trace_thread_remove(&registry, 27);
    CHECK_FALSE(vita_trace_thread_contains(&registry, 27));
    CHECK(vita_trace_thread_add(&registry, 28));
    CHECK(vita_trace_thread_contains(&registry, 28));
}

TEST_CASE("a full exclusion registry fails closed rather than hiding overflow") {
    VitaTraceThreadRegistry registry{};
    for (uint32_t i = 1; i <= VITA_TRACE_MAX_PROFILER_THREADS; ++i)
        REQUIRE(vita_trace_thread_add(&registry, i));
    CHECK_FALSE(vita_trace_thread_add(&registry, 100));
    CHECK_FALSE(vita_trace_thread_registry_complete(&registry));
    vita_trace_thread_remove(&registry, 1);
    CHECK(vita_trace_thread_add(&registry, 101));
    CHECK_FALSE(vita_trace_thread_registry_complete(&registry));
}

TEST_CASE("zero and unknown IDs do not alter an exclusion registry") {
    VitaTraceThreadRegistry registry{};
    CHECK_FALSE(vita_trace_thread_add(&registry, 0));
    REQUIRE(vita_trace_thread_add(&registry, 1));
    vita_trace_thread_remove(&registry, 0);
    vita_trace_thread_remove(&registry, 500);
    CHECK(vita_trace_thread_contains(&registry, 1));
    CHECK(vita_trace_thread_registry_complete(&registry));
}
