#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>
#include <stdio.h>
#include <atomic>
#include "tracy_vita_lock.hpp"
#include "tracy_vita_platform.hpp"

#include <tracy/Tracy.hpp>
#include <client/TracyProfiler.hpp>

#include "vita_tracy/client.h"
#include "vita_tracy/clock_sync.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

namespace {

constexpr uint32_t kDefaultSamplesPerCore = 2048;
constexpr uint32_t kDefaultControlCapacity = 256;
constexpr uint32_t kBatchLimit = 256;
enum Command { None, Sampling, Stats, PmuConfigure, PmuStart, PmuStop, Detach };

struct Bridge {
    SceUID memblock = -1;
    void *shared = nullptr;
    uint32_t size = 0;
    SceUID drain_thread = -1;
    bool drain_started = false;
    std::atomic<bool> draining{false};
    std::atomic<int> command{None};
    std::atomic<int> command_result{0};
    SceUID command_done = -1;
    bool command_pending = false; // Only accessed under g_api_mutex.
    uint32_t requested_hz = 0;
    uint32_t requested_flags = 0;
    VitaTracyStats requested_stats{};
    VitaTracyPmuConfig requested_pmu{};
    VitaTracyClockSync clock{};
    uint32_t reported_drops = 0;
    uint32_t allocation_drops = 0;
    uint32_t tid_resolution_drops = 0;
};

Bridge g_bridge;
pthread_mutex_t g_api_mutex = PTHREAD_MUTEX_INITIALIZER;
// Names are part of Tracy's pointer-based protocol and must stay immutable
// until profiler shutdown, including across PMU reconfiguration/reconnection.
char g_pmu_names[VITA_TRACE_CORE_COUNT][256][64]{};
const char *const g_cycle_names[] = {"pmu c0 whole-core cycles/s", "pmu c1 whole-core cycles/s",
    "pmu c2 whole-core cycles/s", "pmu c3 whole-core cycles/s"};
const char *const g_interval_names[] = {"pmu c0 interval us", "pmu c1 interval us",
    "pmu c2 interval us", "pmu c3 interval us"};
const char *const g_gap_names[] = {"pmu c0 interval rejected", "pmu c1 interval rejected",
    "pmu c2 interval rejected", "pmu c3 interval rejected"};

void EmitTimedPlot(const char *name, uint64_t timestamp, double value) {
    TracyLfqPrepare(tracy::QueueType::PlotDataDouble);
    tracy::MemWrite(&item->plotDataDouble.name, (uint64_t)name);
    tracy::MemWrite(&item->plotDataDouble.time,
        vita_tracy_kernel_us_to_tracy_ns(&g_bridge.clock, timestamp));
    tracy::MemWrite(&item->plotDataDouble.val, value);
    TracyLfqCommit;
}

void EmitPmu(const VitaTracePmuSample &sample, uint32_t cpu) {
#ifdef TRACY_ON_DEMAND
    if (!tracy::GetProfiler().IsConnected()) return;
#endif
    if (sample.cpu != cpu || sample.count > VITA_PMU_EVENTS) return;
    const bool gap = sample.flags != 0 || !sample.elapsed_us ||
                     sample.elapsed_us > VITA_PMU_MAX_INTERVAL_US;
    EmitTimedPlot(g_gap_names[cpu], sample.timestamp, gap ? 1.0 : 0.0);
    if (gap) return; // An unknown interval is not a measurement of zero work.
    const double scale = 1000000.0 / sample.elapsed_us;
    EmitTimedPlot(g_interval_names[cpu], sample.timestamp, sample.elapsed_us);
    EmitTimedPlot(g_cycle_names[cpu], sample.timestamp, sample.cycles * scale);
    for (uint32_t i = 0; i < sample.count; ++i) {
        const uint32_t event = sample.events[i];
        if (event > 255u) continue;
        char *name = g_pmu_names[cpu][event];
        if (!name[0]) {
            const char *label = event == 0x68 ? "renamed" : event == 0x60 ? "icache-stall" :
                event == 0x61 ? "dcache-stall" : event == 0x03 ? "L1D-refill" :
                event == 0x01 ? "L1I-refill" : event == 0x10 ? "branch-mispredict" : "event";
            snprintf(name, 64, "pmu c%u whole-core %s 0x%02X/s", (unsigned)cpu, label, (unsigned)event);
        }
        EmitTimedPlot(name, sample.timestamp, sample.values[i] * scale);
    }
}

/* Tracy owns the trace allocation once the item is committed. A kernel
 * sample carries a single PC, so the "callstack" is one frame deep; LR and
 * deeper unwinding arrive in a later phase. */
bool ResolveSampleThread(const VitaTraceSample &sample, uint32_t &tid) {
    tid = sample.tid;
    if (sample.flags & VITA_TRACE_SAMPLE_GLOBAL_TID) {
        /* Resolve at drain time for every sample. VitaSDK exposes no public
         * thread start/exit observer that could invalidate a long-lived GUID
         * cache, and attributing a reused GUID to the old PUID is worse than
         * paying an explicit worker-side syscall. This is never done in IRQ. */
        const int resolved = vitaTracyResolveThread(sample.tid);
        if (resolved <= 0) {
            ++g_bridge.tid_resolution_drops;
            return false;
        }
        tid = (uint32_t)resolved;
    }

    if (g_bridge.shared) {
        const auto *header = (const VitaTraceSharedHeader *)g_bridge.shared;
        if (vita_trace_thread_contains(&header->profiler_threads, tid)) return false;
    }
    return true;
}

void EmitSample(const VitaTraceSample &sample) {
#ifdef TRACY_ON_DEMAND
    if (!tracy::GetProfiler().IsConnected()) return;
#endif
    uint32_t thread = 0;
    if (!ResolveSampleThread(sample, thread)) return;
    int64_t time = vita_tracy_kernel_us_to_tracy_ns(&g_bridge.clock, sample.timestamp);

    auto *trace = (uint64_t *)tracy::tracy_malloc(2 * sizeof(uint64_t));
    if (!trace) {
        ++g_bridge.allocation_drops;
        return;
    }
    trace[0] = 1;
    trace[1] = sample.pc;

    TracyLfqPrepare(tracy::QueueType::CallstackSample);
    tracy::MemWrite(&item->callstackSampleFat.time, time);
    tracy::MemWrite(&item->callstackSampleFat.thread, thread);
    tracy::MemWrite(&item->callstackSampleFat.ptr, (uint64_t)trace);
    TracyLfqCommit;
}

void EmitControl(const VitaTraceControlRecord &record) {
    switch (record.type) {
    case VITA_TRACE_MODULE_SNAPSHOT: {
        /* Module bases travel as messages so the PC-to-source mapping can
         * be rebuilt on the PC; see tools/symbol_map.py. */
        const auto &mod = record.payload.module_snapshot;
        for (uint32_t i = 0; i < mod.segment_count && i < VITA_TRACE_MODULE_MAX_SEGMENTS; ++i) {
            if (!mod.segments[i].memsz) continue;
            char buf[160];
            snprintf(buf, sizeof(buf), "vita-tracy module %.*s nid=0x%08X seg=%u vaddr=0x%08X size=0x%X",
                     (int)sizeof(mod.module_name), mod.module_name, (unsigned)mod.module_nid, (unsigned)i,
                     (unsigned)mod.segments[i].vaddr, (unsigned)mod.segments[i].memsz);
            TracyMessage(buf, strlen(buf));
            /* AppInfo is deferred across on-demand connections; an initial
             * module map must not disappear before the viewer connects. */
            TracyAppInfo(buf, strlen(buf));
        }
        break;
    }
    case VITA_TRACE_PROCESS_EXIT: {
        const char *msg = "vita-tracy target process exited";
        TracyMessage(msg, strlen(msg));
        break;
    }
    case VITA_TRACE_THREAD_START:
    case VITA_TRACE_THREAD_EXIT:
    case VITA_TRACE_PMU:
    case VITA_TRACE_CLOCK_SYNC:
    default:
        break;
    }
}

void EmitWakeStatus(uint32_t events) {
    for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
        const char *reasons[] = {"IRQ routed to wrong CPU", "counter access or ownership failure"};
        const uint32_t masks[] = {VITA_TRACY_WAKE_PMU_CPU(cpu), VITA_TRACY_WAKE_PMU_COUNTER(cpu)};
        for (uint32_t i = 0; i < 2; ++i) {
            if (!(events & masks[i])) continue;
            char text[160];
            snprintf(text, sizeof(text), "vita-tracy PMU c%u reader stopped: %s; explicit stop/restart required",
                     (unsigned)cpu, reasons[i]);
            TracyMessage(text, strlen(text));
            // Preserve failures for a viewer that connects after the IRQ stopped.
            TracyAppInfo(text, strlen(text));
        }
        if (events & VITA_TRACY_WAKE_SAMPLE_IRQ(cpu)) {
            char text[176];
            snprintf(text, sizeof(text),
                     "vita-tracy PC sampler c%u stopped: PMU ownership/configuration changed; explicit stop/restart required",
                     (unsigned)cpu);
            TracyMessage(text, strlen(text));
            TracyAppInfo(text, strlen(text));
        }
    }
}

bool DrainBatch() {
        bool remaining = false;
        void *control = vita_trace_shared_control_ring(g_bridge.shared);
        if (control != nullptr) {
            VitaTraceControlRecord record;
            uint32_t count = 0;
            while (count++ < kBatchLimit && vita_trace_ring_try_pop(control, &record)) {
                EmitControl(record);
            }
            remaining |= vita_trace_ring_pending(control) != 0;
        }

        uint32_t dropped = g_bridge.allocation_drops + g_bridge.tid_resolution_drops;
        if (control) dropped += vita_trace_ring_dropped(control);
        for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
            void *ring = vita_trace_shared_core_ring(g_bridge.shared, cpu);
            if (ring == nullptr) {
                continue;
            }
            VitaTraceSample sample;
            uint32_t count = 0;
            while (count++ < kBatchLimit && vita_trace_ring_try_pop(ring, &sample)) {
                EmitSample(sample);
            }
            dropped += vita_trace_ring_dropped(ring);
            remaining |= vita_trace_ring_pending(ring) != 0;
            void *pmu_ring = vita_trace_shared_pmu_ring(g_bridge.shared, cpu);
            if (pmu_ring) {
                VitaTracePmuSample pmu;
                count = 0;
                while (count++ < kBatchLimit && vita_trace_ring_try_pop(pmu_ring, &pmu)) EmitPmu(pmu, cpu);
                dropped += vita_trace_ring_dropped(pmu_ring);
                remaining |= vita_trace_ring_pending(pmu_ring) != 0;
            }
        }

        /* Overflow degrades into counted drops rather than blocking the
         * producer, so the loss has to be visible in the capture. */
        if (dropped != g_bridge.reported_drops) {
            g_bridge.reported_drops = dropped;
            TracyPlot("vita-tracy dropped records", (int64_t)dropped);
        }

        return remaining;
}

/* Once a second while a viewer is connected: the kernel sampler's counters
 * as plots, so a capture shows where samples go (kernel-mode overflows,
 * missed wraps, other processes) without a separate stats client. */
void EmitSamplerStats() {
#ifdef TRACY_ON_DEMAND
    if (!tracy::GetProfiler().IsConnected()) return;
#endif
    static uint64_t last_us = 0;
    const uint64_t now_us = sceKernelGetProcessTimeWide();
    if (now_us - last_us < 1000000u) return;
    last_us = now_us;
    VitaTracyStats stats{};
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    if (vitaTracyGetStats(&stats) < 0 || !stats.sample_irq_handler_registered) return;
    /* Tracy identifies plots by name pointer: literals only. */
    static const char *const names[3][6] = {
        {"vita-tracy c0 overflows", "vita-tracy c0 user samples", "vita-tracy c0 kernel-mode",
         "vita-tracy c0 missed wraps", "vita-tracy c0 other process", "vita-tracy c0 adopted"},
        {"vita-tracy c1 overflows", "vita-tracy c1 user samples", "vita-tracy c1 kernel-mode",
         "vita-tracy c1 missed wraps", "vita-tracy c1 other process", "vita-tracy c1 adopted"},
        {"vita-tracy c2 overflows", "vita-tracy c2 user samples", "vita-tracy c2 kernel-mode",
         "vita-tracy c2 missed wraps", "vita-tracy c2 other process", "vita-tracy c2 adopted"},
    };
    for (uint32_t cpu = 0; cpu < 3; ++cpu) {
        if (!(stats.sample_irq_core_mask & (1u << cpu))) continue;
        TracyPlot(names[cpu][0], (int64_t)stats.sample_irq_overflows[cpu]);
        TracyPlot(names[cpu][1], (int64_t)stats.samples_emitted[cpu]);
        TracyPlot(names[cpu][2], (int64_t)stats.sample_irq_kernel[cpu]);
        TracyPlot(names[cpu][3], (int64_t)stats.sample_irq_missed[cpu]);
        TracyPlot(names[cpu][4], (int64_t)stats.sample_irq_not_target[cpu]);
        TracyPlot(names[cpu][5], (int64_t)stats.sample_irq_adopted[cpu]);
    }
}

int DrainThread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    tracy_vita_profiler_thread_enter();
    while (g_bridge.draining.load(std::memory_order_acquire)) {
        const int command = g_bridge.command.exchange(None, std::memory_order_acq_rel);
        if (command != None) {
            int result;
            if (command == Sampling) {
                VitaTracySamplingConfig cfg{};
                cfg.size = sizeof(cfg);
                cfg.abi_version = VITA_TRACY_ABI_VERSION;
                cfg.frequency_hz = g_bridge.requested_hz;
                cfg.flags = g_bridge.requested_flags;
                result = vitaTracySetSampling(&cfg);
                if (result == 0 && cfg.frequency_hz && (cfg.flags & VITA_TRACY_SAMPLING_ALLOW_SUSPEND)) {
                    const char *warning = "vita-tracy: intrusive suspend diagnostics; not CPU-time samples";
                    TracyAppInfo(warning, strlen(warning));
                }
            } else if (command == Stats) {
                result = vitaTracyGetStats(&g_bridge.requested_stats);
            } else if (command == PmuStart) {
                result = vitaTracyPmuSampleStart();
                if (result == 0) {
                    const char *scope = "vita-tracy PMU: whole-core counts, not per-thread CPU time; 0x68 counts renamed instructions";
                    TracyAppInfo(scope, strlen(scope));
                }
            } else if (command == PmuConfigure) {
                result = vitaTracySetPmu(&g_bridge.requested_pmu);
            } else if (command == PmuStop) {
                result = vitaTracyPmuSampleStop();
            } else {
                /* Stop producers before releasing the mapping. No application
                 * thread becomes the sampler's permanently excluded caller. */
                result = vitaTracyUnregister((uint32_t)sceKernelGetProcessId());
                if (result == 0) {
                    while (DrainBatch()) {}
                    g_bridge.draining.store(false, std::memory_order_release);
                }
            }
            g_bridge.command_result.store(result, std::memory_order_release);
            sceKernelSignalSema(g_bridge.command_done, 1);
            if (command == Detach && result == 0) break;
        }
        if (DrainBatch()) continue;
        /* A retained event bit covers the drain-to-wait race, including a
         * command posted just before this blocking syscall. No idle polling. */
        const int ret = vitaTracyWaitForData(0);
        if (ret >= 0) EmitWakeStatus((uint32_t)ret);
        EmitSamplerStats();
        if (ret < 0) {
            g_bridge.command_result.store(ret, std::memory_order_release);
            g_bridge.draining.store(false, std::memory_order_release);
            sceKernelSignalSema(g_bridge.command_done, 1);
        }
    }
    tracy_vita_profiler_thread_exit();
    return 0;
}

/* An unsuccessful wait is not cancellation: the worker may still own the
 * payload. Retain it and consume its completion before allowing another call. */
int AwaitCommand() {
    if (!g_bridge.command_pending) return 0;
    const int ret = sceKernelWaitSema(g_bridge.command_done, 1, nullptr);
    if (ret >= 0) {
        (void)g_bridge.command_result.load(std::memory_order_acquire);
        g_bridge.command_pending = false;
    }
    return ret;
}

int ReadyForCommand() {
    const int ret = AwaitCommand();
    if (ret < 0) return ret;
    return g_bridge.draining.load(std::memory_order_acquire) ? 0 : VITA_TRACY_ERROR_STATE;
}

/* g_api_mutex serializes callers; the semaphore is the completion fence. */
int Submit(Command command) {
    int ret = ReadyForCommand();
    if (ret < 0) return ret;
    g_bridge.command_pending = true;
    g_bridge.command.store(command, std::memory_order_release);
    ret = vitaTracyWakeup();
    if (ret < 0) {
        int expected = command;
        if (g_bridge.command.compare_exchange_strong(expected, None, std::memory_order_acq_rel)) {
            g_bridge.command_pending = false;
            return ret;
        }
        // Already consumed: wait for completion before reusing the payload.
    }
    ret = AwaitCommand();
    return ret < 0 ? ret : g_bridge.command_result.load(std::memory_order_acquire);
}

uint32_t RoundUpTo4K(uint32_t value) {
    return (value + 0xFFFu) & ~0xFFFu;
}

/* Only after unregister has confirmed all kernel producers are stopped. */
int ReleaseBridgeResources() {
    int ret;
    if (g_bridge.drain_thread >= 0) {
        if (g_bridge.drain_started) {
            ret = sceKernelWaitThreadEnd(g_bridge.drain_thread, nullptr, nullptr);
            if (ret < 0) return ret;
        }
        ret = sceKernelDeleteThread(g_bridge.drain_thread);
        if (ret < 0) return ret;
        g_bridge.drain_thread = -1;
        g_bridge.drain_started = false;
    }
    tracy_vita_profiler_threads_bind(nullptr);
    if (g_bridge.command_done >= 0) {
        ret = sceKernelDeleteSema(g_bridge.command_done);
        if (ret < 0) return ret;
        g_bridge.command_done = -1;
    }
    ret = sceKernelFreeMemBlock(g_bridge.memblock);
    if (ret < 0) return ret;
    g_bridge.memblock = -1;
    g_bridge.shared = nullptr;
    g_bridge.size = 0;
    g_bridge.command_pending = false;
    return 0;
}

int RollbackAttach(int error) {
    const int ret = vitaTracyUnregister((uint32_t)sceKernelGetProcessId());
    if (ret < 0 && ret != VITA_TRACY_ERROR_TARGET) return ret;
    const int cleanup = ReleaseBridgeResources();
    return cleanup < 0 ? cleanup : error;
}


} // namespace

extern "C" {

int vita_tracy_kernel_attach(uint32_t samples_per_core, uint32_t control_capacity) {
    VitaTracyLockGuard lock(&g_api_mutex);
    if (g_bridge.shared != nullptr) {
        return VITA_TRACY_ERROR_STATE;
    }
    if (samples_per_core == 0) {
        samples_per_core = kDefaultSamplesPerCore;
    }
    if (control_capacity == 0) {
        control_capacity = kDefaultControlCapacity;
    }

    size_t needed = vita_trace_shared_layout_size(samples_per_core, control_capacity);
    if (needed == 0 || needed > UINT32_MAX - 0xFFFu) {
        return VITA_TRACY_ERROR_ARGS;
    }

    uint32_t block_size = RoundUpTo4K((uint32_t)needed);
    SceUID memblock = sceKernelAllocMemBlock("VitaTracyRing", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                             block_size, nullptr);
    if (memblock < 0) {
        return memblock;
    }

    void *base = nullptr;
    int ret = sceKernelGetMemBlockBase(memblock, &base);
    if (ret < 0) {
        sceKernelFreeMemBlock(memblock);
        return ret;
    }

    if (!vita_trace_shared_init(base, block_size, (uint32_t)sceKernelGetProcessId(),
                                vita_tracy_timebase_frequency(), samples_per_core,
                                control_capacity)) {
        sceKernelFreeMemBlock(memblock);
        return VITA_TRACY_ERROR_ARGS;
    }

    VitaTracyRegisterArgs args;
    memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.abi_version = VITA_TRACY_ABI_VERSION;
    args.target_pid = (uint32_t)sceKernelGetProcessId();
    args.ring_user_addr = (uint32_t)(uintptr_t)base;
    args.ring_size = block_size;
    tracy_vita_profiler_threads_bind(base);

    /* Bracket the syscall so the kernel's own clock reading is known to lie
     * between these two Tracy timestamps. */
    int64_t before = tracy_vita_get_time();
    ret = vitaTracyRegister(&args);
    int64_t after = tracy_vita_get_time();

    if (ret < 0) {
        tracy_vita_profiler_threads_bind(nullptr);
        sceKernelFreeMemBlock(memblock);
        return ret;
    }

    g_bridge.memblock = memblock;
    g_bridge.shared = base;
    g_bridge.size = block_size;
    /* Acknowledgement validates the handshake, not plugin residency. */
    if (!vita_trace_shared_is_acknowledged(base)) {
        /* The kernel returned success: unregister before freeing, even if
         * its acknowledgement was malformed. Missing imports are NOT safe
         * to probe; the caller must establish plugin residency first. */
        return RollbackAttach(VITA_TRACY_ERROR_UNSUPPORTED);
    }

    g_bridge.reported_drops = 0;
    g_bridge.allocation_drops = 0;
    g_bridge.tid_resolution_drops = 0;

    /* The register call emits a clock sync record carrying the kernel tick
     * taken inside that window. */
    uint64_t kernel_ref = 0;
    bool have_sync = false;
    void *control = vita_trace_shared_control_ring(base);
    if (control != nullptr) {
        VitaTraceControlRecord record;
        if (vita_trace_ring_try_pop(control, &record) &&
            record.type == VITA_TRACE_CLOCK_SYNC) {
            kernel_ref = record.payload.clock_sync.kernel_tick;
            have_sync = true;
        }
    }
    if (!have_sync) {
        return RollbackAttach(VITA_TRACY_ERROR_ABI);
    }
    vita_tracy_clock_sync_set(&g_bridge.clock, before, after, kernel_ref);

    g_bridge.command_done = sceKernelCreateSema("VitaTracyCommand", 0, 0, 1, nullptr);
    if (g_bridge.command_done < 0) {
        ret = g_bridge.command_done;
        return RollbackAttach(ret);
    }
    g_bridge.command.store(None, std::memory_order_relaxed);
    g_bridge.command_pending = false;
    g_bridge.draining.store(true, std::memory_order_release);
    SceUID thid = sceKernelCreateThread("VitaTracyDrain", DrainThread, 0x40, 0x4000, 0, 0, nullptr);
    g_bridge.drain_thread = thid >= 0 ? thid : -1;
    ret = thid < 0 ? thid : sceKernelStartThread(thid, 0, nullptr);
    g_bridge.drain_started = ret >= 0;
    if (ret < 0) {
        g_bridge.draining.store(false, std::memory_order_release);
        return RollbackAttach(ret);
    }

    g_bridge.drain_thread = thid;
    return 0;
}

int vita_tracy_kernel_set_sampling(uint32_t frequency_hz) {
    return vita_tracy_kernel_set_sampling_ex(frequency_hz, 0);
}

int vita_tracy_kernel_configure_pmu(const VitaTracyPmuConfig *config) {
    if (!config) return VITA_TRACY_ERROR_ARGS;
    VitaTracyLockGuard lock(&g_api_mutex);
    int ret = ReadyForCommand();
    if (ret < 0) return ret;
    if (!g_bridge.shared) return VITA_TRACY_ERROR_STATE;
    g_bridge.requested_pmu = *config;
    return Submit(PmuConfigure);
}

int vita_tracy_kernel_set_sampling_ex(uint32_t frequency_hz, uint32_t flags) {
    VitaTracyLockGuard lock(&g_api_mutex);
    int ret = ReadyForCommand();
    if (ret < 0) return ret;
    if (g_bridge.shared == nullptr) {
        return VITA_TRACY_ERROR_STATE;
    }

    g_bridge.requested_hz = frequency_hz;
    g_bridge.requested_flags = flags;
    return Submit(Sampling);
}

void vita_tracy_kernel_detach(void) {
    (void)vita_tracy_kernel_detach_checked();
}

int vita_tracy_kernel_get_stats(VitaTracyStats *stats) {
    if (!stats) return VITA_TRACY_ERROR_ARGS;
    VitaTracyLockGuard lock(&g_api_mutex);
    int ret = ReadyForCommand();
    if (ret < 0) return ret;
    g_bridge.requested_stats = *stats;
    ret = Submit(Stats);
    if (ret == 0) *stats = g_bridge.requested_stats;
    return ret;
}

int vita_tracy_kernel_pmu_sample_start(void) {
    VitaTracyLockGuard lock(&g_api_mutex);
    return Submit(PmuStart);
}

int vita_tracy_kernel_pmu_sample_stop(void) {
    VitaTracyLockGuard lock(&g_api_mutex);
    return Submit(PmuStop);
}

int vita_tracy_kernel_detach_checked(void) {
    VitaTracyLockGuard lock(&g_api_mutex);
    if (g_bridge.shared == nullptr) {
        return 0;
    }

    int ret = AwaitCommand();
    if (ret < 0) return ret;
    if (g_bridge.draining.load(std::memory_order_acquire)) {
        ret = Submit(Detach);
        if (ret < 0) return ret;
    } else {
        // Recover a failed worker or partially initialized attachment. Do not
        // free memory while the kernel might still hold the mapping.
        ret = vitaTracyUnregister((uint32_t)sceKernelGetProcessId());
        if (ret < 0 && ret != VITA_TRACY_ERROR_TARGET) return ret;
    }
    return ReleaseBridgeResources();
}

} // extern "C"
