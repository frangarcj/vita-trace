#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_TRACY_ABI_VERSION 6u

/* Suspend/read/resume is a diagnostic, not CPU-time sampling. Never enable
 * it as an implicit fallback when an interrupt sampler is unavailable. */
#define VITA_TRACY_SAMPLING_ALLOW_SUSPEND 1u
#define VITA_TRACY_SAMPLING_PMU_IRQ 2u
/* With PMU_IRQ only: register the handler and service overflows, count them
 * in the stats, but never build or emit a sample. Hardware bring-up aid. */
#define VITA_TRACY_SAMPLING_IRQ_COUNT_ONLY 4u
/* With PMU_IRQ only: prepare the banks and register the raw IRQ node, but
 * never arm a PMU overflow source. Every IRQ on the selected cores then
 * passes through the node and is counted in sample_irq_calls; this checks
 * exception-chain integrity on its own. Implies COUNT_ONLY. */
#define VITA_TRACY_SAMPLING_IRQ_REGISTER_ONLY 8u
/* Bring-up knobs, only with COUNT_ONLY: skip the per-thread context programming
 * or skip arming the overflow source. Temporary. */
#define VITA_TRACY_SAMPLING_IRQ_SKIP_PROGRAM 0x10u
#define VITA_TRACY_SAMPLING_IRQ_SKIP_ARM 0x20u
#define VITA_TRACY_SAMPLING_IRQ_SPI244 0x40u      /* register/enable GIC SPI 244 (experimental) */
#define VITA_TRACY_SAMPLING_IRQ_INTEN_ONLY 0x80u
#define VITA_TRACY_SAMPLING_IRQ_SVC_NODE 0x100u   /* accepted for compatibility; the SVC node is always installed */  /* arm PMINTENSET.C but keep the job's counter disabled */
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

/* PMU_IRQ sampling can also count up to six ARM PMU events per thread. Each
 * sample then carries the events its thread raised during the one period of
 * cycles that sample stands for. Event codes are Cortex-A9 PMU numbers. */
#define VITA_TRACY_MAX_SAMPLE_EVENTS 6u

typedef struct VitaTracySamplingConfig {
    uint32_t size;
    uint32_t abi_version;
    uint32_t frequency_hz;
    uint32_t flags;
    uint32_t event_count; /* 0..VITA_TRACY_MAX_SAMPLE_EVENTS; PMU_IRQ only */
    uint32_t events[VITA_TRACY_MAX_SAMPLE_EVENTS];
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
    uint32_t core_mask;    /* raw bits 0..3; zero defaults to app cores 0..2 */
    uint32_t frequency_hz; /* zero defaults to 100 Hz; otherwise 10..1000 */
    VitaTracyPmuCounterConfig counters[VITA_TRACY_PMU_MAX_COUNTERS];
} VitaTracyPmuConfig;

typedef struct VitaTracyStats {
    uint32_t size;
    uint32_t abi_version;
    uint32_t samples_emitted[4]; /* per transport ring; diagnostic uses ring 0 */
    uint32_t samples_dropped[4]; /* not per-core CPU attribution in diagnostic mode */
    uint32_t control_dropped;
    uint32_t uptime_ms;

    /* Legacy low-32-bit totals across selected cores. Use PMU records with
     * their actual elapsed_us for rates, never ticks * a nominal period. */
    uint32_t pmu_cycle_delta_total;
    uint32_t pmu_sample_ticks;

    uint32_t pmu_busy_loop_cycles; /* Reserved, zero. No diagnostic busy loop. */
    uint32_t sampling_flags;
    uint32_t registry_incomplete_ticks;
    uint32_t sample_read_failures;
    uint32_t sample_resume_failures;
    uint32_t timer_ticks;
    uint32_t diagnostic_batches;
    uint32_t pmu_active_mask;
    int32_t pmu_last_error;
    uint32_t pmu_records[4];
    uint32_t pmu_dropped[4];
    uint32_t pmu_gaps[4];
    uint32_t pmu_wrong_cpu[4];
    uint32_t pmu_counter_errors[4];
    int32_t last_cleanup_error;
    uint32_t sample_irq_calls[4];
    uint32_t sample_irq_overflows[4];
    uint32_t sample_irq_not_target[4];
    uint32_t sample_irq_kernel[4];
    uint32_t sample_irq_context_errors[4];
    uint32_t sample_irq_arm_mhz;
    uint32_t sample_irq_core_mask;
    int32_t sample_irq_last_error;
    uint32_t sample_irq_handler_registered;
    /* Target threads whose PMU bank the IRQ node enabled on first sight,
     * e.g. threads created after sampling started. */
    uint32_t sample_irq_adopted[4];
    /* Overflows found only after PMOVSR had been cleared under us; the
     * counter was reloaded (and sampled when the wrap was recent). */
    uint32_t sample_irq_missed[4];
} VitaTracyStats;

#ifdef __cplusplus
}
#endif
