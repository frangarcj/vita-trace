#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_TRACY_ABI_VERSION 2u

/* Suspend/read/resume is a diagnostic, not CPU-time sampling. Never enable
 * it as an implicit fallback when an interrupt sampler is unavailable. */
#define VITA_TRACY_SAMPLING_ALLOW_SUSPEND 1u
#define VITA_TRACY_MAX_SAMPLE_HZ 1000u

/* Every request struct starts with size + abi_version so the receiver can
 * reject mismatched builds instead of misreading fields. */

typedef struct VitaTracyRegisterArgs {
    uint32_t size;
    uint32_t abi_version;
    uint32_t target_pid;
    uint32_t ring_user_addr; /* 32-bit VA in the target process */
    uint32_t ring_size;
    uint32_t flags;
} VitaTracyRegisterArgs;

typedef struct VitaTracySamplingConfig {
    uint32_t size;
    uint32_t abi_version;
    uint32_t frequency_hz;
    uint32_t flags;
} VitaTracySamplingConfig;

#define VITA_TRACY_PMU_MAX_COUNTERS 8u

typedef struct VitaTracyPmuCounterConfig {
    uint32_t counter;
    uint32_t event_code;
} VitaTracyPmuCounterConfig;

typedef struct VitaTracyPmuConfig {
    uint32_t size;
    uint32_t abi_version;
    uint32_t target_tid;
    uint32_t counter_count;
    VitaTracyPmuCounterConfig counters[VITA_TRACY_PMU_MAX_COUNTERS];
} VitaTracyPmuConfig;

typedef struct VitaTracyStats {
    uint32_t size;
    uint32_t abi_version;
    uint32_t samples_emitted[4]; /* per transport ring; diagnostic uses ring 0 */
    uint32_t samples_dropped[4]; /* not per-core CPU attribution in diagnostic mode */
    uint32_t control_dropped;
    uint32_t uptime_ms;

    /* Kernel-owned PMU cycle-count sampling (vitaTracyPmuSampleStart/Stop):
     * a dedicated kernel thread, pinned to one core, reads PMCCNTR at PL1
     * and accumulates deltas here -- userland never touches CP15 PMU
     * registers directly, sidestepping the PMUSERENR-visibility puzzle in
     * kernel/pmu.c's file comment (PL1 doesn't need PMUSERENR at all). */
    uint32_t pmu_cycle_delta_total;
    uint32_t pmu_sample_ticks;

    /* One-shot: PMCCNTR delta over a fixed busy loop with no sleep in
     * between, measured entirely in kernel context right as the sampler
     * starts -- isolates "does the counter advance during active
     * execution" from "does it advance across mostly-idle wall-clock
     * time", which pmu_cycle_delta_total alone cannot distinguish. */
    uint32_t pmu_busy_loop_cycles;
    uint32_t sampling_flags;
    uint32_t registry_incomplete_ticks;
    uint32_t sample_read_failures;
    uint32_t sample_resume_failures;
    uint32_t timer_ticks;
    uint32_t diagnostic_batches;
} VitaTracyStats;

#ifdef __cplusplus
}
#endif
