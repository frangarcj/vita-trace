#include <psp2kern/kernel/cpu.h>
#if defined(__vita__)
#include <psp2kern/kernel/intrmgr.h>
#endif
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/power.h>

#include "internal.h"
#include "irq_frame.h"
#include "firmware_exports.h"
#include "pmu_thread_ctx.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/pmu_overflow.h"

#ifndef VITA_TRACY_IRQ_CORE_MASK
#define VITA_TRACY_IRQ_CORE_MASK 7u
#endif
#if VITA_TRACY_IRQ_CORE_MASK == 0 || (VITA_TRACY_IRQ_CORE_MASK & ~7u)
#error "IRQ sampling must select at least one app core (bits 0..2)"
#endif
#define VITA_TRACY_IRQ_HANDLER_PRIORITY 0
/* Thread context only. The 200 ms pause lets catlog flush the line before the
 * next step, so a freeze in that step cannot swallow it. */
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
#  define IRQ_TRACE(...) do { VITA_TRACY_TRACE(__VA_ARGS__); ksceKernelDelayThread(20000); } while (0)
#  define IRQ_SLEEP_US(us) ksceKernelDelayThread(us)
#  define IRQ_STATUS(pid, tag) vita_tracy_pmu_ctx_status((pid), (tag))
#else
#  define IRQ_TRACE(...) ((void)0)
#  define IRQ_SLEEP_US(us) ((void)0)
#  define IRQ_STATUS(pid, tag) ((void)0)
#endif
#define IRQ_ADMISSION_OPEN 0x80000000u
#define IRQ_ADMISSION_ACTIVE 1u

const VitaPmuIo *vita_tracy_pmu_io(void);

enum { IRQ_PREPARE = 1, IRQ_ARM, IRQ_RELEASE, IRQ_PREPARE_ARM };

typedef struct IrqCpu {
    VitaPmuOverflow overflow;
    uint32_t cpu;
    SceUID job;
    int operation;
    int result;
    int job_started;
    uint32_t failed;
    uint32_t admission;
    VitaTraceSample scratch; /* one producer per core; keeps the record off the IRQ stack */
} IrqCpu;

typedef struct IrqSampler {
    IrqCpu cpus[VITA_TRACE_CORE_COUNT];
    VitaTracyKernelState *state;
    uint32_t initialized;
    uint32_t registered;
    uint32_t svc_registered;
    uint32_t service_enabled;
    uint32_t emit_enabled;
    uint32_t adopt_enabled;
    uint32_t core_mask;
    uint32_t period_cycles;
    uint32_t contexts_programmed;
    /* PMU events counted per thread next to the cycle counter, in counters
     * 0..event_count-1; see take_events(). */
    uint32_t event_count;
    uint32_t events[VITA_TRACE_SAMPLE_EVENTS];
} IrqSampler;

_Static_assert(VITA_TRACE_SAMPLE_EVENTS == VITA_TRACY_MAX_SAMPLE_EVENTS, "sample and config event limits");
static IrqSampler g_irq;

static uint32_t event_mask(void) {
    return (1u << g_irq.event_count) - 1u;
}

/* The event counts in the live bank, i.e. the interrupted thread's, since
 * that thread's previous serviced overflow; zeroed again so that every
 * sample covers exactly one period of cycles. Called for every serviced
 * overflow, sampled or not. A bank without our event counters enabled
 * belongs to a thread we never programmed and is left alone. The raw nodes
 * run before intrmgr saves the bank, so the zeroes reach the thread's
 * context. */
static uint32_t take_events(uint32_t *out) {
    const uint32_t count = g_irq.event_count;
    if (!count) return 0;
    const VitaPmuIo *io = vita_tracy_pmu_io();
    const uint32_t mask = event_mask();
    if ((io->read(io->context, VITA_PMU_CNTEN) & mask) != mask) return 0;
    const uint32_t select = io->read(io->context, VITA_PMU_SELR);
    for (uint32_t i = 0; i < count; ++i) {
        io->write(io->context, VITA_PMU_SELR, i);
        const uint32_t value = io->read(io->context, VITA_PMU_VALUE);
        io->write(io->context, VITA_PMU_VALUE, 0u);
        if (out) out[i] = value;
    }
    io->write(io->context, VITA_PMU_SELR, select);
    return count;
}

/* User stack ranges of the target's threads, written from thread context and
 * read by the IRQ node. A thread's stack is one committed memblock (the Vita
 * does not page on demand), so a copy inside the range cannot fault. A slot
 * is published by writing its bounds before its tid and retired by clearing
 * the tid first; readers re-check the tid after reading the bounds. */
#define IRQ_STACK_SLOTS 64u
typedef struct IrqStackRange {
    uint32_t tid, lo, hi;
} IrqStackRange;
static IrqStackRange g_stacks[IRQ_STACK_SLOTS];

void vita_tracy_sampler_irq_set_stack(uint32_t tid, uint32_t lo, uint32_t hi) {
    if (!tid) {
        for (uint32_t i = 0; i < IRQ_STACK_SLOTS; ++i) __atomic_store_n(&g_stacks[i].tid, 0u, __ATOMIC_RELEASE);
        return;
    }
    uint32_t free_slot = IRQ_STACK_SLOTS;
    for (uint32_t i = 0; i < IRQ_STACK_SLOTS; ++i) {
        const uint32_t t = __atomic_load_n(&g_stacks[i].tid, __ATOMIC_ACQUIRE);
        if (t == tid) {
            if (g_stacks[i].lo == lo && g_stacks[i].hi == hi) return;
            __atomic_store_n(&g_stacks[i].tid, 0u, __ATOMIC_RELEASE);
            free_slot = i;
            break;
        }
        if (!t && free_slot == IRQ_STACK_SLOTS) free_slot = i;
    }
    if (free_slot == IRQ_STACK_SLOTS || hi <= lo) return;
    __atomic_store_n(&g_stacks[free_slot].lo, lo, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stacks[free_slot].hi, hi, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stacks[free_slot].tid, tid, __ATOMIC_RELEASE);
}

/* Returns the words copied; sets *cut when an unreadable page ended the copy
 * early. */
static uint32_t copy_user_stack(uint32_t tid, uint32_t sp, uint32_t *dst, int *cut) {
    *cut = 0;
    for (uint32_t i = 0; i < IRQ_STACK_SLOTS; ++i) {
        if (__atomic_load_n(&g_stacks[i].tid, __ATOMIC_ACQUIRE) != tid) continue;
        const uint32_t lo = __atomic_load_n(&g_stacks[i].lo, __ATOMIC_RELAXED);
        const uint32_t hi = __atomic_load_n(&g_stacks[i].hi, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_stacks[i].tid, __ATOMIC_ACQUIRE) != tid) return 0;
        if ((sp & 3u) || sp < lo || sp >= hi) return 0;
        /* The ends of the range the firmware reports may not be mapped; the
         * copy checks every page before reading it. */
        uint32_t words = (hi - sp) / 4u;
        if (words > VITA_TRACE_SAMPLE_STACK_WORDS) words = VITA_TRACE_SAMPLE_STACK_WORDS;
        const uint32_t copied = vita_tracy_read_user_words(dst, sp, words);
        *cut = copied < words;
        return copied;
    }
    return 0;
}

/* The Cortex-A9 PMU overflow line reaches the GIC as SPI 244 (the devkit
 * pamgr registered it with target mask 0xF). Retail registers nothing for
 * it, and intrmgr returns from an unhandled ID without EOI, which leaves the
 * core deaf to lower-priority interrupts until reboot. A no-op handler that
 * returns -1 keeps the line armed and lets intrmgr acknowledge it. Sampling
 * itself still happens in the raw priority-0 node, which runs first. */
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
#define VITA_TRACY_PMU_INTR_CODE 244
#define VITA_TRACY_PMU_INTR_PRIORITY 0xD0
static uint32_t g_pmu_intr_registered;

static __attribute__((unused)) int pmu_intr_handler(int unk, void *user_ctx) {
    (void)unk;
    (void)user_ctx;
    return -1; /* handled; keep enabled */
}

static __attribute__((unused)) int register_pmu_intr(void) {
    if (g_pmu_intr_registered) return 0;
    /* Target only the sampled cores: a delivery on a core whose node does
     * not service PMOVSR turns a level line into an interrupt storm. */
    int ret = ksceKernelRegisterIntrHandler(VITA_TRACY_PMU_INTR_CODE, "VitaTracyPmu", 0,
        pmu_intr_handler, NULL, VITA_TRACY_PMU_INTR_PRIORITY, (int)VITA_TRACY_IRQ_CORE_MASK, NULL);
    IRQ_TRACE("irq: register intr %d -> 0x%08X\n", VITA_TRACY_PMU_INTR_CODE, (unsigned)ret);
    if (ret < 0) return ret;
    g_pmu_intr_registered = 1;
    ret = ksceKernelEnableIntr(VITA_TRACY_PMU_INTR_CODE);
    IRQ_TRACE("irq: enable intr %d -> 0x%08X\n", VITA_TRACY_PMU_INTR_CODE, (unsigned)ret);
    return ret;
}

static __attribute__((unused)) void release_pmu_intr(void) {
    if (!g_pmu_intr_registered) return;
    int d = ksceKernelDisableIntr(VITA_TRACY_PMU_INTR_CODE);
    int r = ksceKernelReleaseIntrHandler(VITA_TRACY_PMU_INTR_CODE);
    IRQ_TRACE("irq: disable/release intr %d -> 0x%08X / 0x%08X\n", VITA_TRACY_PMU_INTR_CODE, (unsigned)d, (unsigned)r);
    if (r >= 0) g_pmu_intr_registered = 0;
}
#endif

/* Bring-up diagnostics gathered at IRQ entry on core 0, before Sony's
 * dispatcher touches the PMU: was the cycle counter enabled for the
 * interrupted user thread, and how far did it advance since the last IRQ? */
typedef struct IrqPmuProbe {
    uint32_t entries, user_entries, enabled_at_entry, pmcr_e_at_entry;
    uint32_t last_pmccntr, have_last, max_delta, sum_delta_lo;
    uint32_t last_pmcr, last_cnten, last_inten;
    uint32_t mode_usr, mode_svc, mode_sys, mode_irq, mode_other, last_spsr, last_cpsr;
    uint32_t ctx_ok, ctx_target_pid, ctx_watch_tid, ctx_watch_enabled;
    uint32_t ovsr_seen, ovsr_user;
    SceUID watch_tid;
} IrqPmuProbe;
static IrqPmuProbe g_probe;

#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
/* Bring-up (COUNT_ONLY): every 500 ms, the target's thread states and the
 * IRQ counters to catlog, to see what stops first when the app freezes. */
static SceUID g_monitor = -1;
static volatile uint32_t g_monitor_run;

static int monitor_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    uint32_t tick = 0;
    while (g_monitor_run) {
        VitaTracyKernelState *st = g_irq.state;
        if (st) {
            char tag[16];
            snprintf(tag, sizeof(tag), "t%u", (unsigned)tick);
            vita_tracy_pmu_ctx_status(st->target_pid, tag);
            VITA_TRACY_TRACE("mon t%u irq %u/%u/%u ovf %u/%u/%u emit %u/%u/%u err %d\n", (unsigned)tick,
                (unsigned)st->stats.sample_irq_calls[0], (unsigned)st->stats.sample_irq_calls[1],
                (unsigned)st->stats.sample_irq_calls[2], (unsigned)st->stats.sample_irq_overflows[0],
                (unsigned)st->stats.sample_irq_overflows[1], (unsigned)st->stats.sample_irq_overflows[2],
                (unsigned)st->stats.samples_emitted[0], (unsigned)st->stats.samples_emitted[1],
                (unsigned)st->stats.samples_emitted[2], (int)st->stats.sample_irq_last_error);
        }
        ++tick;
        ksceKernelDelayThread(500000);
    }
    return 0;
}

static void monitor_start(void) {
    if (g_monitor >= 0) return;
    g_monitor_run = 1;
    g_monitor = ksceKernelCreateThread("VitaTracyIrqMon", monitor_thread, 0x10000100, 0x2000, 0,
                                       (int)(0x10000u << 2), NULL);
    if (g_monitor >= 0 && ksceKernelStartThread(g_monitor, 0, NULL) < 0) {
        ksceKernelDeleteThread(g_monitor);
        g_monitor = -1;
    }
}

static void monitor_stop(void) {
    if (g_monitor < 0) return;
    g_monitor_run = 0;
    ksceKernelWaitThreadEnd(g_monitor, NULL, NULL);
    ksceKernelDeleteThread(g_monitor);
    g_monitor = -1;
}
#endif

/* The assembly entry builds our private frame and always tail-chains to the
 * next raw handler. It does not receive a Sony C exception-context pointer. */
extern uint32_t vita_tracy_irq_handler_node[];
extern uint32_t vita_tracy_svc_handler_node[];
void vita_tracy_irq_handler_c(const VitaTracyIrqFrame *context);
void vita_tracy_svc_handler_c(const VitaTracyIrqFrame *context);
#ifndef __vita__
uint32_t vita_tracy_irq_handler_node[2];
uint32_t vita_tracy_svc_handler_node[2];
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
    IRQ_TRACE("irq: job thread running on core %d for core %u\n", ksceKernelCpuId(), index);
    if ((uint32_t)ksceKernelCpuId() != index) {
        cpu->result = VITA_TRACY_ERROR_CPU;
        return 0;
    }

    SceKernelIntrStatus intr = ksceKernelCpuSuspendIntr();
    if (cpu->operation == IRQ_PREPARE) {
        cpu->result = map_pmu_error(vita_pmu_overflow_prepare(
            &cpu->overflow, vita_tracy_pmu_io(), g_irq.period_cycles));
        cpu->overflow.event_mask = event_mask();
    } else if (cpu->operation == IRQ_PREPARE_ARM) {
        cpu->result = map_pmu_error(vita_pmu_overflow_prepare(
            &cpu->overflow, vita_tracy_pmu_io(), g_irq.period_cycles));
        cpu->overflow.event_mask = event_mask();
        if (cpu->result == 0)
            cpu->result = map_pmu_error(vita_pmu_overflow_arm(&cpu->overflow, vita_tracy_pmu_io()));
        if (cpu->result == 0 && g_irq.state && (g_irq.state->sampling_flags & VITA_TRACY_SAMPLING_IRQ_INTEN_ONLY))
            vita_tracy_pmu_io()->write(vita_tracy_pmu_io()->context, VITA_PMU_CNTCLR, 0x80000000u);
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
        if (cpu->result == 0 && g_irq.state && index == 0 &&
            !(g_irq.state->sampling_flags & VITA_TRACY_SAMPLING_IRQ_SKIP_PROGRAM)) {
            /* Program the target threads' saved contexts from this core: the
             * ones pinned here are switched out while this job runs. */
            ksceKernelCpuResumeIntr(intr);
            int programmed = vita_tracy_pmu_ctx_program(g_irq.state->target_pid, cpu->overflow.preload,
                                                        g_irq.events, g_irq.event_count);
            IRQ_TRACE("irq: programmed %d target threads from core %u\n", programmed, index);
            g_irq.contexts_programmed = programmed >= 0;
            intr = ksceKernelCpuSuspendIntr();
        }
#endif
    } else if (cpu->operation == IRQ_ARM) {
        cpu->result = map_pmu_error(vita_pmu_overflow_arm(
            &cpu->overflow, vita_tracy_pmu_io()));
    } else {
        cpu->result = map_pmu_error(vita_pmu_overflow_release(
            &cpu->overflow, vita_tracy_pmu_io()));
        /* PMINTENSET is per-core, not per-thread: clear it here even when this
         * fresh thread's own bank made the ownership check fail. */
        vita_tracy_pmu_io()->write(vita_tracy_pmu_io()->context, VITA_PMU_INTCLR, 0x80000000u);
        cpu->overflow.acquired = 0;
        cpu->overflow.armed = 0;
        if (cpu->result == VITA_TRACY_ERROR_STATE) cpu->result = 0;
    }
    ksceKernelCpuResumeIntr(intr);
    IRQ_TRACE("irq: job op=%d on core %u -> %d\n", cpu->operation, index, cpu->result);
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
    IRQ_TRACE("irq: job op=%d core=%u create\n", operation, (unsigned)cpu->cpu);
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
    IRQ_TRACE("irq: job op=%d core=%u done wait=%d result=%d\n", operation,
        (unsigned)cpu->cpu, ret, cpu->result);
    return ret < 0 ? ret : cpu->result;
}

static int register_handler(VitaTracyKernelState *st) {
    if (__atomic_load_n(&g_irq.registered, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&st->stats.sample_irq_handler_registered, 1u, __ATOMIC_RELEASE);
        return vita_tracy_irq_handler_node[0] ? 0 : VITA_TRACY_ERROR_STATE;
    }
    IRQ_TRACE("irq: registering node %p (words %08X %08X)\n",
        (void *)vita_tracy_irq_handler_node, (unsigned)vita_tracy_irq_handler_node[0],
        (unsigned)vita_tracy_irq_handler_node[1]);
    int ret = vita_tracy_fw_register_handler(SCE_EXCP_IRQ, VITA_TRACY_IRQ_HANDLER_PRIORITY,
                                         vita_tracy_irq_handler_node);
    IRQ_TRACE("irq: register -> %d, next=%08X\n", ret, (unsigned)vita_tracy_irq_handler_node[0]);
    if (ret < 0) return ret;
    __atomic_store_n(&g_irq.registered, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&st->stats.sample_irq_handler_registered, 1u, __ATOMIC_RELEASE);
    /* Success pins this node even if its predecessor is unexpectedly absent.
     * Do not arm a PMU source when tail-chaining has nowhere valid to go. */
    if (!vita_tracy_irq_handler_node[0]) return VITA_TRACY_ERROR_STATE;
    return 0;
}

static __attribute__((unused)) void probe_at_entry(const VitaTracyIrqFrame *context) {
    const VitaPmuIo *io = vita_tracy_pmu_io();
    g_probe.entries++;
    const uint32_t cnten = io->read(io->context, VITA_PMU_CNTEN);
    const uint32_t pmcr = io->read(io->context, VITA_PMU_PMCR);
    g_probe.last_cnten = cnten;
    g_probe.last_pmcr = pmcr;
    g_probe.last_inten = io->read(io->context, VITA_PMU_INTEN);
    if (io->read(io->context, VITA_PMU_OVSR) & 0x80000000u) {
        g_probe.ovsr_seen++;
        if (context && (context->spsr & 0x1Fu) == 0x10u) g_probe.ovsr_user++;
    }
    if (g_irq.state) {
        /* Bring-up only: core 3 is never sampled, its slots carry diagnostics. */
        g_irq.state->stats.sample_irq_calls[3] = g_probe.user_entries;
        g_irq.state->stats.sample_irq_overflows[3] = g_probe.enabled_at_entry;
        g_irq.state->stats.sample_irq_kernel[3] = g_probe.last_inten;
        g_irq.state->stats.sample_irq_not_target[3] = g_probe.last_pmcr;
        g_irq.state->stats.sample_irq_context_errors[3] = g_probe.last_cnten;
        g_irq.state->stats.samples_emitted[3] = g_probe.ovsr_seen;
        g_irq.state->stats.samples_dropped[3] = g_probe.ovsr_user;
        g_irq.state->stats.pmu_records[3] = g_probe.sum_delta_lo;
        g_irq.state->stats.pmu_dropped[3] = g_probe.max_delta;
        g_irq.state->stats.pmu_gaps[2] = g_probe.ctx_watch_tid;
        g_irq.state->stats.pmu_wrong_cpu[2] = g_probe.ctx_watch_enabled;
    }
    if (context) {
        const uint32_t mode = context->spsr & 0x1Fu;
        g_probe.last_spsr = context->spsr;
        g_probe.last_cpsr = context->entry_cpsr;
        if (mode == 0x10u) g_probe.mode_usr++;
        else if (mode == 0x13u) g_probe.mode_svc++;
        else if (mode == 0x1Fu) g_probe.mode_sys++;
        else if (mode == 0x12u) g_probe.mode_irq++;
        else g_probe.mode_other++;
    }
    {
        SceKernelThreadContextInfo info;
        if (vita_tracy_fw_thread_context(&info) >= 0) {
            g_probe.ctx_ok++;
            if (g_irq.state && info.process_id == g_irq.state->target_pid) g_probe.ctx_target_pid++;
            if (g_probe.watch_tid && info.thread_id == g_probe.watch_tid) {
                g_probe.ctx_watch_tid++;
                if (cnten & 0x80000000u) g_probe.ctx_watch_enabled++;
            }
        }
    }
    if (context && (context->spsr & 0x1Fu) == 0x10u) {
        g_probe.user_entries++;
        if (cnten & 0x80000000u) g_probe.enabled_at_entry++;
        if (pmcr & 1u) g_probe.pmcr_e_at_entry++;
        const uint32_t now = io->read(io->context, VITA_PMU_CYCLES);
        if (g_probe.have_last) {
            const uint32_t d = now - g_probe.last_pmccntr;
            if (d > g_probe.max_delta) g_probe.max_delta = d;
            g_probe.sum_delta_lo += d;
        }
        g_probe.last_pmccntr = now;
        g_probe.have_last = 1;
    }
}

/* Threads created after start inherit PMCR.E from the process default but
 * not PMCNTENSET.C. This node runs before intrmgr saves the interrupted
 * thread's PMU bank, so enabling the counter in the live bank of a target
 * thread interrupted in user mode (where the bank is certainly its own) makes
 * intrmgr store it into that thread's context. A bank with any counter
 * enabled belongs to someone else's configuration and is left alone. */
static void adopt_thread(uint32_t cpu_id, IrqCpu *cpu, VitaTracyKernelState *st,
                         const VitaTracyIrqFrame *context) {
    if (!__atomic_load_n(&g_irq.adopt_enabled, __ATOMIC_ACQUIRE)) return;
    if (!context || (context->spsr & 0x1Fu) != 0x10u) return;
    const VitaPmuIo *io = vita_tracy_pmu_io();
    if (io->read(io->context, VITA_PMU_CNTEN) != 0) return;
    if (vita_trace_control_pending(&st->control)) return;
    SceKernelThreadContextInfo info;
    if (vita_tracy_fw_thread_context(&info) < 0 || info.process_id != st->target_pid) return;
    const uint32_t pmcr = io->read(io->context, VITA_PMU_PMCR);
    io->write(io->context, VITA_PMU_PMCR, (pmcr & ~0x3Fu) | 1u); /* E only: no D, X, DP, resets */
    io->write(io->context, VITA_PMU_OVSR, 0x80000000u);
    io->write(io->context, VITA_PMU_CYCLES, cpu->overflow.preload);
    if (g_irq.event_count) {
        const uint32_t select = io->read(io->context, VITA_PMU_SELR);
        for (uint32_t i = 0; i < g_irq.event_count; ++i) {
            io->write(io->context, VITA_PMU_SELR, i);
            io->write(io->context, VITA_PMU_TYPE, g_irq.events[i]);
            io->write(io->context, VITA_PMU_VALUE, 0u);
        }
        io->write(io->context, VITA_PMU_SELR, select);
    }
    io->write(io->context, VITA_PMU_CNTEN, 0x80000000u | event_mask());
    __atomic_fetch_add(&st->stats.sample_irq_adopted[cpu_id], 1u, __ATOMIC_RELAXED);
}

static void handle_irq(uint32_t cpu_id, const VitaTracyIrqFrame *context) {
    if (!__atomic_load_n(&g_irq.service_enabled, __ATOMIC_ACQUIRE)) return;
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
    if (cpu_id == 0) probe_at_entry(context); /* bring-up diagnostics only */
#endif

    VitaTracyKernelState *st = g_irq.state;
    if (!st) return;
    IrqCpu *cpu = &g_irq.cpus[cpu_id];
    __atomic_fetch_add(&st->stats.sample_irq_calls[cpu_id], 1u, __ATOMIC_RELAXED);
    /* The observer is installed before the per-core arm jobs finish. An
     * unrelated IRQ on a merely prepared bank is not an ownership failure. */
    if (!cpu->overflow.acquired || !cpu->overflow.armed || cpu->failed) return;

    int ret = vita_pmu_overflow_service(&cpu->overflow, vita_tracy_pmu_io());
    if (ret < 0) {
        cpu->failed = 1;
        record_error(st, map_pmu_error(ret));
        /* Raw priority-0 node: no threadmgr calls here (2026-09-22 hang). */
        __atomic_fetch_or(&st->irq_pending_wake, VITA_TRACY_WAKE_SAMPLE_IRQ(cpu_id), __ATOMIC_RELEASE);
        return;
    }
    if (!ret) {
        adopt_thread(cpu_id, cpu, st, context);
        return;
    }
    if (ret == VITA_PMU_OVERFLOW_MISSED_RECENT || ret == VITA_PMU_OVERFLOW_MISSED_STALE)
        __atomic_fetch_add(&st->stats.sample_irq_missed[cpu_id], 1u, __ATOMIC_RELAXED);
    uint32_t events[VITA_TRACE_SAMPLE_EVENTS];
    const uint32_t event_count = take_events(events);
    if (ret == VITA_PMU_OVERFLOW_MISSED_STALE) return;

    __atomic_fetch_add(&st->stats.sample_irq_overflows[cpu_id], 1u, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&g_irq.emit_enabled, __ATOMIC_ACQUIRE) ||
        vita_trace_control_pending(&st->control)) return;
    if (!context) {
        __atomic_fetch_add(&st->stats.sample_irq_context_errors[cpu_id], 1u, __ATOMIC_RELAXED);
        return;
    }

    SceKernelThreadContextInfo info;
    if (vita_tracy_fw_thread_context(&info) < 0) {
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

    if (!__atomic_load_n(&g_irq.emit_enabled, __ATOMIC_ACQUIRE) ||
        vita_trace_control_pending(&st->control)) return;

    VitaTraceSample *sample = &cpu->scratch;
    sample->timestamp = vita_tracy_kernel_now();
    sample->pid = (uint32_t)info.process_id;
    sample->tid = (uint32_t)info.thread_id; /* Global GUID; resolved outside IRQ. */
    sample->pc = pc;
    sample->sp = context->sp;
    sample->lr = context->lr;
    sample->cpu = (uint16_t)cpu_id;
    sample->flags = VITA_TRACE_SAMPLE_PMU_IRQ | VITA_TRACE_SAMPLE_GLOBAL_TID;
    if (context->spsr & (1u << 5)) sample->flags |= VITA_TRACE_SAMPLE_THUMB;
    sample->r7 = context->r[7];
    sample->r11 = context->r[11];
    sample->event_count = event_count;
    for (uint32_t i = 0; i < VITA_TRACE_SAMPLE_EVENTS; ++i)
        sample->events[i] = i < event_count ? events[i] : 0u;
    int cut;
    sample->stack_words = copy_user_stack((uint32_t)info.thread_id, context->sp, sample->stack, &cut);
    if (cut) sample->flags |= VITA_TRACE_SAMPLE_STACK_CUT;
    vita_tracy_emit_sample(st, cpu_id, sample);
    __atomic_fetch_or(&st->irq_pending_wake, VITA_TRACY_WAKE_DATA, __ATOMIC_RELEASE);
}

void vita_tracy_svc_handler_c(const VitaTracyIrqFrame *context) {
    (void)context;
    const uint32_t cpu_id = (uint32_t)ksceKernelCpuId();
    if (cpu_id >= VITA_TRACE_CORE_COUNT) return;
    if (!__atomic_load_n(&g_irq.service_enabled, __ATOMIC_ACQUIRE)) return;
    IrqCpu *cpu = &g_irq.cpus[cpu_id];
    uint32_t expected = IRQ_ADMISSION_OPEN;
    if (!__atomic_compare_exchange_n(&cpu->admission, &expected,
            IRQ_ADMISSION_OPEN | IRQ_ADMISSION_ACTIVE, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    VitaTracyKernelState *st = g_irq.state;
    if (st && cpu->overflow.acquired && cpu->overflow.armed && !cpu->failed) {
        int ret = vita_pmu_overflow_service(&cpu->overflow, vita_tracy_pmu_io());
        if (ret == VITA_PMU_OVERFLOW_MISSED_RECENT || ret == VITA_PMU_OVERFLOW_MISSED_STALE)
            __atomic_fetch_add(&st->stats.sample_irq_missed[cpu_id], 1u, __ATOMIC_RELAXED);
        if (ret > 0) take_events(NULL); /* this period is not sampled */
        if (ret > 0 && ret != VITA_PMU_OVERFLOW_MISSED_STALE) {
            __atomic_fetch_add(&st->stats.sample_irq_overflows[cpu_id], 1u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&st->stats.sample_irq_kernel[cpu_id], 1u, __ATOMIC_RELAXED); /* seen at a syscall: not attributable */
        }
    }
    __atomic_fetch_and(&cpu->admission, ~IRQ_ADMISSION_ACTIVE, __ATOMIC_RELEASE);
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

    IRQ_TRACE("irq: start hz=%u flags=0x%X mask=%u, reading ARM clock\n", (unsigned)st->sampling_hz,
        (unsigned)st->sampling_flags, (unsigned)VITA_TRACY_IRQ_CORE_MASK);
    const int arm_mhz = kscePowerGetArmClockFrequency();
    IRQ_TRACE("irq: arm_mhz=%d\n", arm_mhz);
    if (arm_mhz <= 0) return VITA_TRACY_ERROR_UNSUPPORTED;
    const uint64_t period = ((uint64_t)(uint32_t)arm_mhz * 1000000ull) / st->sampling_hz;
    if (!period || period > UINT32_MAX) return VITA_TRACY_ERROR_ARGS;

    g_irq.state = st;
    g_irq.core_mask = VITA_TRACY_IRQ_CORE_MASK;
    g_irq.period_cycles = (uint32_t)period;
    g_irq.event_count = st->sampling_event_count <= VITA_TRACE_SAMPLE_EVENTS ? st->sampling_event_count : 0u;
    for (uint32_t i = 0; i < VITA_TRACE_SAMPLE_EVENTS; ++i)
        g_irq.events[i] = i < g_irq.event_count ? st->sampling_events[i] : 0u;
    __atomic_store_n(&st->stats.sample_irq_arm_mhz, (uint32_t)arm_mhz, __ATOMIC_RELEASE);
    __atomic_store_n(&st->stats.sample_irq_core_mask, g_irq.core_mask, __ATOMIC_RELEASE);
    record_error(st, 0);

    int ret = 0;
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (!(g_irq.core_mask & (1u << i))) continue;
        g_irq.cpus[i].failed = 0;
    }

    /* Register first: a bank prepared in a short-lived job thread is that
     * thread's context, not the core's, so preparing before the node exists
     * proves nothing and would only corrupt the restore value. */
    ret = register_handler(st);
    if (ret < 0) goto fail_before_handler;

#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
    /* Not modelled by the host tests: only the IRQ node is registered there. */
    if (!g_irq.svc_registered) {
        IRQ_TRACE("irq: registering svc node %p\n", (void *)vita_tracy_svc_handler_node);
        int sret = vita_tracy_fw_register_handler(SCE_EXCP_SVC, VITA_TRACY_IRQ_HANDLER_PRIORITY,
                                                  vita_tracy_svc_handler_node);
        IRQ_TRACE("irq: svc register -> %d, next=%08X\n", sret, (unsigned)vita_tracy_svc_handler_node[0]);
        if (sret >= 0) g_irq.svc_registered = 1;
    }
#endif
    /* From this point the plugin is intentionally non-unloadable until reboot:
     * the public Excpmgr API has no unregister operation. */
    __atomic_store_n(&g_irq.service_enabled, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_irq.emit_enabled, 0u, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (g_irq.core_mask & (1u << i))
            __atomic_store_n(&g_irq.cpus[i].admission, IRQ_ADMISSION_OPEN, __ATOMIC_RELEASE);
    }
    if (st->sampling_flags & VITA_TRACY_SAMPLING_IRQ_REGISTER_ONLY) return VITA_TRACY_OK;
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
    if ((st->sampling_flags & VITA_TRACY_SAMPLING_IRQ_SPI244) &&
        !(st->sampling_flags & VITA_TRACY_SAMPLING_IRQ_SKIP_ARM)) {
        ret = register_pmu_intr();
        if (ret < 0) goto fail_after_handler;
    }
#endif
    /* Arming in a fresh job thread would fail the ownership check (the bank
     * it sees is that thread's own context), so prepare+arm run together. */
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        if (!(g_irq.core_mask & (1u << i))) continue;
        g_irq.cpus[i].overflow.acquired = 0;
        g_irq.cpus[i].overflow.armed = 0;
        if (st->sampling_flags & VITA_TRACY_SAMPLING_IRQ_SKIP_ARM) continue;
        ret = run_job(&g_irq.cpus[i], IRQ_PREPARE_ARM);
        if (ret < 0) goto fail_after_handler;
    }
    memset(&g_probe, 0, sizeof(g_probe));
    if (!(st->sampling_flags & VITA_TRACY_SAMPLING_IRQ_SKIP_PROGRAM))
        __atomic_store_n(&g_irq.adopt_enabled, 1u, __ATOMIC_RELEASE);
    vita_tracy_sampler_irq_refresh_stacks();
    /* The firmware keeps PMU state per thread: the interrupt source only
     * fires for threads whose saved context enables the cycle counter. */
    if (!(st->sampling_flags & VITA_TRACY_SAMPLING_IRQ_COUNT_ONLY))
        __atomic_store_n(&g_irq.emit_enabled, 1u, __ATOMIC_RELEASE);
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
    else
        monitor_start();
#endif
    return VITA_TRACY_OK;

fail_after_handler:
    record_error(st, ret);
    __atomic_store_n(&g_irq.emit_enabled, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_irq.adopt_enabled, 0u, __ATOMIC_RELEASE);
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
    __atomic_store_n(&g_irq.adopt_enabled, 0u, __ATOMIC_RELEASE);
    vita_tracy_sampler_irq_set_stack(0u, 0u, 0u);
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
    monitor_stop();
    release_pmu_intr();
    if (g_irq.contexts_programmed && st) {
        int rel = vita_tracy_pmu_ctx_release(st->target_pid);
        IRQ_TRACE("irq: target contexts released -> %d\n", rel);
        g_irq.contexts_programmed = 0;
    }
#endif
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
        g_irq.event_count = 0;
    }
    if (first_error && st) record_error(st, first_error);
    return first_error;
}

#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
void vita_tracy_sampler_irq_refresh_stacks(void) {
    VitaTracyKernelState *st = g_irq.state;
    if (!st || !__atomic_load_n(&g_irq.service_enabled, __ATOMIC_ACQUIRE)) return;
    SceUID ids[IRQ_STACK_SLOTS];
    int count = 0;
    if (ksceKernelGetThreadIdList(st->target_pid, ids, IRQ_STACK_SLOTS, &count) < 0) return;
    /* Retire threads that are gone, then (re)publish the live ones. */
    for (uint32_t i = 0; i < IRQ_STACK_SLOTS; ++i) {
        const uint32_t t = __atomic_load_n(&g_stacks[i].tid, __ATOMIC_ACQUIRE);
        if (!t) continue;
        int alive = 0;
        for (int j = 0; j < count; ++j) alive |= (uint32_t)ids[j] == t;
        if (!alive) __atomic_store_n(&g_stacks[i].tid, 0u, __ATOMIC_RELEASE);
    }
    for (int j = 0; j < count; ++j) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ksceKernelGetThreadInfo(ids[j], &info) < 0 || !info.stack || info.stackSize <= 0) continue;
        const uint32_t lo = (uint32_t)(uintptr_t)info.stack;
        vita_tracy_sampler_irq_set_stack((uint32_t)ids[j], lo, lo + (uint32_t)info.stackSize);
    }
}
#else
void vita_tracy_sampler_irq_refresh_stacks(void) {}
#endif

int vita_tracy_sampler_irq_active(void) {
    initialize();
    return __atomic_load_n(&g_irq.service_enabled, __ATOMIC_ACQUIRE) != 0;
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

#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
/* Bring-up: the watched thread's saved PMCCNTR/PMCNTENSET in both blocks. */
void vita_tracy_sampler_irq_fill_diagnostics(void) {
    if (!g_irq.state) return;
    if (!g_probe.watch_tid)
        g_probe.watch_tid = vita_tracy_pmu_ctx_find_thread(g_irq.state->target_pid, "bringup-spin-c0");
    if (!g_probe.watch_tid) return;
    uint32_t p = 0, t = 0, pc = 0, tc = 0;
    if (vita_tracy_pmu_ctx_peek(g_probe.watch_tid, &p, &t, &pc, &tc) == 0) {
        g_irq.state->stats.pmu_gaps[3] = p;
        g_irq.state->stats.pmu_wrong_cpu[3] = t;
        g_irq.state->stats.pmu_counter_errors[3] = pc;
        g_irq.state->stats.pmu_records[2] = tc; /* core 2 unused with mask 1 */
    }
}
#endif
