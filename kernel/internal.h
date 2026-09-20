#pragma once

#include <psp2kern/types.h>

#include "vita_tracy/abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/state.h"
#include "tick_source.h"
#include "vita_tracy/control_gate.h"

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
    VitaTracyTickSource sample_clock;
    SceUID data_event;
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
int vita_tracy_sampler_irq_start(VitaTracyKernelState *st);
int vita_tracy_sampler_irq_stop(VitaTracyKernelState *st);
int vita_tracy_sampler_irq_handler_registered(void);

int vita_tracy_pmu_configure(VitaTracyKernelState *st, const VitaTracyPmuConfig *cfg);

/* Whole-core IRQ PMU records; setup/restore helpers run only at boundaries. */
int vita_tracy_pmu_sample_start(VitaTracyKernelState *st);
int vita_tracy_pmu_sample_stop(VitaTracyKernelState *st);

/* Non-intrusive PC sampling. Returns VITA_TRACY_ERROR_UNSUPPORTED until a
 * source is found; see docs/reverse_engineering.md. */
int vita_tracy_sampler_pamgr_start(VitaTracyKernelState *st);
void vita_tracy_sampler_pamgr_stop(VitaTracyKernelState *st);
