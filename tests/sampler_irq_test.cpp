#include <doctest/doctest.h>

#include <array>
#include <map>
#include <vector>

#ifndef VITA_TRACY_IRQ_CORE_MASK
#define VITA_TRACY_IRQ_CORE_MASK 7u
#endif

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>

extern "C" {
#include "internal.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/pmu_core.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

void vita_tracy_irq_handler_c(SceExcpmgrExceptionContext *context, SceExcpHandlingCode code);
void vita_tracy_sampler_irq_test_reset(void);
}

namespace {

struct Bank {
    uint32_t pmcr = 6u << 11;
    uint32_t enable = 0;
    uint32_t interrupts = 0;
    uint32_t overflow = 0;
    uint32_t cycles = 200;
    uint32_t select = 0;
    std::array<uint32_t, 6> types{};
    std::array<uint32_t, 6> values{};
};

struct Job {
    SceKernelThreadEntry entry = nullptr;
    uint32_t core = 0;
    bool started = false;
};

struct Fake {
    int core = 0;
    int arm_mhz = 444;
    int next_job = 20;
    int notifications = 0;
    uint32_t notify_events = 0;
    int handler_registrations = 0;
    int handler_result = 0;
    int context_result = 0;
    VitaTracyKernelState *stop_in_context = nullptr;
    int stop_result = 0;
    bool nested_context = false;
    int fail_join_core = -1;
    int fail_delete_core = -1;
    int fail_arm_core = -1;
    unsigned starts[4]{};
    uint64_t now = 10000;
    void *registered_handler = nullptr;
    SceKernelThreadContextInfo thread{123, 0x500};
    std::array<Bank, 4> banks{};
    std::map<int, Job> jobs{};
} fake;

uint32_t read_reg(void *, VitaPmuRegister reg) {
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

void write_reg(void *, VitaPmuRegister reg, uint32_t value) {
    Bank &b = fake.banks.at(fake.core);
    switch (reg) {
    case VITA_PMU_PMCR:
        CHECK((value & 6u) == 0);
        b.pmcr = value;
        break;
    case VITA_PMU_CNTEN: b.enable |= value; break;
    case VITA_PMU_CNTCLR: b.enable &= ~value; break;
    case VITA_PMU_INTEN: b.interrupts |= value; break;
    case VITA_PMU_INTCLR: b.interrupts &= ~value; break;
    case VITA_PMU_OVSR: b.overflow &= ~value; break;
    case VITA_PMU_CYCLES: b.cycles = value; break;
    case VITA_PMU_SELR: b.select = value; break;
    case VITA_PMU_TYPE: b.types.at(b.select) = value; break;
    case VITA_PMU_VALUE: b.values.at(b.select) = value; break;
    }
}

struct Fixture {
    VitaTracyKernelState state{};
    std::vector<uint8_t> memory;

    Fixture() {
        vita_tracy_sampler_irq_test_reset();
        fake = {};
        memory.resize(vita_trace_shared_layout_size(32, 16));
        REQUIRE(vita_trace_shared_init(memory.data(), memory.size(), 123, 1000000, 32, 16) == 1);
        state.shared = memory.data();
        state.target_pid = 123;
        state.sampling_hz = 500;
        vita_trace_control_set_target(&state.control, 123);
    }

    ~Fixture() {
        fake.fail_join_core = fake.fail_delete_core = fake.fail_arm_core = -1;
        CHECK(vita_tracy_sampler_irq_stop(&state) == 0);
        CHECK(fake.jobs.empty());
    }

    void overflow(unsigned core, SceExcpmgrExceptionContext &context) {
        fake.core = static_cast<int>(core);
        fake.banks[core].overflow |= VITA_PMU_CYCLE_BIT;
        vita_tracy_irq_handler_c(&context, SCE_EXCPMGR_EXCEPTION_HANDLED);
        fake.core = 0;
    }
};

} // namespace

extern "C" {

const VitaPmuIo *vita_tracy_pmu_io(void) {
    static const VitaPmuIo io{nullptr, read_reg, write_reg};
    return &io;
}

int ksceKernelCpuId(void) { return fake.core; }
SceKernelIntrStatus ksceKernelCpuSuspendIntr(void) { return 1; }
SceKernelIntrStatus ksceKernelCpuResumeIntr(SceKernelIntrStatus value) { return value; }
int kscePowerGetArmClockFrequency(void) { return fake.arm_mhz; }
uint64_t vita_tracy_kernel_now(void) { return fake.now; }

int ksceExcpmgrRegisterHandler(SceExcpKind kind, int priority, void *handler) {
    CHECK(kind == SCE_EXCP_IRQ);
    CHECK(priority == 7);
    ++fake.handler_registrations;
    fake.registered_handler = handler;
    return fake.handler_result;
}

int ksceKernelGetThreadContextInfo(SceKernelThreadContextInfo *info) {
    if (fake.nested_context) {
        fake.nested_context = false;
        SceExcpmgrExceptionContext nested{};
        nested.SPSR = 0x10u;
        nested.address_of_faulting_instruction = 0x81234000u;
        fake.banks[fake.core].overflow |= VITA_PMU_CYCLE_BIT;
        vita_tracy_irq_handler_c(&nested, SCE_EXCPMGR_EXCEPTION_HANDLED);
    }
    if (fake.stop_in_context) {
        auto *state = fake.stop_in_context;
        fake.stop_in_context = nullptr;
        fake.stop_result = vita_tracy_sampler_irq_stop(state);
    }
    if (fake.context_result < 0) return fake.context_result;
    *info = fake.thread;
    return 0;
}

SceUID ksceKernelCreateThread(const char *, SceKernelThreadEntry entry, int, SceSize,
                             unsigned, int affinity, const void *) {
    unsigned core = 0;
    while (core < 4 && affinity != static_cast<int>(0x10000u << core)) ++core;
    REQUIRE(core < 4);
    const int id = ++fake.next_job;
    fake.jobs.emplace(id, Job{entry, core, false});
    return id;
}

int ksceKernelStartThread(SceUID id, SceSize size, void *arg) {
    Job &job = fake.jobs.at(id);
    if (++fake.starts[job.core] == 2 && static_cast<int>(job.core) == fake.fail_arm_core)
        return -91;
    fake.core = static_cast<int>(job.core);
    job.started = true;
    job.entry(size, arg);
    fake.core = 0;
    return 0;
}

int ksceKernelWaitThreadEnd(SceUID id, int *, SceUInt *) {
    CHECK(fake.jobs.at(id).started);
    if (static_cast<int>(fake.jobs.at(id).core) == fake.fail_join_core) return -92;
    return 0;
}

int ksceKernelDeleteThread(SceUID id) {
    if (static_cast<int>(fake.jobs.at(id).core) == fake.fail_delete_core) return -93;
    REQUIRE(fake.jobs.erase(id) == 1);
    return 0;
}

void vita_tracy_emit_sample(VitaTracyKernelState *st, uint32_t cpu, const VitaTraceSample *sample) {
    void *ring = vita_trace_shared_core_ring(st->shared, cpu);
    REQUIRE(ring != nullptr);
    if (vita_trace_ring_try_push(ring, sample))
        ++st->stats.samples_emitted[cpu];
    else
        ++st->stats.samples_dropped[cpu];
}

void vita_tracy_notify(VitaTracyKernelState *) { ++fake.notifications; }
void vita_tracy_notify_events(VitaTracyKernelState *, uint32_t events) {
    ++fake.notifications;
    fake.notify_events |= events;
}

} // extern "C"

TEST_CASE_FIXTURE(Fixture, "IRQ sampler prepares all app cores and restores them on stop") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    CHECK(fake.jobs.empty());
    CHECK(vita_tracy_sampler_irq_handler_registered());
    CHECK(state.stats.sample_irq_handler_registered == 1);
    CHECK(state.stats.sample_irq_arm_mhz == 444);
    CHECK(state.stats.sample_irq_core_mask == 7);

    const uint32_t preload = 0u - (444000000u / 500u);
    for (unsigned cpu = 0; cpu < 3; ++cpu) {
        CHECK(fake.banks[cpu].enable == VITA_PMU_CYCLE_BIT);
        CHECK(fake.banks[cpu].interrupts == VITA_PMU_CYCLE_BIT);
        CHECK(fake.banks[cpu].cycles == preload);
    }
    CHECK(fake.banks[3].enable == 0);
    CHECK(fake.banks[3].interrupts == 0);

    REQUIRE(vita_tracy_sampler_irq_stop(&state) == 0);
    for (const Bank &bank : fake.banks) {
        CHECK(bank.enable == 0);
        CHECK(bank.interrupts == 0);
        CHECK(bank.cycles == 200);
    }
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler captures target user context without suspending it") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    SceExcpmgrExceptionContext context{};
    context.address_of_faulting_instruction = 0x81234566u;
    context.sp = 0x83001000u;
    context.lr = 0x81230001u;
    context.SPSR = 0x10u | (1u << 5);

    overflow(1, context);

    VitaTraceSample sample{};
    REQUIRE(vita_trace_ring_try_pop(vita_trace_shared_core_ring(memory.data(), 1), &sample));
    CHECK(sample.pid == 123);
    CHECK(sample.tid == 0x500);
    CHECK(sample.pc == 0x81234566u);
    CHECK(sample.sp == 0x83001000u);
    CHECK(sample.lr == 0x81230001u);
    CHECK(sample.cpu == 1);
    CHECK((sample.flags & VITA_TRACE_SAMPLE_PMU_IRQ) != 0);
    CHECK((sample.flags & VITA_TRACE_SAMPLE_GLOBAL_TID) != 0);
    CHECK((sample.flags & VITA_TRACE_SAMPLE_THUMB) != 0);
    CHECK(state.stats.sample_irq_calls[1] == 1);
    CHECK(state.stats.sample_irq_overflows[1] == 1);
    CHECK(state.stats.samples_emitted[1] == 1);
    CHECK(fake.notifications == 1);
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler ignores unrelated IRQs without touching thread context") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    fake.context_result = -77;
    fake.core = 2;
    SceExcpmgrExceptionContext context{};
    vita_tracy_irq_handler_c(&context, SCE_EXCPMGR_EXCEPTION_HANDLED);
    CHECK(state.stats.sample_irq_calls[2] == 1);
    CHECK(state.stats.sample_irq_overflows[2] == 0);
    CHECK(state.stats.sample_irq_context_errors[2] == 0);
    CHECK(vita_trace_ring_pending(vita_trace_shared_core_ring(memory.data(), 2)) == 0);
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler services overflow but filters other processes and kernel mode") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    SceExcpmgrExceptionContext context{};
    context.SPSR = 0x10u;

    fake.thread.process_id = 999;
    overflow(0, context);
    CHECK(state.stats.sample_irq_not_target[0] == 1);
    CHECK(vita_trace_ring_pending(vita_trace_shared_core_ring(memory.data(), 0)) == 0);

    fake.thread.process_id = 123;
    context.SPSR = 0x13u;
    overflow(0, context);
    CHECK(state.stats.sample_irq_kernel[0] == 1);
    CHECK(state.stats.sample_irq_overflows[0] == 2);
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler records exception-context lookup failures") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    fake.context_result = -99;
    SceExcpmgrExceptionContext context{};
    context.SPSR = 0x10u;
    overflow(0, context);
    CHECK(state.stats.sample_irq_context_errors[0] == 1);
    CHECK(vita_trace_ring_pending(vita_trace_shared_core_ring(memory.data(), 0)) == 0);
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler reports PMU ownership loss through a retained wake bit") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    fake.core = 1;
    fake.banks[1].overflow = VITA_PMU_CYCLE_BIT;
    fake.banks[1].interrupts = 0; // A different owner changed our configuration.
    SceExcpmgrExceptionContext context{};
    context.SPSR = 0x10u;
    vita_tracy_irq_handler_c(&context, SCE_EXCPMGR_EXCEPTION_HANDLED);
    fake.core = 0;

    CHECK(state.stats.sample_irq_last_error == VITA_TRACY_ERROR_STATE);
    CHECK((fake.notify_events & VITA_TRACY_WAKE_SAMPLE_IRQ(1)) != 0);
    CHECK(vita_trace_ring_pending(vita_trace_shared_core_ring(memory.data(), 1)) == 0);
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler refuses a busy PMU and rolls back earlier cores") {
    fake.banks[1].enable = 1;
    CHECK(vita_tracy_sampler_irq_start(&state) == VITA_TRACY_ERROR_BUSY);
    CHECK(fake.banks[0].enable == 0);
    CHECK(fake.banks[0].interrupts == 0);
    CHECK(fake.banks[1].enable == 1);
    CHECK(fake.jobs.empty());
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler requires a measurable ARM clock") {
    fake.arm_mhz = 0;
    CHECK(vita_tracy_sampler_irq_start(&state) == VITA_TRACY_ERROR_UNSUPPORTED);
    for (const Bank &bank : fake.banks) {
        CHECK(bank.enable == 0);
        CHECK(bank.interrupts == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "IRQ handler registration failure rolls back prepared PMU state") {
    fake.handler_result = -123;
    CHECK(vita_tracy_sampler_irq_start(&state) == -123);
    CHECK_FALSE(vita_tracy_sampler_irq_handler_registered());
    CHECK(fake.handler_registrations == 1);
    for (const Bank &bank : fake.banks) {
        CHECK(bank.enable == 0);
        CHECK(bank.interrupts == 0);
        CHECK(bank.cycles == 200);
    }
    CHECK(fake.jobs.empty());
}

TEST_CASE_FIXTURE(Fixture, "IRQ stop retains state while an admitted callback still uses it") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    fake.stop_in_context = &state;
    SceExcpmgrExceptionContext context{};
    context.SPSR = 0x10u;
    context.address_of_faulting_instruction = 0x81234000u;
    overflow(0, context);
    CHECK(fake.stop_result == VITA_TRACY_ERROR_BUSY);
    CHECK(fake.banks[0].enable == VITA_PMU_CYCLE_BIT);
    CHECK(vita_tracy_sampler_irq_start(&state) == VITA_TRACY_ERROR_BUSY);
    REQUIRE(vita_tracy_sampler_irq_stop(&state) == 0);
    for (const auto &bank : fake.banks) CHECK(bank.enable == 0);
    auto emitted = state.stats.samples_emitted[0];
    overflow(0, context);
    CHECK(state.stats.samples_emitted[0] == emitted);
}

TEST_CASE_FIXTURE(Fixture, "nested IRQ callbacks cannot become two producers for one ring") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    fake.nested_context = true;
    SceExcpmgrExceptionContext context{};
    context.SPSR = 0x10u;
    context.address_of_faulting_instruction = 0x81234000u;
    overflow(1, context);
    CHECK(state.stats.samples_emitted[1] == 1);
    CHECK(state.stats.sample_irq_calls[1] == 1);
    REQUIRE(vita_tracy_sampler_irq_stop(&state) == 0);
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    overflow(1, context);
    CHECK(state.stats.samples_emitted[1] == 2);
    CHECK(fake.handler_registrations == 1);
}

TEST_CASE_FIXTURE(Fixture, "IRQ failed preparation join retains the helper until cleanup succeeds") {
    fake.fail_join_core = 1;
    CHECK(vita_tracy_sampler_irq_start(&state) == -92);
    CHECK_FALSE(fake.jobs.empty());
    CHECK_FALSE(vita_tracy_sampler_irq_handler_registered());
    CHECK(vita_tracy_sampler_irq_start(&state) == VITA_TRACY_ERROR_BUSY);
    CHECK(vita_tracy_sampler_irq_stop(&state) == -92);
    fake.fail_join_core = -1;
    REQUIRE(vita_tracy_sampler_irq_stop(&state) == 0);
    CHECK(fake.jobs.empty());
    for (const auto &bank : fake.banks) {
        CHECK(bank.cycles == 200);
        CHECK(bank.enable == 0);
        CHECK(bank.interrupts == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "IRQ partial arm failure disarms earlier cores without joining an unstarted job") {
    fake.fail_arm_core = 1;
    CHECK(vita_tracy_sampler_irq_start(&state) == -91);
    CHECK(vita_tracy_sampler_irq_handler_registered());
    CHECK(fake.jobs.empty());
    for (const auto &bank : fake.banks) {
        CHECK(bank.cycles == 200);
        CHECK(bank.enable == 0);
        CHECK(bank.interrupts == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "IRQ failed helper deletion remains retryable") {
    fake.fail_delete_core = 0;
    CHECK(vita_tracy_sampler_irq_start(&state) == -93);
    CHECK_FALSE(fake.jobs.empty());
    fake.fail_delete_core = -1;
    REQUIRE(vita_tracy_sampler_irq_stop(&state) == 0);
    CHECK(fake.jobs.empty());
    CHECK(fake.banks[0].cycles == 200);
}

TEST_CASE_FIXTURE(Fixture, "IRQ sampler honors the build-selected app-core mask") {
    REQUIRE(vita_tracy_sampler_irq_start(&state) == 0);
    CHECK(state.stats.sample_irq_core_mask == VITA_TRACY_IRQ_CORE_MASK);
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
        const bool selected = (VITA_TRACY_IRQ_CORE_MASK & (1u << cpu)) != 0;
        CHECK(fake.banks[cpu].enable == (selected ? VITA_PMU_CYCLE_BIT : 0u));
        CHECK(fake.banks[cpu].interrupts == (selected ? VITA_PMU_CYCLE_BIT : 0u));
        CHECK(fake.starts[cpu] == (selected ? 2u : 0u));
        SceExcpmgrExceptionContext context{};
        context.SPSR = 0x10u;
        context.address_of_faulting_instruction = 0x81234000u;
        overflow(cpu, context);
        CHECK(state.stats.samples_emitted[cpu] == (selected ? 1u : 0u));
    }
}
