#pragma once

#include <stdint.h>

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

/* Raw ScePerf timebase, the clock domain Tracy timestamps and kernel
 * backend events are both expressed in. */
uint64_t vita_tracy_timebase_value(void);
uint32_t vita_tracy_timebase_frequency(void);

/* Whether the clock ended up on ScePerf. When 0 the profiler is running on
 * the microsecond process timer instead, which is always available but far
 * coarser. */
int vita_tracy_timebase_use_perf(void);
int vita_tracy_timebase_adopt_perf(void);

/* Connects to tracy_kernel.skprx: allocates the shared block, registers it,
 * calibrates the two clocks and starts draining the rings into Tracy.
 *
 * The kernel backend is optional. When the plugin is not loaded this fails
 * and the application keeps producing zones, frames and plots normally; it
 * only loses sampling, module metadata and kernel-side events.
 *
 * `samples_per_core` and `control_capacity` are slot counts and must be
 * powers of two. Passing 0 uses the defaults.
 *
 * Returns 0 on success, or a negative VITA_TRACY_ERROR_* / Sce error. */
int vita_tracy_kernel_attach(uint32_t samples_per_core, uint32_t control_capacity);

/* Stops draining, unregisters and frees the shared block. */
void vita_tracy_kernel_detach(void);

/* Requests kernel sampling at the given rate. 0 stops sampling. */
int vita_tracy_kernel_set_sampling(uint32_t frequency_hz);

#ifdef __cplusplus
}
#endif
