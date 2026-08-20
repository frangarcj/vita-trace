#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <tracy/Tracy.hpp>

#include "vita_tracy/client.h"

/* Workloads with a known shape, so a capture can be checked against what
 * the profile is supposed to look like instead of against "it looks busy".
 * These are the cases the design's validation plan calls for: a known time
 * split between two functions, CPU-bound threads pinned to different cores,
 * and a thread that sleeps and must not accumulate CPU time. */

namespace {

volatile uint32_t g_sink = 0;

void FunctionA() {
    ZoneScoped;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 800000; ++i) {
        acc += i * 3u;
    }
    g_sink += acc;
}

void FunctionB() {
    ZoneScoped;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 200000; ++i) {
        acc ^= i;
    }
    g_sink += acc;
}

/* Roughly four parts A to one part B, so A should dominate the samples. */
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

    /* The kernel backend is optional: without the plugin the zones below
     * still reach the viewer, only the samples are missing. */
    bool sampling = vita_tracy_kernel_attach(0, 0) == 0;
    if (sampling) {
        vita_tracy_kernel_set_sampling(500);
    }

    SceUID busy0 = sceKernelCreateThread("busy0", BusyThread, 0x40, 0x4000, 0,
                                         SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    SceUID busy1 = sceKernelCreateThread("busy1", BusyThread, 0x40, 0x4000, 0,
                                         SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    SceUID sleepy = sceKernelCreateThread("sleepy", SleepyThread, 0x40, 0x4000, 0,
                                          SCE_KERNEL_CPU_MASK_USER_2, nullptr);

    sceKernelStartThread(busy0, 0, nullptr);
    sceKernelStartThread(busy1, 0, nullptr);
    sceKernelStartThread(sleepy, 0, nullptr);

    sceKernelWaitThreadEnd(busy0, nullptr, nullptr);
    sceKernelWaitThreadEnd(busy1, nullptr, nullptr);
    sceKernelWaitThreadEnd(sleepy, nullptr, nullptr);

    if (sampling) {
        vita_tracy_kernel_detach();
    }
    vita_tracy_shutdown();

    sceKernelExitProcess(0);
    return 0;
}
