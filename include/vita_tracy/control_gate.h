#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Single attached session. Positive SceUIDs fit in 31 bits; the high bit
 * belongs to the cleanup request, atomically tied to that exact target. */
typedef struct VitaTraceControlGate {
    uint32_t busy;
    uint32_t target_state;
} VitaTraceControlGate;
typedef int (*VitaTraceCleanup)(void *context);

int vita_trace_control_enter(VitaTraceControlGate *gate);
int vita_trace_control_pending(const VitaTraceControlGate *gate);
uint32_t vita_trace_control_target(const VitaTraceControlGate *gate);
/* Called with control ownership, on successful attach/detach only. */
void vita_trace_control_set_target(VitaTraceControlGate *gate, uint32_t pid);
int vita_trace_control_request(VitaTraceControlGate *gate, uint32_t pid);
/* Never waits for another owner. A failed cleanup remains pending for retry. */
int vita_trace_control_reap(VitaTraceControlGate *gate, VitaTraceCleanup cleanup, void *context);
int vita_trace_control_leave(VitaTraceControlGate *gate, VitaTraceCleanup cleanup, void *context);

#ifdef __cplusplus
}
#endif
