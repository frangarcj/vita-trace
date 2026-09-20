#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include "internal.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/pmu_core.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

const VitaPmuIo *vita_tracy_pmu_io(void);

typedef struct PmuCpu {
    VitaPmuCore counters;
    VitaTracyTickSource clock;
    VitaTracyKernelState *state;
    void *ring;
    uint32_t cpu, sequence;
    SceUID job;
    int operation, result, job_started;
    uint32_t failed;
} PmuCpu;

static PmuCpu g_cpus[VITA_TRACE_CORE_COUNT];
static int g_initialized;
static uint32_t g_recording;
static uint32_t g_core_mask = 7u;
static uint32_t g_frequency = 100u;
static SceUID g_configured_pid;
static VitaPmuPlan g_plan; // Cycle-only by default; event selection is explicit.

enum { ACQUIRE = 1, RELEASE = 2 };

static void initialize(void) {
    if (g_initialized) return;
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        g_cpus[i].cpu = i;
        g_cpus[i].job = -1;
        vita_tracy_tick_init(&g_cpus[i].clock);
    }
    g_initialized = 1;
}

static int has_resources(void) {
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        PmuCpu *cpu = &g_cpus[i];
        if (cpu->job >= 0 || cpu->counters.acquired || cpu->clock.timer >= 0 || cpu->clock.event >= 0)
            return 1;
    }
    return 0;
}

static void record_error(VitaTracyKernelState *st, int error) {
    __atomic_store_n(&st->stats.pmu_last_error, error, __ATOMIC_RELEASE);
}

/* The timer owns this core's only PMU producer. No user-memory lookup, thread
 * enumeration, allocation, syscalls to the client or blocking locks here. */
static void on_pmu_tick(void *context) {
    PmuCpu *cpu = (PmuCpu *)context;
    VitaTracyKernelState *st = cpu->state;
    if (!__atomic_load_n(&g_recording, __ATOMIC_ACQUIRE) || cpu->failed) return;
    if ((uint32_t)ksceKernelCpuId() != cpu->cpu) {
        __atomic_fetch_add(&st->stats.pmu_wrong_cpu[cpu->cpu], 1u, __ATOMIC_RELAXED);
        return; // Never read another core's banked counters or write its SPSC ring.
    }
    SceKernelIntrStatus intr = ksceKernelCpuSuspendIntr();
    VitaPmuDelta delta;
    int ret = vita_pmu_read(&cpu->counters, vita_tracy_pmu_io(), vita_tracy_kernel_now(), &delta);
    ksceKernelCpuResumeIntr(intr);
    if (ret < 0) {
        cpu->failed = 1;
        __atomic_fetch_add(&st->stats.pmu_counter_errors[cpu->cpu], 1u, __ATOMIC_RELAXED);
        record_error(st, ret);
        vita_tracy_notify(st);
        return;
    }
    if (!ret) return; // First callback establishes an actual baseline.
    VitaTracePmuSample sample;
    memset(&sample, 0, sizeof(sample));
    sample.timestamp = delta.timestamp;
    sample.elapsed_us = delta.elapsed_us;
    sample.sequence = ++cpu->sequence;
    sample.cpu = cpu->cpu;
    sample.flags = delta.flags;
    sample.count = cpu->counters.plan.count;
    sample.cycles = delta.cycles;
    for (uint32_t i = 0; i < sample.count; ++i) {
        sample.events[i] = cpu->counters.plan.events[i];
        sample.values[i] = delta.values[i];
    }
    if (delta.flags) __atomic_fetch_add(&st->stats.pmu_gaps[cpu->cpu], 1u, __ATOMIC_RELAXED);
    if (vita_trace_ring_try_push(cpu->ring, &sample)) {
        __atomic_fetch_add(&st->stats.pmu_records[cpu->cpu], 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&st->stats.pmu_dropped[cpu->cpu], 1u, __ATOMIC_RELAXED);
    }
    // Legacy totals remain available for old source-level bring-up code only.
    __atomic_fetch_add(&st->stats.pmu_cycle_delta_total, delta.cycles, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st->stats.pmu_sample_ticks, 1u, __ATOMIC_RELAXED);
    vita_tracy_notify(st);
}

static int core_job(SceSize size, void *arg) {
    if (size != sizeof(uint32_t)) return VITA_TRACY_ERROR_ARGS;
    uint32_t index = *(uint32_t *)arg;
    if (index >= VITA_TRACE_CORE_COUNT) return VITA_TRACY_ERROR_ARGS;
    PmuCpu *cpu = &g_cpus[index];
    if ((uint32_t)ksceKernelCpuId() != index) {
        cpu->result = VITA_TRACY_ERROR_STATE;
        return 0;
    }
    SceKernelIntrStatus intr = ksceKernelCpuSuspendIntr();
    cpu->result = cpu->operation == ACQUIRE ?
        vita_pmu_acquire(&cpu->counters, vita_tracy_pmu_io(), &g_plan) :
        vita_pmu_release(&cpu->counters, vita_tracy_pmu_io());
    if (cpu->operation == ACQUIRE) {
        if (cpu->result == VITA_PMU_ERROR_BUSY) cpu->result = VITA_TRACY_ERROR_BUSY;
        else if (cpu->result == VITA_PMU_ERROR_ARGS) cpu->result = VITA_TRACY_ERROR_ARGS;
    }
    ksceKernelCpuResumeIntr(intr);
    return 0;
}

/* Arguments/results live in static per-core slots, never on a caller stack
 * that might disappear if joining the helper fails. Retain failed handles. */
static int finish_job(PmuCpu *cpu) {
    if (cpu->job < 0) return 0;
    int ret = 0;
    if (cpu->job_started) {
        ret = ksceKernelWaitThreadEnd(cpu->job, NULL, NULL);
        if (ret < 0) return ret;
    }
    ret = ksceKernelDeleteThread(cpu->job);
    if (ret < 0) return ret;
    cpu->job = -1;
    cpu->job_started = 0;
    return 0;
}

static int run_job(PmuCpu *cpu, int operation) {
    int ret = finish_job(cpu);
    if (ret < 0) return ret;
    cpu->operation = operation;
    cpu->result = VITA_TRACY_ERROR_STATE;
    SceUID thread = ksceKernelCreateThread("VitaTracyPmuSetup", core_job, 0x10000100,
        0x2000, 0, (int)(0x10000u << cpu->cpu), NULL);
    if (thread < 0) return thread;
    cpu->job = thread;
    cpu->job_started = 0;
    ret = ksceKernelStartThread(thread, sizeof(cpu->cpu), &cpu->cpu);
    if (ret < 0) {
        if (ksceKernelDeleteThread(thread) >= 0) cpu->job = -1;
        return ret;
    }
    cpu->job_started = 1;
    ret = finish_job(cpu);
    return ret < 0 ? ret : cpu->result;
}

int vita_tracy_pmu_configure(VitaTracyKernelState *st, const VitaTracyPmuConfig *cfg) {
    initialize();
    if (has_resources()) return VITA_TRACY_ERROR_STATE;
    if (cfg->target_tid) return VITA_TRACY_ERROR_UNSUPPORTED;
    uint32_t mask = cfg->core_mask ? cfg->core_mask : 7u;
    uint32_t frequency = cfg->frequency_hz ? cfg->frequency_hz : 100u;
    if ((mask & ~15u) || frequency < 10u || frequency > VITA_TRACY_MAX_SAMPLE_HZ ||
        cfg->counter_count > VITA_PMU_EVENTS) return VITA_TRACY_ERROR_ARGS;
    VitaPmuPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.count = cfg->counter_count;
    for (uint32_t i = 0; i < plan.count; ++i) {
        plan.counters[i] = cfg->counters[i].counter;
        plan.events[i] = cfg->counters[i].event_code;
    }
    if (!vita_pmu_plan_valid(&plan, VITA_PMU_EVENTS)) return VITA_TRACY_ERROR_ARGS;
    g_plan = plan;
    g_core_mask = mask;
    g_frequency = frequency;
    g_configured_pid = st->target_pid;
    return VITA_TRACY_OK;
}

int vita_tracy_pmu_sample_stop(VitaTracyKernelState *st) {
    initialize();
    __atomic_store_n(&g_recording, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&st->stats.pmu_active_mask, 0u, __ATOMIC_RELEASE);
    int first_error = 0;
    // Quiesce every callback before restoring any bank or releasing the mapping.
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        int ret = vita_tracy_tick_stop(&g_cpus[i].clock);
        if (ret < 0 && !first_error) first_error = ret;
    }
    if (first_error) { record_error(st, first_error); return first_error; }
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        PmuCpu *cpu = &g_cpus[i];
        int ret = finish_job(cpu);
        if (ret >= 0 && cpu->counters.acquired) ret = run_job(cpu, RELEASE);
        if (ret == VITA_PMU_ERROR_OWNERSHIP && !cpu->counters.acquired) {
            record_error(st, ret); // Someone else owns it; no resources of ours remain.
            ret = 0;
        }
        if (ret < 0 && !first_error) first_error = ret;
        if (!cpu->counters.acquired && cpu->job < 0) cpu->ring = NULL;
    }
    if (first_error) record_error(st, first_error);
    return first_error;
}

int vita_tracy_pmu_sample_start(VitaTracyKernelState *st) {
    initialize();
    if (!st->shared) return VITA_TRACY_ERROR_STATE;
    if (__atomic_load_n(&g_recording, __ATOMIC_ACQUIRE)) return VITA_TRACY_OK;
    if (has_resources()) return VITA_TRACY_ERROR_BUSY;
    if (g_configured_pid != st->target_pid) {
        memset(&g_plan, 0, sizeof(g_plan));
        g_core_mask = 7u;
        g_frequency = 100u;
        g_configured_pid = st->target_pid;
    }
    record_error(st, 0);
    int ret = 0;
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (!(g_core_mask & (1u << i))) continue;
        PmuCpu *cpu = &g_cpus[i];
        cpu->state = st;
        cpu->ring = vita_trace_shared_pmu_ring(st->shared, i);
        cpu->sequence = 0;
        cpu->failed = 0;
        if (!cpu->ring) { ret = VITA_TRACY_ERROR_MAP; goto fail; }
        ret = vita_tracy_tick_prepare(&cpu->clock, g_frequency, 1u << i, on_pmu_tick, cpu);
        if (ret < 0) goto fail;
        ret = run_job(cpu, ACQUIRE);
        if (ret < 0) goto fail;
    }
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (!(g_core_mask & (1u << i))) continue;
        ret = vita_tracy_tick_arm(&g_cpus[i].clock);
        if (ret < 0) goto fail;
    }
    __atomic_store_n(&st->stats.pmu_active_mask, g_core_mask, __ATOMIC_RELEASE);
    __atomic_store_n(&g_recording, 1u, __ATOMIC_RELEASE);
    return VITA_TRACY_OK;
fail:
    record_error(st, ret);
    {
        int cleanup = vita_tracy_pmu_sample_stop(st);
        return cleanup < 0 ? cleanup : ret;
    }
}
