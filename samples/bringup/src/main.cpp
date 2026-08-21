#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <tracy/Tracy.hpp>

#include "vita_tracy/client.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/pmu.h"
#include "vita_tracy/timebase.h"

#include "debugScreen.h"

/* Answers the questions that only a console can answer, on screen and in a
 * file, before doing anything else. Everything here was predicted by reading
 * firmware; this is where the predictions get checked. */

extern "C" int sceKernelGetPMUSERENR(void);

namespace {

constexpr const char *kReportPath = "ux0:data/vita_tracy_bringup.txt";

char g_report[8192];
size_t g_report_len = 0;

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
    if (fd < 0) {
        psvDebugScreenPrintf("could not write %s (0x%08X)\n", kReportPath, (unsigned)fd);
        return;
    }
    sceIoWrite(fd, g_report, g_report_len);
    sceIoClose(fd);
    psvDebugScreenPrintf("\nreport written to %s\n", kReportPath);
}

void CheckTimebase() {
    Report("-- timebase --");
    Report("SCE_SYSMODULE_PERF load: 0x%08X %s", (unsigned)vita_tracy_perf_module_status(),
           vita_tracy_perf_module_status() < 0 ? "(FAILED)" : "(ok)");
    Report("clock source: %s",
           vita_tracy_timebase_use_perf() ? "ScePerf timebase" : "process timer (us)");


    if (!vita_tracy_timebase_use_perf()) {
        Report("ScePerf is not callable here, so nothing below it can run");
        Report("");
        return;
    }

    /* Predicted to be 0xFFFFFFFF: the call tail-jumps into a ScePamgr import
     * that retail firmware has provided nothing for since 3.50. */
    uint32_t reported = scePerfGetTimebaseFrequency();
    Report("scePerfGetTimebaseFrequency: 0x%08X (%u)", (unsigned)reported, (unsigned)reported);
    Report("  plausible: %s", vita_trace_timebase_hz_is_plausible(reported) ? "yes" : "NO");

    uint64_t t0 = scePerfGetTimebaseValue();
    SceUInt64 u0 = sceKernelGetProcessTimeWide();
    sceKernelDelayThread(200000);
    uint64_t t1 = scePerfGetTimebaseValue();
    SceUInt64 u1 = sceKernelGetProcessTimeWide();

    uint64_t ticks = t1 - t0;
    uint64_t micros = (uint64_t)(u1 - u0);
    Report("measured over %llu us: %llu ticks", (unsigned long long)micros,
           (unsigned long long)ticks);
    Report("  -> %u Hz", (unsigned)vita_trace_timebase_calibrate(ticks, micros));
    Report("client uses: %u Hz", (unsigned)vita_tracy_timebase_frequency());

    /* 333 is what ScePamgr returned on firmwares that still had it; if the
     * measurement lands near a round multiple, that settles the unit. */
    Report("ratio to 333: %llu", (unsigned long long)(
        vita_tracy_timebase_frequency() ? vita_tracy_timebase_frequency() / 333u : 0));
    Report("");
}

void CheckPmu() {
    Report("-- pmu --");

    int userenr = sceKernelGetPMUSERENR();
    Report("sceKernelGetPMUSERENR: 0x%08X", (unsigned)userenr);
    if (userenr == 0) {
        Report("  userland PMU access is DISABLED; counters cannot work");
        Report("");
        return;
    }

    static const uint8_t events[] = {
        SCE_PERF_ARM_PMON_CYCLE_COUNT,
        SCE_PERF_ARM_PMON_DCACHE_MISS,
        SCE_PERF_ARM_PMON_ICACHE_MISS,
        SCE_PERF_ARM_PMON_BRANCH_MISPREDICT,
        SCE_PERF_ARM_PMON_DTLB_MISS,
        SCE_PERF_ARM_PMON_DCACHE_STALL,
        SCE_PERF_ARM_PMON_DCACHE_ACCESS,
        SCE_PERF_ARM_PMON_DATA_READ,
    };
    uint32_t programmed = vita_tracy_pmu_begin(events, sizeof(events) / sizeof(events[0]));
    Report("counters programmed: %u of %u requested", (unsigned)programmed,
           (unsigned)(sizeof(events) / sizeof(events[0])));
    if (programmed == 0) {
        Report("");
        return;
    }

    SceUInt32 before = 0, after = 0;
    scePerfArmPmonGetCounterValue(SCE_PERF_ARM_PMON_THREAD_ID_SELF, 0, &before);
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 2000000; ++i) {
        acc += i * 3u;
    }
    scePerfArmPmonGetCounterValue(SCE_PERF_ARM_PMON_THREAD_ID_SELF, 0, &after);
    Report("counter 0 (cycles) delta over 2M iterations: %u",
           (unsigned)vita_tracy_pmu_delta(before, after));
    Report("  a zero or absurd delta means the counter is not really running");
    Report("");
}

void CheckKernelPlugin() {
    Report("-- kernel plugin --");

    int ret = vita_tracy_kernel_attach(0, 0);
    Report("vita_tracy_kernel_attach: %d", ret);
    if (ret != 0) {
        Report("  plugin absent or refused; zones still work without it");
        Report("");
        return;
    }

    Report("  attached, ring mapped and acknowledged");

    int sampling = vita_tracy_kernel_set_sampling(250);
    Report("set_sampling(250 Hz): %d", sampling);

    sceKernelDelayThread(1000000);

    VitaTracyStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    int stats_ret = vitaTracyGetStats(&stats);
    Report("vitaTracyGetStats: %d", stats_ret);
    if (stats_ret == 0) {
        for (int cpu = 0; cpu < 4; ++cpu) {
            Report("  core %d: %u emitted, %u dropped", cpu, (unsigned)stats.samples_emitted[cpu],
                   (unsigned)stats.samples_dropped[cpu]);
        }
        Report("  control dropped: %u", (unsigned)stats.control_dropped);
    }
    Report("");
}

void Work() {
    ZoneScoped;
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 100000; ++i) {
        acc += i * 3u;
    }
}

} // namespace

int main() {
    psvDebugScreenInit();
    psvDebugScreenPrintf("vita-tracy bring-up\n\n");

    vita_tracy_init();
    tracy::SetThreadName("bringup");

    Report("vita-tracy bring-up report");
    Report("abi version %u", (unsigned)VITA_TRACY_ABI_VERSION);
    Report("");

    CheckTimebase();
    CheckPmu();
    CheckKernelPlugin();

    WriteReport();

    /* Left running so a viewer can attach over the network. With on-demand
     * collection nothing is recorded until one does. */
    psvDebugScreenPrintf("\nlistening for a Tracy viewer on port 8086\n");
    psvDebugScreenPrintf("press PS button to quit\n");

    bool announced = false;
    for (;;) {
        Work();
        FrameMark;
        if (!announced && TracyIsConnected) {
            psvDebugScreenPrintf("viewer connected\n");
            announced = true;
        }
        sceKernelDelayThread(16000);
    }

    return 0;
}
