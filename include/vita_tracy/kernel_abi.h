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

int vitaTracyRegister(const VitaTracyRegisterArgs *args);
int vitaTracyUnregister(uint32_t target_pid);
int vitaTracySetSampling(const VitaTracySamplingConfig *cfg);
int vitaTracySetPmu(const VitaTracyPmuConfig *cfg);
int vitaTracySnapshotModules(uint32_t target_pid);
int vitaTracyGetStats(VitaTracyStats *stats);

/* One consumer waits for published batches. Notifications are retained
 * when they arrive before the wait; wake is also used for control requests.
 * timeout_us == 0 waits indefinitely. Only the attached process may call. */
int vitaTracyWaitForData(uint32_t timeout_us);
int vitaTracyWakeup(void);

/* Starts/stops a kernel-owned PMCCNTR sampler (kernel/pmu.c): a dedicated
 * kernel thread reads the cycle counter at PL1 and accumulates it into
 * VitaTracyStats.pmu_cycle_delta_total, entirely avoiding userland CP15
 * access. No config struct -- fixed to one core, cycle counter only,
 * for now. */
int vitaTracyPmuSampleStart(void);
int vitaTracyPmuSampleStop(void);

#ifdef __cplusplus
}
#endif
