#pragma once

#include <psp2kern/types.h>

#include "vita_tracy/abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/state.h"
#include "tick_source.h"
#include "vita_tracy/control_gate.h"

/* Bring-up tracing: kernel printf reaches the catlog TCP sink with a timestamp.
 * Thread context only; never from a timer callback or the raw IRQ node. */
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
#  include <psp2kern/kernel/debug.h>
#  define VITA_TRACY_TRACE(...) ksceDebugPrintf("vita-tracy: " __VA_ARGS__)
#else
#  define VITA_TRACY_TRACE(...) ((void)0)
#endif

typedef struct VitaTracyKernelState {
    VitaTracyState state;
    VitaTraceControlGate control;
    uint32_t shutdown_requested;

    SceUID target_pid;

    /* Mapping of the process-allocated shared block into kernel space. The
     * uid must outlive the pointer so it can be released on process death. */
    SceUID map_uid;
    void *shared;
    SceSize shared_size;

    SceUID proc_event_uid;

    SceUID sampler_thread;
    int sampler_should_run;
    uint32_t sampling_hz;
    uint32_t sampling_flags;
    uint32_t sampling_event_count;
    uint32_t sampling_events[VITA_TRACY_MAX_SAMPLE_EVENTS];
    VitaTracyTickSource sample_clock;
    SceUID data_event;
    /* Wake bits raised from the raw IRQ node, which must not touch threadmgr.
     * vitaTracyWaitForData drains them; it bounds its wait while that node
     * is live so no event is delayed for more than one poll interval. */
    uint32_t irq_pending_wake;
    uint32_t control_writer_lock;
    uint32_t sampler_backend;

    /* The thread that called vitaTracySetSampling, captured when the
     * sampler starts. The debug fallback sampler must never suspend it --
     * see kernel/sampler_debug_fallback.c. */
    SceUID control_thread;

    VitaTracyStats stats;
} VitaTracyKernelState;

enum {
    VITA_TRACY_SAMPLER_NONE = 0,
    VITA_TRACY_SAMPLER_PAMGR,
    VITA_TRACY_SAMPLER_SUSPEND,
    VITA_TRACY_SAMPLER_PMU_IRQ
};

VitaTracyKernelState *vita_tracy_state(void);

/* Kernel-side timestamp. Expressed in the ksceKernelGetSystemTimeWide
 * domain, which is not the ScePerf domain the client stamps zones with, so
 * clock sync records carry pairs for the PC to fit a transform. */
uint64_t vita_tracy_kernel_now(void);

/* Ring writers. Both drop rather than block, and bump the drop counters. */
void vita_tracy_emit_sample(VitaTracyKernelState *st, uint32_t cpu, const VitaTraceSample *sample);
void vita_tracy_emit_control(VitaTracyKernelState *st, const VitaTraceControlRecord *record);
void vita_tracy_notify(VitaTracyKernelState *st);
void vita_tracy_notify_events(VitaTracyKernelState *st, uint32_t events);

/* Releases the mapping and stops sampling. Safe to call repeatedly and
 * from the process-death path. */
int vita_tracy_detach(VitaTracyKernelState *st);
int vita_tracy_control_begin(VitaTracyKernelState *st);
void vita_tracy_control_end(VitaTracyKernelState *st);
void vita_tracy_target_exited(VitaTracyKernelState *st, SceUID pid);

int vita_tracy_modules_snapshot(VitaTracyKernelState *st, SceUID pid);

int vita_tracy_proc_events_register(VitaTracyKernelState *st);
int vita_tracy_proc_events_unregister(VitaTracyKernelState *st);

int vita_tracy_sampler_start(VitaTracyKernelState *st);
int vita_tracy_sampler_stop(VitaTracyKernelState *st);
int vita_tracy_sampler_diagnostic_start(VitaTracyKernelState *st);
int vita_tracy_sampler_diagnostic_stop(VitaTracyKernelState *st);
int vita_tracy_sampler_irq_start(VitaTracyKernelState *st);
int vita_tracy_sampler_irq_stop(VitaTracyKernelState *st);
int vita_tracy_sampler_irq_handler_registered(void);
int vita_tracy_sampler_irq_active(void);
void vita_tracy_sampler_irq_fill_diagnostics(void);
/* Thread context: refresh the target threads' stack ranges the IRQ node may
 * copy from. Cheap enough to call a few times a second. */
void vita_tracy_sampler_irq_refresh_stacks(void);
/* Record one thread's user stack range [lo, hi); tid 0 clears the table. */
void vita_tracy_sampler_irq_set_stack(uint32_t tid, uint32_t lo, uint32_t hi);
/* Scheduler on/off-CPU hooks recording the target's context switches into
 * the per-core switch rings; see sched_hooks.c. Stop before the shared
 * block goes away. */
int vita_tracy_sched_hooks_start(VitaTracyKernelState *st);
void vita_tracy_sched_hooks_stop(void);
/* IRQ context only; see user_read.c. Returns the words copied: the copy
 * stops at the first page a user read would fault on. */
uint32_t vita_tracy_read_user_words(uint32_t *dst, uint32_t user_src, uint32_t words);

int vita_tracy_pmu_configure(VitaTracyKernelState *st, const VitaTracyPmuConfig *cfg);

/* Whole-core IRQ PMU records; setup/restore helpers run only at boundaries. */
int vita_tracy_pmu_sample_start(VitaTracyKernelState *st);
int vita_tracy_pmu_sample_stop(VitaTracyKernelState *st);

/* Non-intrusive PC sampling. Returns VITA_TRACY_ERROR_UNSUPPORTED until a
 * source is found; see docs/reverse_engineering.md. */
int vita_tracy_sampler_pamgr_start(VitaTracyKernelState *st);
void vita_tracy_sampler_pamgr_stop(VitaTracyKernelState *st);
