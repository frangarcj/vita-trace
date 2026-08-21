#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include <stdio.h>

#include <tracy/Tracy.hpp>

#include "import_check.h"

#include "vita_tracy/pmu.h"

/* Reads PMUSERENR, the register that decides whether userland may touch the
 * performance monitors at all. ScePerf gates every PMU entry point on it and
 * fails early when it reads zero, so checking it first turns "the counters
 * silently refuse" into something diagnosable. VitaSDK ships the stub but
 * declares it in no header. */
extern "C" int sceKernelGetPMUSERENR(void);

namespace {

struct CounterState {
    uint8_t event_code;
    uint32_t previous;
    char plot_name[48];
};

CounterState g_counters[VITA_TRACY_PMU_MAX_PROBE];
uint32_t g_counter_count = 0;
bool g_running = false;

const char *EventName(uint8_t code) {
    switch (code) {
    case SCE_PERF_ARM_PMON_CYCLE_COUNT:
        return "cycles";
    case SCE_PERF_ARM_PMON_ICACHE_MISS:
        return "icache miss";
    case SCE_PERF_ARM_PMON_DCACHE_MISS:
        return "dcache miss";
    case SCE_PERF_ARM_PMON_DCACHE_STALL:
        return "dcache stall";
    case SCE_PERF_ARM_PMON_DTLB_MISS:
        return "dtlb miss";
    case SCE_PERF_ARM_PMON_BRANCH_MISPREDICT:
        return "branch mispredict";
    default:
        return nullptr;
    }
}

} // namespace

extern "C" {

uint32_t vita_tracy_pmu_begin(const uint8_t *event_codes, uint32_t count) {
    if (event_codes == nullptr || count == 0 || g_running) {
        return 0;
    }
    if (count > VITA_TRACY_PMU_MAX_PROBE) {
        count = VITA_TRACY_PMU_MAX_PROBE;
    }

    /* Every scePerfArmPmon* entry point is a ScePerf import, so it is only
     * safe to call once that module is loaded and bound. */
    if (!vita_tracy_import_resolved((const void *)&scePerfArmPmonReset)) {
        return 0;
    }
    if (sceKernelGetPMUSERENR() == 0) {
        return 0;
    }

    /* Always the calling thread. ScePerf services that case by writing the
     * CP15 monitor registers directly, whereas naming another thread routes
     * through ScePamgr, which retail firmware dropped in 3.50. */
    SceUID self = SCE_PERF_ARM_PMON_THREAD_ID_SELF;
    if (scePerfArmPmonReset(self) < 0) {
        return 0;
    }

    g_counter_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        /* How many counters retail firmware leaves programmable is not
         * documented, so the count comes from the hardware rejecting the
         * first one it will not take. */
        if (scePerfArmPmonSelectEvent(self, i, event_codes[i]) < 0) {
            break;
        }

        CounterState &state = g_counters[g_counter_count];
        state.event_code = event_codes[i];
        state.previous = 0;

        const char *name = EventName(event_codes[i]);
        if (name != nullptr) {
            snprintf(state.plot_name, sizeof(state.plot_name), "pmu %s", name);
        } else {
            snprintf(state.plot_name, sizeof(state.plot_name), "pmu event 0x%02X",
                     (unsigned)event_codes[i]);
        }

        g_counter_count++;
    }

    if (g_counter_count == 0) {
        return 0;
    }

    if (scePerfArmPmonStart(self) < 0) {
        g_counter_count = 0;
        return 0;
    }

    g_running = true;
    return g_counter_count;
}

void vita_tracy_pmu_sample(void) {
    if (!g_running) {
        return;
    }

    SceUID self = SCE_PERF_ARM_PMON_THREAD_ID_SELF;
    for (uint32_t i = 0; i < g_counter_count; ++i) {
        SceUInt32 value = 0;
        if (scePerfArmPmonGetCounterValue(self, i, &value) < 0) {
            continue;
        }
        CounterState &state = g_counters[i];
        uint32_t delta = vita_tracy_pmu_delta(state.previous, value);
        state.previous = value;
        TracyPlot(state.plot_name, (int64_t)delta);
    }
}

void vita_tracy_pmu_end(void) {
    if (!g_running) {
        return;
    }
    scePerfArmPmonStop(SCE_PERF_ARM_PMON_THREAD_ID_SELF);
    g_running = false;
    g_counter_count = 0;
}

} // extern "C"
