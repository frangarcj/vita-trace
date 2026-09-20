#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include <taihen.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
constexpr const char *kKernelPluginLoadMarkerPath = "ux0:data/vita_tracy_kernel.on";
constexpr const char *kKernelPluginAttachMarkerPath = "ux0:data/vita_tracy_kernel_attach.on";
constexpr const char *kKernelPluginPath = "ux0:data/tracy_kernel.skprx";
constexpr const char *kKernelPluginModidPath = "ux0:data/vita_tracy_kernel_modid.txt";

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

/* Calling the control ABI while tracy_kernel.skprx is not resident takes the
 * process down: the weak import is patched to branch to address 0, and
 * there is no way to tell that apart from a bound stub by reading it. So
 * attaching is opt-in, gated on its own marker file.
 *
 * Loading and attaching CANNOT share one run, even though both are opt-in:
 * a process's imports are resolved once, when the loader starts it, which
 * for this app is before main() -- and therefore before LoadKernelPlugin()
 * gets a chance to taiHEN-load the module that would have made
 * vitaTracyRegister resolve to something real. Loading the provider mid-run
 * does not retroactively fix an import this process already resolved to
 * nothing. Confirmed the hard way on 2026-08-21: the first attempt loaded
 * and attached in the same run and crashed at pc=0 inside
 * vita_tracy_kernel_attach, with r12 pointing at the unresolved
 * vitaTracyRegister stub. So loading and attaching use separate markers,
 * and attaching only happens on a later, separate launch. */
bool KernelPluginMarkerExists(const char *path) {
    SceUID marker = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (marker < 0) {
        return false;
    }
    sceIoClose(marker);
    return true;
}

/* Reads back the modid LoadKernelPlugin stored on a PRIOR run and unloads
 * it, if present. Found necessary 2026-08-21: a kernel module's own
 * export/syscall table only gets bound to whichever code was running the
 * FIRST time this session's "VitaTracyKernel" library successfully
 * registered -- reloading a newer version on top, without unloading the
 * old instance first, leaves every later run's control-ABI calls
 * (vitaTracyRegister, vitaTracySetPmu, ...) still dispatching into that
 * first instance's compiled code, silently, no error anywhere. Several
 * iterations of kernel/pmu.c in a row all measured as "unchanged behavior"
 * on hardware before this surfaced -- the version bump alone (which does
 * defeat the SEPARATE SCE_KERNEL_ERROR_MODULEMGR_OLD_LIB anti-downgrade
 * check) was never enough by itself. */
void UnloadStalePlugin() {
    SceUID fd = sceIoOpen(kKernelPluginModidPath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    char buf[16];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    SceUID stale_modid = (SceUID)strtol(buf, NULL, 16);

    int res = 0;
    int ret = taiStopUnloadKernelModule(stale_modid, 0, NULL, 0, NULL, &res);
    Report("taiStopUnloadKernelModule(stale 0x%08X): ret=%d res=0x%08X", (unsigned)stale_modid, ret,
           (unsigned)res);
}

void StoreKernelPluginModid(SceUID modid) {
    SceUID fd = sceIoOpen(kKernelPluginModidPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%08X", (unsigned)modid);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

void LoadKernelPlugin() {
    Report("-- kernel plugin load --");

    if (!KernelPluginMarkerExists(kKernelPluginLoadMarkerPath)) {
        Report("skipped: create %s to try it", kKernelPluginLoadMarkerPath);
        Report("");
        return;
    }

    /* Never reload (unload-then-load) on a run that also intends to
     * attach: this process's own control-ABI imports already resolved,
     * at launch, against whatever was resident at that moment. Unloading
     * that exact instance mid-run -- even to load a "fresh" replacement
     * right back -- leaves those already-bound imports dangling into
     * memory this same call just freed. Only a load-only run (no attach
     * marker yet) is safe to reload in; a run that's about to attach must
     * leave whatever it was launched against alone. */
    if (KernelPluginMarkerExists(kKernelPluginAttachMarkerPath)) {
        Report("skipped: attach marker present, reusing whatever this");
        Report("  process's imports already resolved against at launch");
        Report("");
        return;
    }

    UnloadStalePlugin();

    /* Loaded through taiHEN from here, on demand, rather than through
     * ux0:tai/config.txt: a failed load or a contained crash costs a
     * relaunch of this app, not a system that no longer boots. That is
     * NOT the same as safe -- once tracy_kernel.skprx is actually running
     * in kernel context, a real bug in it (a deadlock on a kernel lock, a
     * jump to a bad PC at PL1) can still hang the whole console the same
     * way a permanently-installed plugin would, and needs a hold-the-power-
     * button reboot. */
    SceUID modid = taiLoadStartKernelModule(kKernelPluginPath, 0, NULL, 0);
    Report("taiLoadStartKernelModule(%s): 0x%08X", kKernelPluginPath, (unsigned)modid);
    if (modid < 0) {
        Report("  load/start failed: taiHEN missing, file missing at that");
        Report("  path, module_start returned an error, or it's already");
        Report("  loaded from an earlier run (harmless)");
    } else {
        StoreKernelPluginModid(modid);
        Report("  loaded. Relaunch this app with %s", kKernelPluginAttachMarkerPath);
        Report("  present to attach -- NOT in this run, see comment above");
        Report("  CheckKernelPlugin for why.");
    }
    Report("");
}

/* PMCCNTR (the dedicated cycle counter) is readable directly once
 * PMUSERENR is set, with no PMSELR indirection -- unlike the six
 * programmable counters. Reading it straight from userland, rather than
 * through any Sce* wrapper, is the whole point here: it proves the
 * kernel's CP15 writes (kernel/pmu.c) actually took effect on the core
 * this thread is running on, independent of ScePerf, which cannot load on
 * this console at all. */
uint32_t ReadPmccntr() {
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(value));
    return value;
}

/* Reading PMUSERENR itself is unconditionally permitted at PL0 by the ARM
 * architecture regardless of what it currently holds -- it's the register
 * software checks *to decide* whether it has PMU access, so gating the
 * read on itself would make it useless. If sceKernelGetPMUSERENR() is
 * really just this same instruction with no kernel round-trip, both reads
 * should always agree; sceKernelGetPMUSERENR() couldn't legitimately show
 * something this raw read doesn't. */
uint32_t ReadPmuserenrRaw(void) {
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c14, 0" : "=r"(value));
    return value;
}

/* Requires the plugin already attached (vitaTracySetPmu rejects any state
 * before that -- see kernel/service.c). Programs only the cycle counter
 * (counter_count = 0): vita_tracy_pmu_configure always enables it
 * unconditionally, so this is the minimal call that still exercises every
 * write in kernel/pmu.c -- PMUSERENR, PMCR, PMCNTENCLR/SET -- across every
 * core in this thread's own affinity mask. */
void CheckKernelPmu() {
    Report("-- pmu via kernel (CP15) --");

    VitaTracyPmuConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = sizeof(cfg);
    cfg.abi_version = VITA_TRACY_ABI_VERSION;

    int ret = vitaTracySetPmu(&cfg);
    Report("vitaTracySetPmu: %d", ret);
    if (ret < 0) {
        Report("");
        return;
    }

    /* Confirmed on hardware 2026-08-21: reading PMCCNTR from userland when
     * this reads 0 genuinely faults (privileged/undefined instruction,
     * caught pc inside CheckKernelPlugin -- not a branch-to-zero import
     * miss this time). kernel/pmu.c's own CP15 read-back, immediately
     * after the mcr write, in the SAME thread that wrote it, does see
     * PMUSERENR=1 on every one of the 4 per-core threads -- so the
     * enable genuinely takes on each core while that specific kernel
     * thread is the one running there, but does not appear to survive
     * for a DIFFERENT thread (this userland one) to observe afterward.
     * Still unexplained: docs/reverse_engineering.md's own finding that
     * context switches never touch CP15 c9 predicts this should persist
     * per-core regardless of which thread asks. Gating on this check
     * again after one confirmed crash trying to skip it. */
    int userenr = sceKernelGetPMUSERENR();
    uint32_t raw = ReadPmuserenrRaw();
    Report("sceKernelGetPMUSERENR after kernel enable: 0x%08X", (unsigned)userenr);
    Report("raw mrc c9,c14,0 on this same thread:       0x%08X", (unsigned)raw);
    Report("  %s", (unsigned)userenr == raw ? "agree -- Sce's function is just this same read"
                                             : "DISAGREE -- something between them isn't a raw read");
    if (userenr == 0) {
        Report("  still 0 -- see kernel/pmu.c's file comment for what's");
        Report("  confirmed and what's still open");
        Report("");
        return;
    }

    uint32_t before = ReadPmccntr();
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 2000000; ++i) {
        acc += i * 3u;
    }
    uint32_t after = ReadPmccntr();
    Report("PMCCNTR delta over 2M iterations (raw CP15 read): %u",
           (unsigned)vita_tracy_pmu_delta(before, after));
    Report("  a zero or absurd delta means the cycle counter is not really running");
    Report("");
}

void CheckKernelPlugin() {
    Report("-- kernel plugin --");

    if (!KernelPluginMarkerExists(kKernelPluginAttachMarkerPath)) {
        Report("skipped: create %s to try it,", kKernelPluginAttachMarkerPath);
        Report("  on a run AFTER tracy_kernel.skprx was loaded, never the");
        Report("  same run that loaded it");
        Report("");
        return;
    }

    int ret = vita_tracy_kernel_attach(0, 0);
    Report("vita_tracy_kernel_attach: %d", ret);
    if (ret != 0) {
        Report("  plugin absent or refused; zones still work without it");
        Report("");
        return;
    }

    Report("  attached, ring mapped and acknowledged");

    CheckKernelPmu();

    /* Kernel-owned PMCCNTR sampling: never touches userland CP15 at all
     * (see kernel/pmu.c). Started before the same 1s delay the regular
     * sampler already waits out below, so it accumulates over that same
     * window -- no extra wall-clock cost to test it. */
    int pmu_sample_start = vita_tracy_kernel_pmu_sample_start();
    Report("vitaTracyPmuSampleStart: %d", pmu_sample_start);

    int sampling = vita_tracy_kernel_set_sampling_ex(250, VITA_TRACY_SAMPLING_ALLOW_SUSPEND);
    Report("suspend diagnostics, timer 250 Hz (not CPU-time sampling): %d", sampling);

    sceKernelDelayThread(1000000);

    VitaTracyStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    int stats_ret = vita_tracy_kernel_get_stats(&stats);
    Report("vitaTracyGetStats: %d", stats_ret);
    if (stats_ret == 0) {
        for (int cpu = 0; cpu < 4; ++cpu) {
            Report("  ring %d: %u emitted, %u dropped", cpu, (unsigned)stats.samples_emitted[cpu],
                   (unsigned)stats.samples_dropped[cpu]);
        }
        Report("  control dropped: %u", (unsigned)stats.control_dropped);

        Report("  pmu busy-loop (5M iters, no sleep): %u cycles -- %u/iter",
               (unsigned)stats.pmu_busy_loop_cycles, (unsigned)(stats.pmu_busy_loop_cycles / 5000000u));

        /* kernel/pmu.c's VITA_TRACY_PMU_SAMPLE_INTERVAL_US, in ms; no shared
         * constant for it since only this bring-up test reads it back. */
        uint32_t elapsed_ms = stats.pmu_sample_ticks * 10u;
        Report("  pmu: %u ticks (~%u ms), cycle delta %u", (unsigned)stats.pmu_sample_ticks,
               (unsigned)elapsed_ms, (unsigned)stats.pmu_cycle_delta_total);
        if (elapsed_ms > 0) {
            uint64_t hz = ((uint64_t)stats.pmu_cycle_delta_total * 1000ull) / elapsed_ms;
            Report("  implied clock: %llu Hz -- a plausible CPU clock (hundreds of",
                   (unsigned long long)hz);
            Report("  MHz) means the kernel-owned sampler genuinely works; zero or");
            Report("  absurd means it doesn't");
        }
    }

    int pmu_sample_stop = vita_tracy_kernel_pmu_sample_stop();
    Report("vitaTracyPmuSampleStop: %d", pmu_sample_stop);

    Report("");
}

/* Named, nested, and varying in cost frame to frame -- gives the flame
 * graph and timeline actual shape to look at, instead of one flat "Zone"
 * bar per frame. Still just busywork; the point is the zone structure, not
 * the arithmetic. */
volatile uint32_t g_dummy_acc = 0;

void Spin(uint32_t iterations) {
    ZoneScoped;
    for (uint32_t i = 0; i < iterations; ++i) {
        g_dummy_acc += i * 3u;
    }
}

void Physics(uint32_t load) {
    ZoneScopedN("Physics");
    Spin(30000 + load);
}

void AI(uint32_t load) {
    ZoneScopedN("AI");
    Spin(8000 + load / 2);
}

void Update(uint32_t load) {
    ZoneScopedN("Update");
    Physics(load);
    AI(load);
}

void Culling(uint32_t load) {
    ZoneScopedN("Culling");
    Spin(6000 + load / 3);
}

void Draw(uint32_t load) {
    ZoneScopedN("Draw");
    Spin(40000 + load);
}

void Render(uint32_t load) {
    ZoneScopedN("Render");
    Culling(load);
    Draw(load);
}

void Work() {
    ZoneScopedN("Frame");

    static uint32_t frame = 0;
    ++frame;

    /* A triangle wave over ~60 frames so consecutive zones visibly differ
     * in width instead of all being identical. */
    uint32_t phase = frame % 60;
    uint32_t load = (phase < 30 ? phase : 60 - phase) * 1000;
    TracyPlot("frame load", (int64_t)load);

    Update(load);
    Render(load);
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
    LoadKernelPlugin();
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
