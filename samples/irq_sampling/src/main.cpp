#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <tracy/Tracy.hpp>

#include "debugScreen.h"
#include "vita_tracy/abi.h"
#include "vita_tracy/client.h"

namespace {

std::atomic<bool> g_run{true};
std::atomic<uint32_t> g_sink{0};

/* Everything printed on screen also goes to a file so the run can be read
 * back over FTP; the file is rewritten on every refresh. */
constexpr const char *kReportPath = "ux0:data/vita_tracy_irq.txt";
constexpr const char *kRatePath = "ux0:data/vita_tracy_irq_hz.txt";
/* Present: register + service overflows, count them, emit nothing. */
constexpr const char *kCountOnlyPath = "ux0:data/vita_tracy_irq_count_only";
/* Plain PC sampling, without PMU events and scheduler hooks. */
constexpr const char *kPlainPath = "ux0:data/vita_tracy_irq_plain";
/* L1D refills and accesses per sample. */
constexpr uint32_t kEvents[] = {0x03, 0x04};
/* Present: register the raw IRQ node only, never arm the PMU. */
constexpr const char *kRegisterOnlyPath = "ux0:data/vita_tracy_irq_register_only";
constexpr unsigned kDefaultHz = 100;
constexpr unsigned kDurationSeconds = 120;

char g_report[8192];
size_t g_report_len = 0;

void ReportReset() {
    g_report_len = 0;
    g_report[0] = '\0';
}

void Report(const char *fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    psvDebugScreenPrintf("%s\n", line);
    size_t len = strlen(line);
    if (g_report_len + len + 2 < sizeof(g_report)) {
        memcpy(g_report + g_report_len, line, len);
        g_report_len += len;
        g_report[g_report_len++] = '\n';
        g_report[g_report_len] = '\0';
    }
}

void WriteReport() {
    sceIoMkdir("ux0:data", 0777);
    SceUID fd = sceIoOpen(kReportPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, g_report, g_report_len);
    sceIoClose(fd);
}

/* Optional override so the first console run can use the 10 Hz the
 * validation matrix asks for without rebuilding. */
bool MarkerExists(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    sceIoClose(fd);
    return true;
}

unsigned RequestedHz() {
    SceUID fd = sceIoOpen(kRatePath, SCE_O_RDONLY, 0);
    if (fd < 0) return kDefaultHz;
    char buf[16];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return kDefaultHz;
    buf[n] = '\0';
    long hz = strtol(buf, nullptr, 10);
    return (hz > 0 && hz < 100000) ? (unsigned)hz : kDefaultHz;
}

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
    while (g_run.load(std::memory_order_relaxed)) { Core0Hot(); sceKernelDelayThread(500); }
    return 0;
}

int Core1Thread(SceSize, void *) {
    tracy::SetThreadName("irq-core1");
    while (g_run.load(std::memory_order_relaxed)) { Core1Hot(); sceKernelDelayThread(500); }
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

void PrintStats(unsigned second, unsigned hz) {
    VitaTracyStats stats{};
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    const int ret = vita_tracy_kernel_get_stats(&stats);
    psvDebugScreenClear(0x000000);
    ReportReset();
    if (ret < 0) {
        Report("stats failed: %d", ret);
        WriteReport();
        return;
    }

    Report("vita-tracy IRQ PC sampler - ABI %u", VITA_TRACY_ABI_VERSION);
    Report("experimental hardware validation; %u Hz; second %u/%u", hz, second, kDurationSeconds);
    Report("");
    Report("handler %u  ARM %u MHz  cores 0x%X  error %d",
        stats.sample_irq_handler_registered, stats.sample_irq_arm_mhz,
        stats.sample_irq_core_mask, stats.sample_irq_last_error);
    for (unsigned cpu = 0; cpu < 3; ++cpu) {
        Report("c%u: irq %u overflow %u emit %u drop %u",
            cpu, stats.sample_irq_calls[cpu], stats.sample_irq_overflows[cpu],
            stats.samples_emitted[cpu], stats.samples_dropped[cpu]);
        Report("    foreign %u kernel %u ctxerr %u",
            stats.sample_irq_not_target[cpu], stats.sample_irq_kernel[cpu],
            stats.sample_irq_context_errors[cpu]);
    }
    Report("control dropped: %u", (unsigned)stats.control_dropped);
    Report("adopted %u/%u/%u  missed wraps %u/%u/%u",
        stats.sample_irq_adopted[0], stats.sample_irq_adopted[1], stats.sample_irq_adopted[2],
        stats.sample_irq_missed[0], stats.sample_irq_missed[1], stats.sample_irq_missed[2]);
    /* Hook calls without records: the hook's pid is not ours; compare. */
    Report("switch hooks %u  pid 0x%08X  last other pid 0x%08X", stats.switch_hooks_installed,
        (unsigned)sceKernelGetProcessId(), (unsigned)stats.switch_last_other_pid);
    for (unsigned cpu = 0; cpu < 3; ++cpu)
        Report("  c%u switches: calls %u recorded %u dropped %u", cpu, stats.switch_calls[cpu],
            stats.switch_recorded[cpu], stats.switch_dropped[cpu]);
    Report("");
    Report("Expected source PCs: Core0Hot/Core1Hot/Core2Burst/AppPthreadHot");
    Report("Core2 intentionally sleeps. Application pthread must remain visible.");
    Report("After first successful handler registration the plugin stays resident until reboot.");
    WriteReport();
}

} // namespace

int main() {
    psvDebugScreenInit();
    vita_tracy_init();
    tracy::SetThreadName("irq-main");

    const unsigned hz = RequestedHz();
    const int attach = vita_tracy_kernel_attach(0, 0);
    Report("kernel attach: %d", attach);
    WriteReport(); /* Before the IRQ start: a hang there must be placeable. */
    if (attach < 0) {
        Report("matching tracy_kernel.skprx must be loaded before this HB");
        WriteReport();
        sceKernelDelayThread(5000000);
        vita_tracy_shutdown();
        return attach;
    }

    const bool register_only = MarkerExists(kRegisterOnlyPath);
    const bool count_only = register_only || MarkerExists(kCountOnlyPath);
    const bool plain = count_only || MarkerExists(kPlainPath);
    const uint32_t flags = VITA_TRACY_SAMPLING_PMU_IRQ |
                           (count_only ? VITA_TRACY_SAMPLING_IRQ_COUNT_ONLY : 0u) |
                           (register_only ? VITA_TRACY_SAMPLING_IRQ_REGISTER_ONLY : 0u) |
                           (plain ? 0u : VITA_TRACY_SAMPLING_CONTEXT_SWITCHES);
    Report("mode: %s", register_only ? "register only (node installed, PMU never armed)"
                     : count_only ? "count only (no samples emitted)" : "full (samples emitted)");
    WriteReport();
    const int sampling = vita_tracy_kernel_set_sampling_events((int)hz, flags, kEvents,
        plain ? 0u : (uint32_t)(sizeof(kEvents) / sizeof(kEvents[0])));
    Report("PMU-overflow IRQ sampling %u Hz%s: %d", hz,
        plain ? "" : " + L1D events + context switches", sampling);
    if (sampling < 0) {
        VitaTracyStats stats{};
        stats.size = sizeof(stats);
        stats.abi_version = VITA_TRACY_ABI_VERSION;
        if (vita_tracy_kernel_get_stats(&stats) == 0)
            Report("sampler error %d, handler %u, ARM %u MHz",
                stats.sample_irq_last_error, stats.sample_irq_handler_registered,
                stats.sample_irq_arm_mhz);
        const int detach = vita_tracy_kernel_detach_checked();
        Report("detach: %d", detach);
        WriteReport();
        sceKernelDelayThread(5000000);
        vita_tracy_shutdown();
        return sampling;
    }
    WriteReport();

    SceUID core0 = Start("irq-app-c0", Core0Thread, SCE_KERNEL_CPU_MASK_USER_0);
    SceUID core1 = Start("irq-app-c1", Core1Thread, SCE_KERNEL_CPU_MASK_USER_1);
    SceUID core2 = Start("irq-app-c2", Core2Thread, SCE_KERNEL_CPU_MASK_USER_2);

    /* A POSIX worker, the way ports create threads. Not std::thread: the
     * static libstdc++ treats threads as inactive unless pthread_cancel is
     * linked, and forcing it in crashed in pthread_mutex_unlock at startup
     * (2026-09-28). */
    pthread_t pthread_worker;
    const bool have_pthread = pthread_create(&pthread_worker, nullptr, [](void *) -> void * {
        tracy::SetThreadName("irq-app-pthread");
        while (g_run.load(std::memory_order_relaxed)) { AppPthreadHot(); sceKernelDelayThread(500); }
        return nullptr;
    }, nullptr) == 0;

    for (unsigned second = 1; second <= kDurationSeconds; ++second) {
        sceKernelDelayThread(1000000);
        PrintStats(second, hz);
        FrameMark;
    }

    g_run.store(false, std::memory_order_relaxed);
    if (have_pthread) pthread_join(pthread_worker, nullptr);
    Stop(core0);
    Stop(core1);
    Stop(core2);

    const int stop = vita_tracy_kernel_set_sampling(0);
    Report("");
    Report("sampling stop: %d", stop);
    const int detach = vita_tracy_kernel_detach_checked();
    Report("detach: %d", detach);
    const int shutdown = vita_tracy_shutdown_checked();
    Report("shutdown: %d", shutdown);
    WriteReport();
    sceKernelDelayThread(3000000);
    sceKernelExitProcess(0);
    return 0;
}
