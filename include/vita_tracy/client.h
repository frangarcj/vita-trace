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
 * Returns 0 on success, or the failing sceSysmodule error. */
int vita_tracy_init(void);

/* Unloads only what vita_tracy_init loaded. */
void vita_tracy_shutdown(void);

/* Raw ScePerf timebase, the clock domain Tracy timestamps and kernel
 * backend events are both expressed in. */
uint64_t vita_tracy_timebase_value(void);
uint32_t vita_tracy_timebase_frequency(void);

#ifdef __cplusplus
}
#endif
