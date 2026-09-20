#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*VitaTracyTickCallback)(void *context);
/* A single-CPU system timer. A custom callback must be IRQ-safe: no blocking,
 * allocation, logging or register-state assumptions beyond its own backend. */
typedef struct VitaTracyTickSource {
    int32_t timer;
    int32_t event;
    uint32_t enabled;
    uint32_t ticks;
    uint32_t prepared;
    VitaTracyTickCallback callback;
    void *context;
} VitaTracyTickSource;

void vita_tracy_tick_init(VitaTracyTickSource *source);
int vita_tracy_tick_start(VitaTracyTickSource *source, uint32_t frequency_hz);
/* Prepare/arm split lets multi-core backends finish setup before recording. */
int vita_tracy_tick_prepare(VitaTracyTickSource *source, uint32_t frequency_hz,
                           uint32_t cpu_mask, VitaTracyTickCallback callback, void *context);
int vita_tracy_tick_arm(VitaTracyTickSource *source);
int vita_tracy_tick_wait(VitaTracyTickSource *source);
void vita_tracy_tick_wake(VitaTracyTickSource *source);
/* On failure, retain handles so the caller can retry and refuse unload. */
int vita_tracy_tick_stop(VitaTracyTickSource *source);

#ifdef __cplusplus
}
#endif
