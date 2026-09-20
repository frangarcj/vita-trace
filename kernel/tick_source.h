#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A system-allocated periodic timer. The callback only wakes the worker:
 * it does NOT capture the interrupted PC and must not be described as such. */
typedef struct VitaTracyTickSource {
    int32_t timer;
    int32_t event;
    uint32_t enabled;
    uint32_t ticks;
} VitaTracyTickSource;

void vita_tracy_tick_init(VitaTracyTickSource *source);
int vita_tracy_tick_start(VitaTracyTickSource *source, uint32_t frequency_hz);
int vita_tracy_tick_wait(VitaTracyTickSource *source);
void vita_tracy_tick_wake(VitaTracyTickSource *source);
/* On failure, retain handles so the caller can retry and refuse unload. */
int vita_tracy_tick_stop(VitaTracyTickSource *source);

#ifdef __cplusplus
}
#endif
