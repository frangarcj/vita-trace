#include <doctest/doctest.h>
#include <psp2kern/kernel/systimer.h>
#include <psp2kern/kernel/threadmgr.h>
#include "tick_source.h"
#include "vita_tracy/kernel_abi.h"

namespace {
struct Fake {
    int calls = 0;
    int fail_at = -1;
    bool fail_free = false;
    bool event_alive = false;
    bool timer_alive = false;
    bool pending = false;
    uint64_t interval = 0;
    uint32_t mask = 0;
    uint8_t prescale = 0;
    SceSysTimerClockSource clock = SCE_SYSTIMER_CLOCK_SOURCE_SYS;
    SceSysTimerCallback callback = nullptr;
    void *arg = nullptr;
    int step() { return ++calls == fail_at ? -123 : 0; }
} fake;

struct Fixture {
    VitaTracyTickSource source;
    Fixture() { fake = {}; vita_tracy_tick_init(&source); }
};
}

extern "C" {
int ksceKernelCreateEventFlag(const char *, int, int, void *) {
    if (fake.step() < 0) return -123;
    fake.event_alive = true;
    return 23;
}
int ksceKernelDeleteEventFlag(int id) {
    CHECK(id == 23);
    CHECK_FALSE(fake.timer_alive);
    if (fake.step() < 0) return -123;
    fake.event_alive = false;
    return 0;
}
int ksceKernelSetEventFlag(int id, unsigned int bits) {
    CHECK(id == 23);
    CHECK(fake.event_alive);
    CHECK(bits == 1);
    fake.pending = true;
    return 0;
}
int ksceKernelWaitEventFlag(int id, unsigned int bits, unsigned int mode,
                           unsigned int *out_bits, uint32_t *timeout) {
    CHECK(id == 23);
    CHECK(fake.event_alive);
    CHECK(bits == 1);
    CHECK(mode == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT));
    CHECK(timeout == nullptr);
    if (!fake.pending) return -456;
    *out_bits = 1;
    fake.pending = false;
    return 0;
}
SceSysTimerId ksceKernelSysTimerAlloc(SceSysTimerType type) {
    CHECK(type == SCE_SYSTIMER_TYPE_WORD);
    if (fake.step() < 0) return -123;
    fake.timer_alive = true;
    return 17;
}
int ksceKernelSysTimerFree(SceSysTimerId id) {
    CHECK(id == 17);
    CHECK(fake.event_alive);
    if (fake.step() < 0 || fake.fail_free) return -123;
    fake.timer_alive = false;
    return 0;
}
int ksceKernelSysTimerStartCount(SceSysTimerId id) { CHECK(id == 17); return fake.step(); }
int ksceKernelSysTimerStopCount(SceSysTimerId id) { CHECK(id == 17); return fake.step(); }
int ksceKernelSysTimerResetCount(SceSysTimerId id) { CHECK(id == 17); return fake.step(); }
int ksceKernelSysTimerSetClockSource(SceSysTimerId id, SceSysTimerClockSource clock, uint8_t prescale) {
    CHECK(id == 17);
    fake.clock = clock;
    fake.prescale = prescale;
    return fake.step();
}
int ksceKernelSysTimerSetInterval(SceSysTimerId id, SceKernelSysClock interval) {
    CHECK(id == 17);
    fake.interval = interval;
    return fake.step();
}
int ksceKernelSysTimerSetHandler(SceSysTimerId id, SceSysTimerCallback callback, uint32_t mask, void *arg) {
    CHECK(id == 17);
    fake.callback = callback;
    fake.arg = arg;
    fake.mask = mask;
    return fake.step();
}
}

TEST_CASE_FIXTURE(Fixture, "timer uses a fixed 1 MHz input and a periodic interval") {
    REQUIRE(vita_tracy_tick_start(&source, 500) == 0);
    CHECK(fake.clock == SCE_SYSTIMER_CLOCK_SOURCE_48MHZ);
    CHECK(fake.prescale == 47);
    CHECK(fake.interval == 2000);
    CHECK(fake.mask == 1);
    CHECK(vita_tracy_tick_stop(&source) == 0);
    CHECK_FALSE(fake.timer_alive);
    CHECK_FALSE(fake.event_alive);
}

TEST_CASE_FIXTURE(Fixture, "a tick before the wait is retained and overruns coalesce") {
    REQUIRE(vita_tracy_tick_start(&source, 250) == 0);
    REQUIRE(fake.callback != nullptr);
    fake.callback(17, fake.arg);
    fake.callback(17, fake.arg);
    CHECK(source.ticks == 2);
    CHECK(vita_tracy_tick_wait(&source) == 0);
    CHECK_FALSE(fake.pending);
    fake.callback(17, fake.arg);
    CHECK(vita_tracy_tick_wait(&source) == 0);
    CHECK(source.ticks == 3);
    CHECK(vita_tracy_tick_stop(&source) == 0);
}

TEST_CASE_FIXTURE(Fixture, "invalid rates allocate no kernel resources") {
    CHECK(vita_tracy_tick_start(&source, 0) == VITA_TRACY_ERROR_ARGS);
    CHECK(vita_tracy_tick_start(&source, VITA_TRACY_MAX_SAMPLE_HZ + 1) == VITA_TRACY_ERROR_ARGS);
    CHECK(fake.calls == 0);
    CHECK(vita_tracy_tick_stop(&source) == 0);
}

TEST_CASE("every timer startup failure rolls back handles in the right order") {
    for (int failure = 1; failure <= 7; ++failure) {
        Fixture fixture;
        fake.fail_at = failure;
        CHECK(vita_tracy_tick_start(&fixture.source, 100) == -123);
        CHECK_FALSE(fake.timer_alive);
        CHECK_FALSE(fake.event_alive);
        CHECK(fixture.source.enabled == 0);
        CHECK(vita_tracy_tick_stop(&fixture.source) == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "a failed timer release keeps its event and blocks reuse") {
    REQUIRE(vita_tracy_tick_start(&source, 100) == 0);
    fake.fail_free = true;
    CHECK(vita_tracy_tick_stop(&source) == -123);
    CHECK(fake.timer_alive);
    CHECK(fake.event_alive);
    CHECK(source.enabled == 0);
    fake.callback(17, fake.arg);
    CHECK_FALSE(fake.pending);
    CHECK(vita_tracy_tick_start(&source, 200) == VITA_TRACY_ERROR_STATE);
    fake.fail_free = false;
    CHECK(vita_tracy_tick_stop(&source) == 0);
    CHECK_FALSE(fake.timer_alive);
    CHECK_FALSE(fake.event_alive);
    CHECK(vita_tracy_tick_stop(&source) == 0);
}

TEST_CASE_FIXTURE(Fixture, "shutdown can wake a worker without waiting for the next tick") {
    REQUIRE(vita_tracy_tick_start(&source, 1) == 0);
    vita_tracy_tick_wake(&source);
    CHECK(vita_tracy_tick_wait(&source) == 0);
    CHECK(source.ticks == 0);
    CHECK(vita_tracy_tick_stop(&source) == 0);
}

TEST_CASE_FIXTURE(Fixture, "custom timer callbacks run only after arm on their selected core") {
    int calls = 0;
    auto callback = [](void *context) { ++*static_cast<int *>(context); };
    REQUIRE(vita_tracy_tick_prepare(&source, 100, 4u, callback, &calls) == 0);
    CHECK(fake.mask == 4u);
    fake.callback(17, fake.arg);
    CHECK(calls == 0);
    REQUIRE(vita_tracy_tick_arm(&source) == 0);
    CHECK(vita_tracy_tick_arm(&source) == VITA_TRACY_ERROR_STATE);
    fake.callback(17, fake.arg);
    CHECK(calls == 1);
    CHECK_FALSE(fake.pending); // No worker wakeup when capture runs in the callback.
    REQUIRE(vita_tracy_tick_stop(&source) == 0);
    fake.callback(17, fake.arg);
    CHECK(calls == 1);
}

TEST_CASE_FIXTURE(Fixture, "closing a callback admission gate retains live callback resources") {
    auto callback = [](void *p) {
        auto *s = static_cast<VitaTracyTickSource *>(p);
        CHECK(vita_tracy_tick_stop(s) == VITA_TRACY_ERROR_BUSY);
        CHECK(fake.timer_alive); CHECK(fake.event_alive);
        CHECK(vita_tracy_tick_arm(s) == VITA_TRACY_ERROR_STATE);
        fake.callback(17, fake.arg); // A nested/late callback is rejected.
    };
    REQUIRE(vita_tracy_tick_prepare(&source, 100, 1u, callback, &source) == 0);
    REQUIRE(vita_tracy_tick_arm(&source) == 0);
    fake.callback(17, fake.arg);
    CHECK(source.ticks == 1);
    CHECK(source.enabled == 0);
    REQUIRE(vita_tracy_tick_stop(&source) == 0);
    CHECK_FALSE(fake.timer_alive); CHECK_FALSE(fake.event_alive);
}

TEST_CASE_FIXTURE(Fixture, "timer callbacks require exactly one valid CPU bit") {
    for (uint32_t mask : {0u, 3u, 16u, 0x10000u})
        CHECK(vita_tracy_tick_prepare(&source, 100, mask, nullptr, nullptr) == VITA_TRACY_ERROR_ARGS);
    CHECK(fake.calls == 0);
}
