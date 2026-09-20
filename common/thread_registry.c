#include "vita_tracy/thread_registry.h"

int vita_trace_thread_contains(const VitaTraceThreadRegistry *registry, uint32_t tid) {
    if (tid == 0) return 0;
    for (uint32_t i = 0; i < VITA_TRACE_MAX_PROFILER_THREADS; ++i) {
        if (__atomic_load_n(&registry->tids[i], __ATOMIC_ACQUIRE) == tid) return 1;
    }
    return 0;
}

int vita_trace_thread_add(VitaTraceThreadRegistry *registry, uint32_t tid) {
    if (tid == 0) return 0;
    if (vita_trace_thread_contains(registry, tid)) return 1;
    for (uint32_t i = 0; i < VITA_TRACE_MAX_PROFILER_THREADS; ++i) {
        uint32_t empty = 0;
        if (__atomic_compare_exchange_n(&registry->tids[i], &empty, tid, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return 1;
    }
    __atomic_store_n(&registry->overflow, 1u, __ATOMIC_RELEASE);
    return 0;
}

void vita_trace_thread_remove(VitaTraceThreadRegistry *registry, uint32_t tid) {
    if (tid == 0) return;
    for (uint32_t i = 0; i < VITA_TRACE_MAX_PROFILER_THREADS; ++i) {
        uint32_t expected = tid;
        __atomic_compare_exchange_n(&registry->tids[i], &expected, 0u, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}

int vita_trace_thread_registry_complete(const VitaTraceThreadRegistry *registry) {
    return __atomic_load_n(&registry->overflow, __ATOMIC_ACQUIRE) == 0;
}
