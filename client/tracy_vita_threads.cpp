#include <mutex>
#include <psp2/kernel/threadmgr.h>

#include "tracy_vita_platform.hpp"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/thread_registry.h"

namespace {
std::mutex g_registry_mutex;
VitaTraceThreadRegistry g_workers{};
VitaTraceSharedHeader *g_shared = nullptr;
}

extern "C" void tracy_vita_profiler_thread_enter(void) {
    const uint32_t tid = (uint32_t)sceKernelGetThreadId();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    vita_trace_thread_add(&g_workers, tid);
    if (g_shared) vita_trace_thread_add(&g_shared->profiler_threads, tid);
}

extern "C" void tracy_vita_profiler_thread_exit(void) {
    const uint32_t tid = (uint32_t)sceKernelGetThreadId();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (g_shared) vita_trace_thread_remove(&g_shared->profiler_threads, tid);
    vita_trace_thread_remove(&g_workers, tid);
}

/* The mutex covers publication and unpublication of the mapping as well as
 * worker lifecycle changes. No callback can touch a freed shared block. */
extern "C" void tracy_vita_profiler_threads_bind(void *shared) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_shared = static_cast<VitaTraceSharedHeader *>(shared);
    if (!g_shared) return;
    for (uint32_t i = 0; i < VITA_TRACE_MAX_PROFILER_THREADS; ++i) {
        const uint32_t tid = __atomic_load_n(&g_workers.tids[i], __ATOMIC_ACQUIRE);
        if (tid) vita_trace_thread_add(&g_shared->profiler_threads, tid);
    }
    __atomic_store_n(&g_shared->profiler_threads.overflow,
        __atomic_load_n(&g_workers.overflow, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}
