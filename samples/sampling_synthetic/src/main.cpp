#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <thread>
#include <stdio.h>

#include <tracy/Tracy.hpp>

#include "vita_tracy/client.h"
#include "vita_tracy/abi.h"

/* Workloads with a known shape, so a capture can be checked against what
 * the profile is supposed to look like instead of against "it looks busy".
 * These are the cases the design's validation plan calls for: a known time
 * split between two functions, CPU-bound threads pinned to different cores,
 * and a thread that sleeps and must not accumulate CPU time. */

namespace {

std::atomic<uint32_t> g_sink{0};

__attribute__((noinline)) void FunctionA() {
    ZoneScoped;
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 800000; ++i) {
        acc += i * 3u;
    }
    g_sink.fetch_add(acc, std::memory_order_relaxed);
}

__attribute__((noinline)) void FunctionB() {
    ZoneScoped;
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 200000; ++i) {
        acc ^= i;
    }
    g_sink.fetch_add(acc, std::memory_order_relaxed);
}

/* A has more iterations; no exact timing ratio is assumed. */
int BusyThread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    tracy::SetThreadName("busy");

    for (int i = 0; i < 400; ++i) {
        FunctionA();
        FunctionB();
        FrameMarkNamed("busy");
    }
    return 0;
}

/* Must show as idle: a sleeping thread accumulating CPU time is exactly the
 * failure mode a mcount-style profiler has. */
int SleepyThread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    tracy::SetThreadName("sleepy");

    for (int i = 0; i < 200; ++i) {
        ZoneScopedN("sleep");
        sceKernelDelayThread(20000);
    }
    return 0;
}

} // namespace

int main() {
    vita_tracy_init();
    tracy::SetThreadName("main");

    /* This diagnostic executable REQUIRES the matching kernel plugin loaded.
     * Weak unresolved imports are not a safe residency probe. */
    bool sampling = vita_tracy_kernel_attach(0, 0) == 0;
    if (sampling) {
        const int ret = vita_tracy_kernel_set_sampling_ex(250, VITA_TRACY_SAMPLING_ALLOW_SUSPEND);
        printf("timer-driven suspend diagnostics (not CPU time): %d\n", ret);
    }

    SceUID busy0 = sceKernelCreateThread("busy0", BusyThread, 0x40, 0x4000, 0,
                                         SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    SceUID busy1 = sceKernelCreateThread("busy1", BusyThread, 0x40, 0x4000, 0,
                                         SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    SceUID sleepy = sceKernelCreateThread("sleepy", SleepyThread, 0x40, 0x4000, 0,
                                          SCE_KERNEL_CPU_MASK_USER_2, nullptr);

    SceUID native_threads[] = {busy0, busy1, sleepy};
    for (SceUID &tid : native_threads) {
        if (tid >= 0 && sceKernelStartThread(tid, 0, nullptr) < 0) {
            printf("could not start thread %d\n", tid);
            sceKernelDeleteThread(tid);
            tid = -1;
        }
    }

    // Regression: this worker has the same kernel name as Tracy's pthreads,
    // and main is the thread that requested sampling. Both must be visible.
    std::thread pthread_worker([] { BusyThread(0, nullptr); });
    for (int i = 0; i < 100; ++i) {
        FunctionA();
        FunctionB();
        FrameMark;
    }
    pthread_worker.join();

    for (SceUID tid : native_threads) {
        if (tid >= 0) {
            sceKernelWaitThreadEnd(tid, nullptr, nullptr);
            sceKernelDeleteThread(tid);
        }
    }

    if (sampling) {
        vita_tracy_kernel_detach();
    }
    vita_tracy_shutdown();

    sceKernelExitProcess(0);
    return 0;
}
