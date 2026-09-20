#include <doctest/doctest.h>
#include <array>
#include <map>
#include <vector>
#include <cstring>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/threadmgr.h>
extern "C" {
#include "internal.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"
const VitaPmuIo *vita_tracy_pmu_io(void);
}

namespace {
struct Bank {
    uint32_t pmcr = 6u << 11, enable = 0, interrupts = 0, overflow = 0, select = 0, cycles = 200;
    std::array<uint32_t, 6> types{{1,2,3,4,5,6}}, values{{10,20,30,40,50,60}};
};
struct Job { SceKernelThreadEntry entry; uint32_t core; bool started = false; };
struct Fake {
    int core = 0, next_job = 20, notifications = 0, reads = 0;
    int fail_prepare = -1, fail_arm = -1, fail_start = -1, fail_join = -1, wrong_job = -1;
    int busy_timer = -1;
    uint64_t now = 10000;
    std::array<Bank, 4> banks;
    std::array<VitaTracyTickSource *,4> timers{};
    std::map<int, Job> jobs;
} fake;
uint32_t read_reg(void *, VitaPmuRegister reg) {
    ++fake.reads;
    Bank &b = fake.banks.at(fake.core);
    switch (reg) {
    case VITA_PMU_PMCR: return b.pmcr;
    case VITA_PMU_CNTEN: return b.enable;
    case VITA_PMU_INTEN: return b.interrupts;
    case VITA_PMU_OVSR: return b.overflow;
    case VITA_PMU_SELR: return b.select;
    case VITA_PMU_CYCLES: return b.cycles;
    case VITA_PMU_TYPE: return b.types.at(b.select);
    case VITA_PMU_VALUE: return b.values.at(b.select);
    default: FAIL("unexpected read"); return 0;
    }
}
void write_reg(void *, VitaPmuRegister reg, uint32_t v) {
    Bank &b = fake.banks.at(fake.core);
    switch (reg) {
    case VITA_PMU_PMCR: CHECK((v & 6u) == 0); b.pmcr = v; break;
    case VITA_PMU_CNTEN: b.enable |= v; break;
    case VITA_PMU_CNTCLR: b.enable &= ~v; break;
    case VITA_PMU_OVSR: b.overflow &= ~v; break;
    case VITA_PMU_SELR: b.select = v; break;
    case VITA_PMU_CYCLES: b.cycles = v; break;
    case VITA_PMU_TYPE: b.types.at(b.select) = v; break;
    case VITA_PMU_VALUE: b.values.at(b.select) = v; break;
    default: FAIL("unexpected write");
    }
}
void tick(unsigned cpu, int actual_core = -1) {
    fake.core = actual_core < 0 ? (int)cpu : actual_core;
    VitaTracyTickSource *s = fake.timers.at(cpu);
    REQUIRE(s != nullptr);
    if (s->enabled) s->callback(s->context);
}
struct Fixture {
    VitaTracyKernelState state{};
    std::vector<uint8_t> memory;
    VitaTracyPmuConfig config{};
    Fixture() {
        fake = {};
        memory.resize(vita_trace_shared_layout_size(16, 16));
        REQUIRE(vita_trace_shared_init(memory.data(), memory.size(), 123, 1000000, 16, 16) == 1);
        state.shared = memory.data(); state.target_pid = 123;
        config.core_mask = 7; config.frequency_hz = 100;
        config.counter_count = 2;
        config.counters[0] = {0, 0x68}; config.counters[1] = {3, 0x03};
        REQUIRE(vita_tracy_pmu_configure(&state, &config) == 0);
    }
    ~Fixture() {
        fake.busy_timer = fake.fail_join = fake.fail_start = fake.wrong_job = -1;
        CHECK(vita_tracy_pmu_sample_stop(&state) == 0);
        CHECK(fake.jobs.empty());
    }
};
}

extern "C" {
const VitaPmuIo *vita_tracy_pmu_io(void) {
    static VitaPmuIo io{nullptr, read_reg, write_reg}; return &io;
}
int ksceKernelCpuId(void) { return fake.core; }
SceKernelIntrStatus ksceKernelCpuSuspendIntr(void) { return 1; }
SceKernelIntrStatus ksceKernelCpuResumeIntr(SceKernelIntrStatus v) { return v; }
uint64_t vita_tracy_kernel_now(void) { return fake.now; }
void vita_tracy_notify(VitaTracyKernelState *) { ++fake.notifications; }
SceUID ksceKernelCreateThread(const char *, SceKernelThreadEntry entry, int, SceSize, unsigned,
                              int affinity, const void *) {
    unsigned core = 0;
    while (core < 4 && affinity != (int)(0x10000u << core)) ++core;
    REQUIRE(core < 4);
    int id = ++fake.next_job;
    fake.jobs.emplace(id, Job{entry, core});
    return id;
}
int ksceKernelStartThread(SceUID id, SceSize size, void *arg) {
    auto &job = fake.jobs.at(id);
    if ((int)job.core == fake.fail_start) return -101;
    fake.core = (int)job.core == fake.wrong_job ? (job.core + 1) % 4 : job.core;
    job.started = true;
    job.entry(size, arg);
    fake.core = 0;
    return 0;
}
int ksceKernelWaitThreadEnd(SceUID id, int *, SceUInt *) {
    auto &job = fake.jobs.at(id);
    CHECK(job.started); // A failed StartThread must not be joined as a running worker.
    return (int)job.core == fake.fail_join ? -102 : 0;
}
int ksceKernelDeleteThread(SceUID id) { REQUIRE(fake.jobs.erase(id) == 1); return 0; }
void vita_tracy_tick_init(VitaTracyTickSource *s) { *s = {}; s->timer = s->event = -1; }
int vita_tracy_tick_prepare(VitaTracyTickSource *s, uint32_t hz, uint32_t mask,
                            VitaTracyTickCallback callback, void *context) {
    CHECK(hz == 100);
    unsigned cpu = 0; while (cpu < 4 && mask != (1u << cpu)) ++cpu;
    REQUIRE(cpu < 4);
    if ((int)cpu == fake.fail_prepare) return -103;
    s->timer = (int)cpu + 1; s->event = (int)cpu + 10;
    s->callback = callback; s->context = context; s->prepared = 1;
    fake.timers[cpu] = s;
    return 0;
}
int vita_tracy_tick_arm(VitaTracyTickSource *s) {
    if (s->timer - 1 == fake.fail_arm) return -104;
    s->enabled = 1; s->prepared = 0;
    // Simulate a first interrupt before the other cores have armed.
    fake.core = s->timer - 1; s->callback(s->context); fake.core = 0;
    return 0;
}
int vita_tracy_tick_stop(VitaTracyTickSource *s) {
    if (s->timer < 0) return 0;
    if (s->timer - 1 == fake.busy_timer) return VITA_TRACY_ERROR_BUSY;
    fake.timers[s->timer - 1] = nullptr;
    s->enabled = 0; s->timer = s->event = -1;
    return 0;
}
}

TEST_CASE_FIXTURE(Fixture, "PMU session records from IRQ callbacks with no resident polling jobs") {
    REQUIRE(vita_tracy_pmu_sample_start(&state) == 0);
    CHECK(fake.jobs.empty()); CHECK(state.stats.pmu_active_mask == 7);
    CHECK(fake.timers[3] == nullptr); CHECK(fake.banks[3].enable == 0);
    for (unsigned cpu = 0; cpu < 3; ++cpu) tick(cpu);
    CHECK(fake.notifications == 0); // Baselines, not fabricated zero deltas.
    fake.now += 12234;
    for (unsigned cpu = 0; cpu < 3; ++cpu) {
        fake.banks[cpu].cycles += 1000 * (cpu + 1);
        fake.banks[cpu].values[0] += 400;
        tick(cpu);
        VitaTracePmuSample sample{};
        REQUIRE(vita_trace_ring_try_pop(vita_trace_shared_pmu_ring(memory.data(), cpu), &sample) == 1);
        CHECK(sample.cpu == cpu); CHECK(sample.elapsed_us == 12234);
        CHECK(sample.cycles == 1000 * (cpu + 1)); CHECK(sample.events[0] == 0x68);
        CHECK(sample.values[0] == 400); CHECK(state.stats.pmu_records[cpu] == 1);
    }
    REQUIRE(vita_tracy_pmu_sample_stop(&state) == 0);
    CHECK(state.stats.pmu_active_mask == 0);
    for (const Bank &b : fake.banks) { CHECK(b.enable == 0); CHECK(b.cycles == 200); }
}
TEST_CASE_FIXTURE(Fixture, "PMU partial acquisition restores earlier cores without touching an owner") {
    fake.banks[1].enable = 1;
    CHECK(vita_tracy_pmu_sample_start(&state) == VITA_TRACY_ERROR_BUSY);
    CHECK(state.stats.pmu_active_mask == 0); CHECK(fake.banks[0].enable == 0);
    CHECK(fake.banks[1].enable == 1);
    for (auto *timer : fake.timers) CHECK(timer == nullptr);
}
TEST_CASE_FIXTURE(Fixture, "PMU timer startup failure releases all previously acquired resources") {
    fake.fail_arm = 1;
    CHECK(vita_tracy_pmu_sample_start(&state) == -104);
    CHECK(fake.jobs.empty());
    for (auto *timer : fake.timers) CHECK(timer == nullptr);
    for (const Bank &b : fake.banks) CHECK(b.enable == 0);
}
TEST_CASE_FIXTURE(Fixture, "PMU stop with a live callback retains ownership for a later retry") {
    REQUIRE(vita_tracy_pmu_sample_start(&state) == 0);
    fake.busy_timer = 1;
    CHECK(vita_tracy_pmu_sample_stop(&state) == VITA_TRACY_ERROR_BUSY);
    CHECK(fake.banks[0].enable != 0); CHECK(fake.banks[1].enable != 0);
    CHECK(vita_tracy_pmu_sample_start(&state) == VITA_TRACY_ERROR_BUSY);
    int reads = fake.reads; tick(1); CHECK(fake.reads == reads); // Session was closed.
    fake.busy_timer = -1;
    REQUIRE(vita_tracy_pmu_sample_stop(&state) == 0);
    for (const Bank &b : fake.banks) CHECK(b.enable == 0);
}
TEST_CASE_FIXTURE(Fixture, "PMU wrong-core callbacks do not read counters or corrupt another ring") {
    REQUIRE(vita_tracy_pmu_sample_start(&state) == 0);
    int reads = fake.reads;
    tick(1, 0);
    CHECK(fake.reads == reads); CHECK(state.stats.pmu_wrong_cpu[1] == 1);
    CHECK(vita_trace_ring_pending(vita_trace_shared_pmu_ring(memory.data(), 1)) == 0);
}
TEST_CASE_FIXTURE(Fixture, "PMU counter ownership changes stop that core instead of producing nonsense") {
    REQUIRE(vita_tracy_pmu_sample_start(&state) == 0);
    fake.banks[0].types[0] = 0x60;
    tick(0);
    CHECK(state.stats.pmu_counter_errors[0] == 1);
    int reads = fake.reads; tick(0); CHECK(fake.reads == reads);
    REQUIRE(vita_tracy_pmu_sample_stop(&state) == 0);
    CHECK(fake.banks[0].types[0] == 0x60); CHECK(fake.banks[0].enable != 0);
}
TEST_CASE_FIXTURE(Fixture, "PMU rejects per-thread attribution and active reconfiguration") {
    config.target_tid = 55;
    CHECK(vita_tracy_pmu_configure(&state, &config) == VITA_TRACY_ERROR_UNSUPPORTED);
    config.target_tid = 0;
    REQUIRE(vita_tracy_pmu_sample_start(&state) == 0);
    CHECK(vita_tracy_pmu_configure(&state, &config) == VITA_TRACY_ERROR_STATE);
}
TEST_CASE_FIXTURE(Fixture, "PMU failed thread start rolls back without joining an unstarted thread") {
    fake.fail_start = 1;
    CHECK(vita_tracy_pmu_sample_start(&state) == -101);
    CHECK(fake.jobs.empty());
    CHECK(fake.banks[0].enable == 0);
}
