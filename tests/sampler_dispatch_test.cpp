#include <doctest/doctest.h>
extern "C" {
#include "internal.h"
#include "vita_tracy/kernel_abi.h"
}

namespace {
struct Fake {
    int start_result = 0, stop_result = 0;
    int irq_starts = 0, irq_stops = 0, diagnostic_starts = 0, diagnostic_stops = 0;
    int pamgr_result = VITA_TRACY_ERROR_UNSUPPORTED;
} fake;
struct Fixture {
    VitaTracyKernelState state{};
    Fixture() { fake = {}; state.state = VITA_TRACY_STATE_STOPPED; }
};
}
extern "C" {
int vita_tracy_sampler_irq_start(VitaTracyKernelState *) { ++fake.irq_starts; return fake.start_result; }
int vita_tracy_sampler_irq_stop(VitaTracyKernelState *) { ++fake.irq_stops; return fake.stop_result; }
int vita_tracy_sampler_diagnostic_start(VitaTracyKernelState *) { ++fake.diagnostic_starts; return fake.start_result; }
int vita_tracy_sampler_diagnostic_stop(VitaTracyKernelState *) { ++fake.diagnostic_stops; return fake.stop_result; }
int vita_tracy_sampler_pamgr_start(VitaTracyKernelState *) { return fake.pamgr_result; }
void vita_tracy_sampler_pamgr_stop(VitaTracyKernelState *) {}
}

TEST_CASE_FIXTURE(Fixture, "failed IRQ startup remains routed to its cleanup while logically stopped") {
    state.sampling_flags = VITA_TRACY_SAMPLING_PMU_IRQ;
    fake.start_result = -55;
    CHECK(vita_tracy_sampler_start(&state) == -55);
    CHECK(state.sampler_backend == VITA_TRACY_SAMPLER_PMU_IRQ);
    CHECK(state.state == VITA_TRACY_STATE_STOPPED);
    fake.stop_result = -66;
    CHECK(vita_tracy_sampler_stop(&state) == -66);
    CHECK(fake.irq_stops == 1);
    CHECK(state.sampler_backend == VITA_TRACY_SAMPLER_PMU_IRQ);
    CHECK(vita_tracy_sampler_start(&state) == VITA_TRACY_ERROR_BUSY);
    CHECK(fake.irq_starts == 1);
    fake.stop_result = 0;
    CHECK(vita_tracy_sampler_stop(&state) == 0);
    CHECK(state.sampler_backend == VITA_TRACY_SAMPLER_NONE);
}

TEST_CASE_FIXTURE(Fixture, "failed diagnostic startup also keeps its timer cleanup reachable") {
    state.sampling_flags = VITA_TRACY_SAMPLING_ALLOW_SUSPEND;
    fake.start_result = -55;
    CHECK(vita_tracy_sampler_start(&state) == -55);
    CHECK(state.sampler_backend == VITA_TRACY_SAMPLER_SUSPEND);
    REQUIRE(vita_tracy_sampler_stop(&state) == 0);
    CHECK(fake.diagnostic_stops == 1);
    CHECK(fake.irq_stops == 0);
}

TEST_CASE_FIXTURE(Fixture, "ordinary sampling never silently selects the suspend diagnostic") {
    CHECK(vita_tracy_sampler_start(&state) == VITA_TRACY_ERROR_UNSUPPORTED);
    CHECK(fake.diagnostic_starts == 0);
    CHECK(fake.irq_starts == 0);
    CHECK(state.sampler_backend == VITA_TRACY_SAMPLER_NONE);
}

TEST_CASE_FIXTURE(Fixture, "a backend failure is not hidden by opting into the diagnostic") {
    fake.pamgr_result = VITA_TRACY_ERROR_MAP;
    state.sampling_flags = VITA_TRACY_SAMPLING_ALLOW_SUSPEND;
    CHECK(vita_tracy_sampler_start(&state) == VITA_TRACY_ERROR_MAP);
    CHECK(fake.diagnostic_starts == 0);
}

TEST_CASE_FIXTURE(Fixture, "successful IRQ stop permits a different backend and redundant stop is harmless") {
    state.sampling_flags = VITA_TRACY_SAMPLING_PMU_IRQ;
    REQUIRE(vita_tracy_sampler_start(&state) == 0);
    REQUIRE(vita_tracy_sampler_stop(&state) == 0);
    REQUIRE(vita_tracy_sampler_stop(&state) == 0);
    CHECK(fake.irq_stops == 1);
    state.sampling_flags = VITA_TRACY_SAMPLING_ALLOW_SUSPEND;
    REQUIRE(vita_tracy_sampler_start(&state) == 0);
    CHECK(fake.diagnostic_starts == 1);
    REQUIRE(vita_tracy_sampler_stop(&state) == 0);
    CHECK(fake.diagnostic_stops == 1);
}
