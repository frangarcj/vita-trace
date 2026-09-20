#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <thread>

#include <tracy/Tracy.hpp>

#include "debugScreen.h"
#include "vita_tracy/abi.h"
#include "vita_tracy/client.h"

namespace {

std::atomic<bool> g_run{true};
std::atomic<uint32_t> g_sink{0};

__attribute__((noinline)) void Core0Hot() {
    volatile uint32_t value = g_sink.load(std::memory_order_relaxed) | 1u;
    for (uint32_t i = 0; i < 250000u; ++i) value = value * 1664525u + i + 1013904223u;
    g_sink.fetch_xor(value, std::memory_order_relaxed);
}

__attribute__((noinline)) void Core1Hot() {
    volatile uint32_t value = g_sink.load(std::memory_order_relaxed) + 3u;
    for (uint32_t i = 0; i < 170000u; ++i) value ^= (value << 5) + i + (value >> 2);
    g_sink.fetch_add(value, std::memory_order_relaxed);
}

__attribute__((noinline)) void Core2Burst() {
    volatile uint32_t value = g_sink.load(std::memory_order_relaxed) + 7u;
    for (uint32_t i = 0; i < 45000u; ++i) value = (value << 3) ^ (value >> 1) ^ i;
    g_sink.fetch_add(value, std::memory_order_relaxed);
}

__attribute__((noinline)) void AppPthreadHot() {
    volatile uint32_t value = g_sink.load(std::memory_order_relaxed) + 11u;
    for (uint32_t i = 0; i < 90000u; ++i) value = value * 33u + (i ^ (value >> 7));
    g_sink.fetch_xor(value, std::memory_order_relaxed);
}

int Core0Thread(SceSize, void *) {
    tracy::SetThreadName("irq-core0");
    while (g_run.load(std::memory_order_relaxed)) Core0Hot();
    return 0;
}

int Core1Thread(SceSize, void *) {
    tracy::SetThreadName("irq-core1");
    while (g_run.load(std::memory_order_relaxed)) Core1Hot();
    return 0;
}

int Core2Thread(SceSize, void *) {
    tracy::SetThreadName("irq-core2-burst");
    while (g_run.load(std::memory_order_relaxed)) {
        Core2Burst();
        sceKernelDelayThread(12000);
    }
    return 0;
}

SceUID Start(const char *name, SceKernelThreadEntry entry, int affinity) {
    SceUID tid = sceKernelCreateThread(name, entry, 0x40, 0x4000, 0, affinity, nullptr);
    if (tid < 0) return tid;
    if (sceKernelStartThread(tid, 0, nullptr) < 0) {
        sceKernelDeleteThread(tid);
        return -1;
    }
    return tid;
}

void Stop(SceUID tid) {
    if (tid < 0) return;
    sceKernelWaitThreadEnd(tid, nullptr, nullptr);
    sceKernelDeleteThread(tid);
}

void PrintStats(unsigned second) {
    VitaTracyStats stats{};
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    const int ret = vita_tracy_kernel_get_stats(&stats);
    if (ret < 0) {
        psvDebugScreenPrintf("stats failed: %d\n", ret);
        return;
    }

    psvDebugScreenClear(0x000000);
    psvDebugScreenPrintf("vita-tracy IRQ PC sampler - ABI %u\n", VITA_TRACY_ABI_VERSION);
    psvDebugScreenPrintf("experimental hardware validation; second %u/120\n\n", second);
    psvDebugScreenPrintf("handler %u  ARM %u MHz  cores 0x%X  error %d\n",
        stats.sample_irq_handler_registered, stats.sample_irq_arm_mhz,
        stats.sample_irq_core_mask, stats.sample_irq_last_error);
    for (unsigned cpu = 0; cpu < 3; ++cpu) {
        psvDebugScreenPrintf("c%u: irq %u overflow %u emit %u drop %u\n",
            cpu, stats.sample_irq_calls[cpu], stats.sample_irq_overflows[cpu],
            stats.samples_emitted[cpu], stats.samples_dropped[cpu]);
        psvDebugScreenPrintf("    foreign %u kernel %u ctxerr %u\n",
            stats.sample_irq_not_target[cpu], stats.sample_irq_kernel[cpu],
            stats.sample_irq_context_errors[cpu]);
    }
    psvDebugScreenPrintf("\nExpected source PCs: Core0Hot/Core1Hot/Core2Burst/AppPthreadHot\n");
    psvDebugScreenPrintf("Core2 intentionally sleeps. Application std::thread must remain visible.\n");
    psvDebugScreenPrintf("After first successful handler registration the plugin stays resident until reboot.\n");
}

} // namespace

int main() {
    psvDebugScreenInit();
    vita_tracy_init();
    tracy::SetThreadName("irq-main");

    const int attach = vita_tracy_kernel_attach(0, 0);
    psvDebugScreenPrintf("kernel attach: %d\n", attach);
    if (attach < 0) {
        psvDebugScreenPrintf("matching tracy_kernel.skprx must be loaded before this HB\n");
        sceKernelDelayThread(5000000);
        vita_tracy_shutdown();
        return attach;
    }

    const int sampling = vita_tracy_kernel_set_sampling_ex(100, VITA_TRACY_SAMPLING_PMU_IRQ);
    psvDebugScreenPrintf("PMU-overflow IRQ sampling 100 Hz: %d\n", sampling);
    if (sampling < 0) {
        VitaTracyStats stats{};
        stats.size = sizeof(stats);
        stats.abi_version = VITA_TRACY_ABI_VERSION;
        if (vita_tracy_kernel_get_stats(&stats) == 0)
            psvDebugScreenPrintf("sampler error %d, handler %u, ARM %u MHz\n",
                stats.sample_irq_last_error, stats.sample_irq_handler_registered,
                stats.sample_irq_arm_mhz);
        sceKernelDelayThread(5000000);
        vita_tracy_kernel_detach_checked();
        vita_tracy_shutdown();
        return sampling;
    }

    SceUID core0 = Start("irq-app-c0", Core0Thread, SCE_KERNEL_CPU_MASK_USER_0);
    SceUID core1 = Start("irq-app-c1", Core1Thread, SCE_KERNEL_CPU_MASK_USER_1);
    SceUID core2 = Start("irq-app-c2", Core2Thread, SCE_KERNEL_CPU_MASK_USER_2);

    std::thread pthread_worker([] {
        tracy::SetThreadName("irq-app-pthread");
        while (g_run.load(std::memory_order_relaxed)) AppPthreadHot();
    });

    for (unsigned second = 1; second <= 120; ++second) {
        sceKernelDelayThread(1000000);
        PrintStats(second);
        FrameMark;
    }

    g_run.store(false, std::memory_order_relaxed);
    pthread_worker.join();
    Stop(core0);
    Stop(core1);
    Stop(core2);

    const int stop = vita_tracy_kernel_set_sampling(0);
    psvDebugScreenPrintf("\nsampling stop: %d\n", stop);
    const int detach = vita_tracy_kernel_detach_checked();
    psvDebugScreenPrintf("detach: %d\n", detach);
    const int shutdown = vita_tracy_shutdown_checked();
    psvDebugScreenPrintf("shutdown: %d\n", shutdown);
    sceKernelDelayThread(3000000);
    sceKernelExitProcess(0);
    return 0;
}
