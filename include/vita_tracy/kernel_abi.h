#pragma once

#include <stdint.h>

#include "vita_tracy/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Control interface exported by tracy_kernel.skprx as syscalls.
 *
 * These are this project's own names, not Sony APIs. Only control calls go
 * through syscalls; high-frequency events travel through the shared rings
 * so the hot path never crosses the boundary. */

#define VITA_TRACY_OK 0
#define VITA_TRACY_ERROR_ABI (-1)          /* size/abi_version mismatch */
#define VITA_TRACY_ERROR_ARGS (-2)         /* malformed or unreadable arguments */
#define VITA_TRACY_ERROR_STATE (-3)        /* not legal in the current state */
#define VITA_TRACY_ERROR_MAP (-4)          /* the ring could not be mapped */
#define VITA_TRACY_ERROR_TARGET (-5)       /* pid is not the attached target */
#define VITA_TRACY_ERROR_UNSUPPORTED (-6)  /* needs a source that is not implemented yet */
#define VITA_TRACY_ERROR_BUSY (-7)         /* ownership or callback has not quiesced */
#define VITA_TRACY_ERROR_CPU (-8)          /* IRQ routed to a different CPU than requested */

int vitaTracyRegister(const VitaTracyRegisterArgs *args);
int vitaTracyUnregister(uint32_t target_pid);
int vitaTracySetSampling(const VitaTracySamplingConfig *cfg);
int vitaTracySetPmu(const VitaTracyPmuConfig *cfg);
int vitaTracySnapshotModules(uint32_t target_pid);
int vitaTracyGetStats(VitaTracyStats *stats);

/* Positive wake bits; PMU failure bits are independent of potentially full
 * sample/control rings. A wake may contain more than one core's failure. */
#define VITA_TRACY_WAKE_DATA 1u
#define VITA_TRACY_WAKE_PMU_CPU(cpu) (1u << (4u + (cpu)))
#define VITA_TRACY_WAKE_PMU_COUNTER(cpu) (1u << (8u + (cpu)))
#define VITA_TRACY_WAKE_ALL 0xFF1u

/* One consumer waits for published batches. Notifications are retained
 * when they arrive before the wait; wake is also used for control requests.
 * Returns a nonnegative VITA_TRACY_WAKE_* bit mask or a negative error.
 * timeout_us == 0 waits indefinitely. Only the attached process may call. */
int vitaTracyWaitForData(uint32_t timeout_us);
int vitaTracyWakeup(void);

/* Timer callbacks read whole-core counters at PL1. SetPmu selects the
 * configuration while stopped; the default is cycles on cores 0..2 at
 * 100 Hz. BUSY means resources or an existing PMU owner prevented access. */
int vitaTracyPmuSampleStart(void);
int vitaTracyPmuSampleStop(void);

#ifdef __cplusplus
}
#endif
