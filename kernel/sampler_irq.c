#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/power.h>

#include "internal.h"
#include "irq_frame.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/pmu_overflow.h"

#ifndef VITA_TRACY_IRQ_CORE_MASK
#define VITA_TRACY_IRQ_CORE_MASK 7u
#endif
#if VITA_TRACY_IRQ_CORE_MASK == 0 || (VITA_TRACY_IRQ_CORE_MASK & ~7u)
#error "IRQ sampling must select at least one app core (bits 0..2)"
#endif
#define VITA_TRACY_IRQ_HANDLER_PRIORITY 0
#define IRQ_ADMISSION_OPEN 0x80000000u
#define IRQ_ADMISSION_ACTIVE 1u

const VitaPmuIo *vita_tracy_pmu_io(void);

enum { IRQ_PREPARE = 1, IRQ_ARM, IRQ_RELEASE };

typedef struct IrqCpu {
    VitaPmuOverflow overflow;
    uint32_t cpu;
    SceUID job;
    int operation;
    int result;
    int job_started;
    uint32_t failed;
    uint32_t admission;
} IrqCpu;

typedef struct IrqSampler {
    IrqCpu cpus[VITA_TRACE_CORE_COUNT];
    VitaTracyKernelState *state;
    uint32_t initialized;
    uint32_t registered;
    uint32_t service_enabled;
    uint32_t emit_enabled;
    uint32_t core_mask;
    uint32_t period_cycles;
} IrqSampler;

static IrqSampler g_irq;

/* The assembly entry builds our private frame and always tail-chains to the
 * next raw handler. It does not receive a Sony C exception-context pointer. */
extern uint32_t vita_tracy_irq_handler_node[];
void vita_tracy_irq_handler_c(const VitaTracyIrqFrame *context);
#ifndef __vita__
uint32_t vita_tracy_irq_handler_node[2];
#endif

static void initialize(void) {
    if (g_irq.initialized) return;
    memset(&g_irq, 0, sizeof(g_irq));
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        g_irq.cpus[i].cpu = i;
        g_irq.cpus[i].job = -1;
    }
    g_irq.initialized = 1;
}

static int has_resources(void) {
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        IrqCpu *cpu = &g_irq.cpus[i];
        if (cpu->job >= 0 || cpu->overflow.acquired) return 1;
    }
    return 0;
}

static int map_pmu_error(int ret) {
    if (ret == VITA_PMU_ERROR_BUSY) return VITA_TRACY_ERROR_BUSY;
    if (ret == VITA_PMU_ERROR_ARGS) return VITA_TRACY_ERROR_ARGS;
    if (ret == VITA_PMU_ERROR_OWNERSHIP) return VITA_TRACY_ERROR_STATE;
    return ret;
}

static void record_error(VitaTracyKernelState *st, int error) {
    __atomic_store_n(&st->stats.sample_irq_last_error, error, __ATOMIC_RELEASE);
}

static int core_job(SceSize size, void *arg) {
    if (size != sizeof(uint32_t)) return VITA_TRACY_ERROR_ARGS;
    const uint32_t index = *(const uint32_t *)arg;
    if (index >= VITA_TRACE_CORE_COUNT) return VITA_TRACY_ERROR_ARGS;
    IrqCpu *cpu = &g_irq.cpus[index];
    if ((uint32_t)ksceKernelCpuId() != index) {
        cpu->result = VITA_TRACY_ERROR_CPU;
        return 0;
    }

    SceKernelIntrStatus intr = ksceKernelCpuSuspendIntr();
    if (cpu->operation == IRQ_PREPARE) {
        cpu->result = map_pmu_error(vita_pmu_overflow_prepare(
            &cpu->overflow, vita_tracy_pmu_io(), g_irq.period_cycles));
    } else if (cpu->operation == IRQ_ARM) {
        cpu->result = map_pmu_error(vita_pmu_overflow_arm(
            &cpu->overflow, vita_tracy_pmu_io()));
    } else {
        cpu->result = map_pmu_error(vita_pmu_overflow_release(
            &cpu->overflow, vita_tracy_pmu_io()));
    }
    ksceKernelCpuResumeIntr(intr);
    return 0;
}

static int finish_job(IrqCpu *cpu) {
    if (cpu->job < 0) return 0;
    if (cpu->job_started) {
        int ret = ksceKernelWaitThreadEnd(cpu->job, NULL, NULL);
        if (ret < 0) return ret;
    }
    int ret = ksceKernelDeleteThread(cpu->job);
    if (ret < 0) return ret;
    cpu->job = -1;
    cpu->job_started = 0;
    return 0;
}

static int run_job(IrqCpu *cpu, int operation) {
    int ret = finish_job(cpu);
    if (ret < 0) return ret;

    cpu->operation = operation;
    cpu->result = VITA_TRACY_ERROR_STATE;
    SceUID thread = ksceKernelCreateThread("VitaTracyIrqSetup", core_job, 0x10000100,
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

static int register_handler(VitaTracyKernelState *st) {
    if (__atomic_load_n(&g_irq.registered, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&st->stats.sample_irq_handler_registered, 1u, __ATOMIC_RELEASE);
        return vita_tracy_irq_handler_node[0] ? 0 : VITA_TRACY_ERROR_STATE;
    }
    int ret = ksceExcpmgrRegisterHandler(SCE_EXCP_IRQ, VITA_TRACY_IRQ_HANDLER_PRIORITY,
                                         vita_tracy_irq_handler_node);
    if (ret < 0) return ret;
    __atomic_store_n(&g_irq.registered, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&st->stats.sample_irq_handler_registered, 1u, __ATOMIC_RELEASE);
    /* Success pins this node even if its predecessor is unexpectedly absent.
     * Do not arm a PMU source when tail-chaining has nowhere valid to go. */
    if (!vita_tracy_irq_handler_node[0]) return VITA_TRACY_ERROR_STATE;
    return 0;
}

static void handle_irq(uint32_t cpu_id, const VitaTracyIrqFrame *context) {
    if (!__atomic_load_n(&g_irq.service_enabled, __ATOMIC_ACQUIRE)) return;

    VitaTracyKernelState *st = g_irq.state;
    if (!st) return;
    IrqCpu *cpu = &g_irq.cpus[cpu_id];
    __atomic_fetch_add(&st->stats.sample_irq_calls[cpu_id], 1u, __ATOMIC_RELAXED);
    if (!cpu->overflow.acquired || cpu->failed) return;

    int ret = vita_pmu_overflow_service(&cpu->overflow, vita_tracy_pmu_io());
    if (ret < 0) {
        cpu->failed = 1;
        record_error(st, map_pmu_error(ret));
        vita_tracy_notify_events(st, VITA_TRACY_WAKE_SAMPLE_IRQ(cpu_id));
        return;
    }
    if (!ret) return;

    __atomic_fetch_add(&st->stats.sample_irq_overflows[cpu_id], 1u, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&g_irq.emit_enabled, __ATOMIC_ACQUIRE)) return;
    if (!context) {
        __atomic_fetch_add(&st->stats.sample_irq_context_errors[cpu_id], 1u, __ATOMIC_RELAXED);
        return;
    }

    SceKernelThreadContextInfo info;
    if (ksceKernelGetThreadContextInfo(&info) < 0) {
        __atomic_fetch_add(&st->stats.sample_irq_context_errors[cpu_id], 1u, __ATOMIC_RELAXED);
        return;
    }
    if (info.process_id != st->target_pid) {
        __atomic_fetch_add(&st->stats.sample_irq_not_target[cpu_id], 1u, __ATOMIC_RELAXED);
        return;
    }

    /* IRQ entry preserves the interrupted CPSR in SPSR. Only PL0 user mode is
     * useful as an application PC; syscall/kernel samples must not be charged
     * to the user function that happened to issue them. */
    if ((context->spsr & 0x1Fu) != 0x10u) {
        __atomic_fetch_add(&st->stats.sample_irq_kernel[cpu_id], 1u, __ATOMIC_RELAXED);
        return;
    }

    /* ARM exception entry sets LR_irq to resume-PC + 4 in both ARM and Thumb
     * state. Unlike a C callback's LR, the assembly frame contains that bank. */
    const uint32_t pc = context->irq_lr - 4u;
    const uint32_t alignment = (context->spsr & (1u << 5)) ? 1u : 3u;
    if (context->irq_lr <= 4u || (pc & alignment) || (context->spsr & (1u << 24))) {
        __atomic_fetch_add(&st->stats.sample_irq_context_errors[cpu_id], 1u, __ATOMIC_RELAXED);
        return;
    }

    VitaTraceSample sample;
    memset(&sample, 0, sizeof(sample));
    sample.timestamp = vita_tracy_kernel_now();
    sample.pid = (uint32_t)info.process_id;
    sample.tid = (uint32_t)info.thread_id; /* Global GUID; resolved outside IRQ. */
    sample.pc = pc;
    sample.sp = context->sp;
    sample.lr = context->lr;
    sample.cpu = (uint16_t)cpu_id;
    sample.flags = VITA_TRACE_SAMPLE_PMU_IRQ | VITA_TRACE_SAMPLE_GLOBAL_TID;
    if (context->spsr & (1u << 5)) sample.flags |= VITA_TRACE_SAMPLE_THUMB;
    vita_tracy_emit_sample(st, cpu_id, &sample);
    vita_tracy_notify(st);
}

void vita_tracy_irq_handler_c(const VitaTracyIrqFrame *context) {
    const uint32_t cpu_id = (uint32_t)ksceKernelCpuId();
    if (cpu_id >= VITA_TRACE_CORE_COUNT) return;
    IrqCpu *cpu = &g_irq.cpus[cpu_id];
    uint32_t expected = IRQ_ADMISSION_OPEN;
    /* One producer per core; no spinning in an exception. Closing admission
     * and marking a callback active share one atomic operation with stop(). */
    if (!__atomic_compare_exchange_n(&cpu->admission, &expected,
            IRQ_ADMISSION_OPEN | IRQ_ADMISSION_ACTIVE, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    handle_irq(cpu_id, context);
    __atomic_fetch_and(&cpu->admission, ~IRQ_ADMISSION_ACTIVE, __ATOMIC_RELEASE);
}

int vita_tracy_sampler_irq_start(VitaTracyKernelState *st) {
    initialize();
    if (!st || !st->shared || !st->sampling_hz) return VITA_TRACY_ERROR_STATE;
    if (__atomic_load_n(&g_irq.service_enabled, __ATOMIC_ACQUIRE) || has_resources())
        return VITA_TRACY_ERROR_BUSY;

    const int arm_mhz = kscePowerGetArmClockFrequency();
    if (arm_mhz <= 0) return VITA_TRACY_ERROR_UNSUPPORTED;
    const uint64_t period = ((uint64_t)(uint32_t)arm_mhz * 1000000ull) / st->sampling_hz;
    if (!period || period > UINT32_MAX) return VITA_TRACY_ERROR_ARGS;

    g_irq.state = st;
    g_irq.core_mask = VITA_TRACY_IRQ_CORE_MASK;
    g_irq.period_cycles = (uint32_t)period;
    __atomic_store_n(&st->stats.sample_irq_arm_mhz, (uint32_t)arm_mhz, __ATOMIC_RELEASE);
    __atomic_store_n(&st->stats.sample_irq_core_mask, g_irq.core_mask, __ATOMIC_RELEASE);
    record_error(st, 0);

    int ret = 0;
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (!(g_irq.core_mask & (1u << i))) continue;
        g_irq.cpus[i].failed = 0;
        ret = run_job(&g_irq.cpus[i], IRQ_PREPARE);
        if (ret < 0) goto fail_before_handler;
    }

    ret = register_handler(st);
    if (ret < 0) goto fail_before_handler;

    /* From this point the plugin is intentionally non-unloadable until reboot:
     * the public Excpmgr API has no unregister operation. */
    __atomic_store_n(&g_irq.service_enabled, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_irq.emit_enabled, 0u, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (g_irq.core_mask & (1u << i))
            __atomic_store_n(&g_irq.cpus[i].admission, IRQ_ADMISSION_OPEN, __ATOMIC_RELEASE);
    }
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (!(g_irq.core_mask & (1u << i))) continue;
        ret = run_job(&g_irq.cpus[i], IRQ_ARM);
        if (ret < 0) goto fail_after_handler;
    }
    __atomic_store_n(&g_irq.emit_enabled, 1u, __ATOMIC_RELEASE);
    return VITA_TRACY_OK;

fail_after_handler:
    record_error(st, ret);
    __atomic_store_n(&g_irq.emit_enabled, 0u, __ATOMIC_RELEASE);
    {
        int cleanup = vita_tracy_sampler_irq_stop(st);
        return cleanup < 0 ? cleanup : ret;
    }

fail_before_handler:
    record_error(st, ret);
    {
        int cleanup_error = 0;
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        IrqCpu *cpu = &g_irq.cpus[i];
        int cleanup = finish_job(cpu);
        if (cleanup >= 0 && cpu->overflow.acquired) cleanup = run_job(cpu, IRQ_RELEASE);
        if (cleanup < 0 && !cleanup_error) cleanup_error = cleanup;
    }
    if (!has_resources()) g_irq.state = NULL;
        return cleanup_error ? cleanup_error : ret;
    }
}

int vita_tracy_sampler_irq_stop(VitaTracyKernelState *st) {
    initialize();
    if (g_irq.state && st != g_irq.state) return VITA_TRACY_ERROR_TARGET;

    __atomic_store_n(&g_irq.emit_enabled, 0u, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (__atomic_load_n(&g_irq.cpus[i].admission, __ATOMIC_ACQUIRE) & IRQ_ADMISSION_ACTIVE) {
            record_error(st, VITA_TRACY_ERROR_BUSY);
            return VITA_TRACY_ERROR_BUSY;
        }
    }
    int first_error = 0;
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        IrqCpu *cpu = &g_irq.cpus[i];
        int ret = finish_job(cpu);
        if (ret >= 0 && cpu->overflow.acquired) ret = run_job(cpu, IRQ_RELEASE);
        if (ret == VITA_TRACY_ERROR_STATE && !cpu->overflow.acquired) {
            record_error(st, ret);
            ret = 0; /* Ownership was lost; do not overwrite it, but nothing remains ours. */
        }
        if (ret < 0 && !first_error) first_error = ret;
    }

    if (!has_resources()) {
        /* First disable/restore each bank on its own core with local IRQs
         * masked. Keep servicing overflows if that failed, without emitting.
         * Once no bank is armed, close admission and retain shared state if
         * a callback entered while the per-core jobs were finishing. */
        uint32_t active = 0;
        for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i)
            active |= __atomic_fetch_and(&g_irq.cpus[i].admission,
                ~IRQ_ADMISSION_OPEN, __ATOMIC_ACQ_REL) & IRQ_ADMISSION_ACTIVE;
        if (active) {
            record_error(st, VITA_TRACY_ERROR_BUSY);
            return VITA_TRACY_ERROR_BUSY;
        }
        __atomic_store_n(&g_irq.service_enabled, 0u, __ATOMIC_RELEASE);
        g_irq.state = NULL;
        g_irq.core_mask = 0;
        g_irq.period_cycles = 0;
    }
    if (first_error && st) record_error(st, first_error);
    return first_error;
}

int vita_tracy_sampler_irq_handler_registered(void) {
    initialize();
    return __atomic_load_n(&g_irq.registered, __ATOMIC_ACQUIRE) != 0;
}

#ifdef VITA_TRACY_TESTING
void vita_tracy_sampler_irq_test_reset(void) {
    memset(&g_irq, 0, sizeof(g_irq));
    initialize();
}
#endif
