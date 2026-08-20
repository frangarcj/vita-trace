#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

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
constexpr uint32_t kDrainIntervalUs = 10000;

struct Bridge {
    SceUID memblock = -1;
    void *shared = nullptr;
    uint32_t size = 0;
    SceUID drain_thread = -1;
    volatile int draining = 0;
    VitaTracyClockSync clock{};
    uint32_t reported_drops = 0;
};

Bridge g_bridge;

/* Tracy owns the trace allocation once the item is committed. A kernel
 * sample carries a single PC, so the "callstack" is one frame deep; LR and
 * deeper unwinding arrive in a later phase. */
void EmitSample(const VitaTraceSample &sample) {
    int64_t time = vita_tracy_kernel_us_to_tracy_ns(&g_bridge.clock, sample.timestamp);

    auto *trace = (uint64_t *)tracy::tracy_malloc(2 * sizeof(uint64_t));
    trace[0] = 1;
    trace[1] = sample.pc;

    TracyLfqPrepare(tracy::QueueType::CallstackSample);
    tracy::MemWrite(&item->callstackSampleFat.time, time);
    tracy::MemWrite(&item->callstackSampleFat.thread, sample.tid);
    tracy::MemWrite(&item->callstackSampleFat.ptr, (uint64_t)trace);
    TracyLfqCommit;
}

void EmitControl(const VitaTraceControlRecord &record) {
    switch (record.type) {
    case VITA_TRACE_MODULE_SNAPSHOT: {
        /* Module bases travel as messages so the PC-to-source mapping can
         * be rebuilt on the PC; see tools/symbol_map.py. */
        const auto &mod = record.payload.module_snapshot;
        for (uint32_t i = 0; i < mod.segment_count; ++i) {
            char buf[160];
            snprintf(buf, sizeof(buf), "vita-tracy module %s nid=0x%08X seg=%u vaddr=0x%08X size=0x%X",
                     mod.module_name, (unsigned)mod.module_nid, (unsigned)i,
                     (unsigned)mod.segments[i].vaddr, (unsigned)mod.segments[i].memsz);
            TracyMessage(buf, strlen(buf));
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

int DrainThread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    while (g_bridge.draining) {
        void *control = vita_trace_shared_control_ring(g_bridge.shared);
        if (control != nullptr) {
            VitaTraceControlRecord record;
            while (vita_trace_ring_try_pop(control, &record)) {
                EmitControl(record);
            }
        }

        uint32_t dropped = 0;
        for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
            void *ring = vita_trace_shared_core_ring(g_bridge.shared, cpu);
            if (ring == nullptr) {
                continue;
            }
            VitaTraceSample sample;
            while (vita_trace_ring_try_pop(ring, &sample)) {
                EmitSample(sample);
            }
            dropped += vita_trace_ring_dropped(ring);
        }

        /* Overflow degrades into counted drops rather than blocking the
         * producer, so the loss has to be visible in the capture. */
        if (dropped != g_bridge.reported_drops) {
            g_bridge.reported_drops = dropped;
            TracyPlot("vita-tracy dropped samples", (int64_t)dropped);
        }

        sceKernelDelayThread(kDrainIntervalUs);
    }

    return 0;
}

uint32_t RoundUpTo4K(uint32_t value) {
    return (value + 0xFFFu) & ~0xFFFu;
}

} // namespace

extern "C" {

int vita_tracy_kernel_attach(uint32_t samples_per_core, uint32_t control_capacity) {
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
    if (needed == 0) {
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

    /* Bracket the syscall so the kernel's own clock reading is known to lie
     * between these two Tracy timestamps. */
    int64_t before = tracy_vita_get_time();
    ret = vitaTracyRegister(&args);
    int64_t after = tracy_vita_get_time();

    if (ret < 0) {
        sceKernelFreeMemBlock(memblock);
        return ret;
    }

    /* A weak import for an unloaded plugin may well return zero, so success
     * is only believed once the kernel has written into the mapping. */
    if (!vita_trace_shared_is_acknowledged(base)) {
        sceKernelFreeMemBlock(memblock);
        return VITA_TRACY_ERROR_UNSUPPORTED;
    }

    g_bridge.memblock = memblock;
    g_bridge.shared = base;
    g_bridge.size = block_size;
    g_bridge.reported_drops = 0;

    /* The register call emits a clock sync record carrying the kernel tick
     * taken inside that window. */
    uint64_t kernel_ref = 0;
    void *control = vita_trace_shared_control_ring(base);
    if (control != nullptr) {
        VitaTraceControlRecord record;
        if (vita_trace_ring_try_pop(control, &record) &&
            record.type == VITA_TRACE_CLOCK_SYNC) {
            kernel_ref = record.payload.clock_sync.kernel_tick;
        }
    }
    vita_tracy_clock_sync_set(&g_bridge.clock, before, after, kernel_ref);

    g_bridge.draining = 1;
    SceUID thid = sceKernelCreateThread("VitaTracyDrain", DrainThread, 0x40, 0x4000, 0, 0, nullptr);
    if (thid < 0) {
        g_bridge.draining = 0;
        vitaTracyUnregister(args.target_pid);
        sceKernelFreeMemBlock(memblock);
        g_bridge.shared = nullptr;
        g_bridge.memblock = -1;
        return thid;
    }

    g_bridge.drain_thread = thid;
    sceKernelStartThread(thid, 0, nullptr);
    return 0;
}

int vita_tracy_kernel_set_sampling(uint32_t frequency_hz) {
    if (g_bridge.shared == nullptr) {
        return VITA_TRACY_ERROR_STATE;
    }

    VitaTracySamplingConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = sizeof(cfg);
    cfg.abi_version = VITA_TRACY_ABI_VERSION;
    cfg.frequency_hz = frequency_hz;

    return vitaTracySetSampling(&cfg);
}

void vita_tracy_kernel_detach(void) {
    if (g_bridge.shared == nullptr) {
        return;
    }

    if (g_bridge.drain_thread >= 0) {
        g_bridge.draining = 0;
        sceKernelWaitThreadEnd(g_bridge.drain_thread, nullptr, nullptr);
        sceKernelDeleteThread(g_bridge.drain_thread);
        g_bridge.drain_thread = -1;
    }

    vitaTracyUnregister((uint32_t)sceKernelGetProcessId());

    sceKernelFreeMemBlock(g_bridge.memblock);
    g_bridge.memblock = -1;
    g_bridge.shared = nullptr;
    g_bridge.size = 0;
}

} // extern "C"
