#include <doctest/doctest.h>

#include <cstring>
#include <vector>

extern "C" {
#include "internal.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"
void vita_tracy_sched_hooks_test_reset(void);
}

namespace {

typedef void (*OnCpu)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*OffCpu)(uint32_t, uint32_t, uint32_t);

struct Fake {
    int core = 0;
    uint64_t now = 1000;
    bool resolvable = true;
    void *on = nullptr, *off = nullptr;
    int on_sets = 0, off_sets = 0;
} fake;

int set_on(void *hook) { fake.on = hook; ++fake.on_sets; return 0; }
int set_off(void *hook) { fake.off = hook; ++fake.off_sets; return 0; }

struct Fixture {
    VitaTracyKernelState state{};
    std::vector<uint8_t> memory;
    Fixture() {
        vita_tracy_sched_hooks_test_reset();
        fake = {};
        memory.resize(vita_trace_shared_layout_size(16, 16));
        REQUIRE(vita_trace_shared_init(memory.data(), memory.size(), 123, 1000000, 16, 16) == 1);
        state.shared = memory.data();
        state.target_pid = 123;
    }
    bool pop(uint32_t cpu, VitaTraceSwitch &rec) {
        return vita_trace_ring_try_pop(vita_trace_shared_switch_ring(memory.data(), cpu), &rec) != 0;
    }
};

} // namespace

extern "C" {
int vita_tracy_lookup_export(const char *, uint32_t library, uint32_t function, uintptr_t *address) {
    if (!fake.resolvable || library != 0xA8CA0EFDu) return -1;
    if (function == 0x15AAB4F9u) { *address = (uintptr_t)set_on; return 0; }
    if (function == 0xDBE2EE32u) { *address = (uintptr_t)set_off; return 0; }
    return -1;
}
int ksceKernelCpuId(void) { return fake.core; }
uint64_t vita_tracy_kernel_now(void) { return fake.now; }
int ksceKernelDelayThread(SceUInt) { return 0; }
}

TEST_CASE_FIXTURE(Fixture, "scheduler hooks record the target's threads per core") {
    REQUIRE(vita_tracy_sched_hooks_start(&state) == 0);
    REQUIRE(fake.on != nullptr);
    REQUIRE(fake.off != nullptr);
    CHECK(state.stats.switch_hooks_installed == 1);

    fake.core = 1;
    fake.now = 5000;
    reinterpret_cast<OnCpu>(fake.on)(123, 0x40010003u, 0, 0);
    fake.now = 6000;
    reinterpret_cast<OffCpu>(fake.off)(123, 0x40010003u, 4);
    reinterpret_cast<OnCpu>(fake.on)(999, 0x40020001u, 0, 0); /* another process */

    VitaTraceSwitch rec{};
    REQUIRE(pop(1, rec));
    CHECK(rec.timestamp == 5000);
    CHECK(rec.tid == 0x40010003u);
    CHECK(rec.cpu == 1);
    CHECK(rec.kind == VITA_TRACE_SWITCH_IN);
    REQUIRE(pop(1, rec));
    CHECK(rec.kind == VITA_TRACE_SWITCH_OUT);
    CHECK(rec.reason == 4);
    CHECK_FALSE(pop(1, rec));
    CHECK_FALSE(pop(0, rec));
    CHECK(state.stats.switch_calls[1] == 3);
    CHECK(state.stats.switch_recorded[1] == 2);
    CHECK(state.stats.switch_last_other_pid == 999);

    vita_tracy_sched_hooks_stop();
    CHECK(fake.on == nullptr);
    CHECK(fake.off == nullptr);
    CHECK(state.stats.switch_hooks_installed == 0);
}

TEST_CASE_FIXTURE(Fixture, "a scheduler hook called after stop touches nothing") {
    REQUIRE(vita_tracy_sched_hooks_start(&state) == 0);
    OnCpu stale = reinterpret_cast<OnCpu>(fake.on);
    vita_tracy_sched_hooks_stop();
    stale(123, 7, 0, 0); /* a core that loaded the pointer before it was cleared */
    VitaTraceSwitch rec{};
    CHECK_FALSE(pop(0, rec));
    CHECK(state.stats.switch_calls[0] == 0);
}

TEST_CASE_FIXTURE(Fixture, "scheduler hooks count a full ring and refuse a second start") {
    REQUIRE(vita_tracy_sched_hooks_start(&state) == 0);
    CHECK(vita_tracy_sched_hooks_start(&state) == VITA_TRACY_ERROR_BUSY);
    for (uint32_t i = 0; i < VITA_TRACE_SWITCH_RING_CAPACITY + 3; ++i)
        reinterpret_cast<OnCpu>(fake.on)(123, 7, 0, 0);
    CHECK(state.stats.switch_recorded[0] == VITA_TRACE_SWITCH_RING_CAPACITY);
    CHECK(state.stats.switch_dropped[0] == 3);
    vita_tracy_sched_hooks_stop();
}

TEST_CASE_FIXTURE(Fixture, "scheduler hooks are optional when the firmware lacks the setters") {
    fake.resolvable = false;
    CHECK(vita_tracy_sched_hooks_start(&state) == VITA_TRACY_ERROR_UNSUPPORTED);
    CHECK(state.stats.switch_hooks_installed == 0);
    vita_tracy_sched_hooks_stop(); /* harmless */
}
