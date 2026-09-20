#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Single attached session. Cleanup requests are idempotent for that session. */
typedef struct VitaTraceControlGate {
    uint32_t busy;
    uint32_t cleanup_pending;
} VitaTraceControlGate;
typedef int (*VitaTraceCleanup)(void *context);

int vita_trace_control_enter(VitaTraceControlGate *gate);
int vita_trace_control_pending(const VitaTraceControlGate *gate);
void vita_trace_control_request(VitaTraceControlGate *gate);
/* Never waits for another owner. A failed cleanup remains pending for retry. */
int vita_trace_control_reap(VitaTraceControlGate *gate, VitaTraceCleanup cleanup, void *context);
int vita_trace_control_leave(VitaTraceControlGate *gate, VitaTraceCleanup cleanup, void *context);

#ifdef __cplusplus
}
#endif
