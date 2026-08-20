#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_TRACY_ABI_VERSION 1u

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
    uint32_t samples_emitted[4]; /* per core */
    uint32_t samples_dropped[4]; /* per core */
    uint32_t control_dropped;
    uint32_t uptime_ms;
} VitaTracyStats;

#ifdef __cplusplus
}
#endif
