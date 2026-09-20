#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_TRACE_MAX_PROFILER_THREADS 16u

/* Process-local thread IDs (PUIDs), never pthread_t or kernel GUIDs. Zero
 * means vacant. A full registry fails closed rather than sampling Tracy. */
typedef struct VitaTraceThreadRegistry {
    uint32_t tids[VITA_TRACE_MAX_PROFILER_THREADS];
    uint32_t overflow;
} VitaTraceThreadRegistry;

int vita_trace_thread_add(VitaTraceThreadRegistry *registry, uint32_t tid);
void vita_trace_thread_remove(VitaTraceThreadRegistry *registry, uint32_t tid);
int vita_trace_thread_contains(const VitaTraceThreadRegistry *registry, uint32_t tid);
int vita_trace_thread_registry_complete(const VitaTraceThreadRegistry *registry);

#ifdef __cplusplus
}
#endif
