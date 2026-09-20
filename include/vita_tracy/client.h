#pragma once

#include <stdint.h>
#include "vita_tracy/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the modules the Tracy client needs and caches the timebase.
 *
 * SceNet is not touched here: VitaSDK's socket layer initializes it lazily
 * on the first socket() call and tolerates an application that already
 * called sceNetInit, so the client never owns or tears down the network.
 *
 * Always succeeds and always starts the profiler: ScePerf is optional and
 * its absence only costs clock resolution. Returns 0.
 *
 * Query vita_tracy_perf_module_status() for what happened to ScePerf. */
int vita_tracy_init(void);

/* 0 if ScePerf was already loaded or loaded cleanly, else the sceSysmodule
 * error. Negative means the profiler is running on the coarser clock. */
int vita_tracy_perf_module_status(void);

/* Unloads only what vita_tracy_init loaded. */
void vita_tracy_shutdown(void);
/* Reports failed detach/module unload. Safe to retry; never destroys Tracy
 * while the bridge might still enqueue. Producers must already be stopped. */
int vita_tracy_shutdown_checked(void);

/* Raw ScePerf timebase, the clock domain Tracy timestamps and kernel
 * backend events are both expressed in. */
uint64_t vita_tracy_timebase_value(void);
uint32_t vita_tracy_timebase_frequency(void);

/* Whether the clock ended up on ScePerf. When 0 the profiler is running on
 * the microsecond process timer instead, which is always available but far
 * coarser. */
int vita_tracy_timebase_use_perf(void);
int vita_tracy_timebase_adopt_perf(int perf_module_resident);

/* Connects to tracy_kernel.skprx: allocates the shared block, registers it,
 * calibrates the two clocks and starts draining the rings into Tracy.
 *
 * The caller must already know tracy_kernel.skprx is loaded. The control
 * ABI is imported weakly so an application without the plugin still starts,
 * but an import the loader could not bind is patched to branch to address
 * zero, and reading the stub cannot distinguish that from a real one. There
 * is therefore no safe probe: calling this without the plugin present ends
 * the process.
 *
 * `samples_per_core` and `control_capacity` are slot counts and must be
 * powers of two. Passing 0 uses the defaults.
 *
 * Returns 0 on success, or a negative VITA_TRACY_ERROR_* / Sce error. */
int vita_tracy_kernel_attach(uint32_t samples_per_core, uint32_t control_capacity);

/* Stops draining, unregisters and frees the shared block. */
void vita_tracy_kernel_detach(void);
/* Same operation, reporting a shutdown failure. Resources are retained on
 * failure rather than freed while a kernel producer could still use them. */
int vita_tracy_kernel_detach_checked(void);

/* Requests kernel sampling at the given rate. 0 stops sampling. */
int vita_tracy_kernel_set_sampling(uint32_t frequency_hz);
/* Select whole-core PMU counters while stopped. Zero mask/rate use cores
 * 0..2 at 100 Hz. No per-thread attribution or userland CP15 access. */
int vita_tracy_kernel_configure_pmu(const VitaTracyPmuConfig *config);
/* Explicit opt-in to intrusive diagnostics; see VITA_TRACY_SAMPLING_* in
 * abi.h. Control runs on the bridge worker, never permanently excluding main. */
int vita_tracy_kernel_set_sampling_ex(uint32_t frequency_hz, uint32_t flags);
int vita_tracy_kernel_get_stats(VitaTracyStats *stats);
int vita_tracy_kernel_pmu_sample_start(void);
int vita_tracy_kernel_pmu_sample_stop(void);

#ifdef __cplusplus
}
#endif
