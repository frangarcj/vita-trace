#pragma once

#include <psp2kern/types.h>

#include "vita_tracy/abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/state.h"
#include "tick_source.h"

typedef struct VitaTracyKernelState {
    VitaTracyState state;

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

    /* The thread that called vitaTracySetSampling, captured when the
     * sampler starts. The debug fallback sampler must never suspend it --
     * see kernel/sampler_debug_fallback.c. */
    SceUID control_thread;

    /* Kernel-owned PMCCNTR sampler -- see vita_tracy_pmu_sample_start. */
    SceUID pmu_sample_thread;
    int pmu_sample_should_run;

    VitaTracyStats stats;
} VitaTracyKernelState;

VitaTracyKernelState *vita_tracy_state(void);

/* Kernel-side timestamp. Expressed in the ksceKernelGetSystemTimeWide
 * domain, which is not the ScePerf domain the client stamps zones with, so
 * clock sync records carry pairs for the PC to fit a transform. */
uint64_t vita_tracy_kernel_now(void);

/* Ring writers. Both drop rather than block, and bump the drop counters. */
void vita_tracy_emit_sample(VitaTracyKernelState *st, uint32_t cpu, const VitaTraceSample *sample);
void vita_tracy_emit_control(VitaTracyKernelState *st, const VitaTraceControlRecord *record);
void vita_tracy_notify(VitaTracyKernelState *st);

/* Releases the mapping and stops sampling. Safe to call repeatedly and
 * from the process-death path. */
void vita_tracy_detach(VitaTracyKernelState *st);

int vita_tracy_modules_snapshot(VitaTracyKernelState *st, SceUID pid);

int vita_tracy_proc_events_register(VitaTracyKernelState *st);
void vita_tracy_proc_events_unregister(VitaTracyKernelState *st);

int vita_tracy_sampler_start(VitaTracyKernelState *st);
void vita_tracy_sampler_stop(VitaTracyKernelState *st);

int vita_tracy_pmu_configure(VitaTracyKernelState *st, const VitaTracyPmuConfig *cfg);

/* Kernel-owned PMCCNTR sampler: reads the cycle counter at PL1 from a
 * dedicated thread, never touching userland CP15 access at all. */
int vita_tracy_pmu_sample_start(VitaTracyKernelState *st);
void vita_tracy_pmu_sample_stop(VitaTracyKernelState *st);

/* Non-intrusive PC sampling. Returns VITA_TRACY_ERROR_UNSUPPORTED until a
 * source is found; see docs/reverse_engineering.md. */
int vita_tracy_sampler_pamgr_start(VitaTracyKernelState *st);
void vita_tracy_sampler_pamgr_stop(VitaTracyKernelState *st);
