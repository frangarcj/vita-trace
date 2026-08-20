#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include <tracy/Tracy.hpp>

#include "vita_tracy/client.h"
#include "vita_tracy/pmu.h"

/* A perfectly predictable branch against an unpredictable one, with the
 * same instruction count and the same number of branches taken, so the
 * mispredict plot isolates prediction rather than work done. */

namespace {

constexpr uint32_t kIterations = 400000;
volatile uint32_t g_sink = 0;
uint32_t g_pattern[256];

void PredictablePass() {
    ZoneScopedN("predictable");
    uint32_t acc = 0;
    for (uint32_t i = 0; i < kIterations; ++i) {
        /* Alternates on a fixed period the predictor learns immediately. */
        if ((i & 1u) == 0) {
            acc += 3;
        } else {
            acc ^= 5;
        }
    }
    g_sink += acc;
}

void UnpredictablePass() {
    ZoneScopedN("unpredictable");
    uint32_t acc = 0;
    for (uint32_t i = 0; i < kIterations; ++i) {
        if (g_pattern[i & 0xFFu] & 1u) {
            acc += 3;
        } else {
            acc ^= 5;
        }
    }
    g_sink += acc;
}

} // namespace

int main() {
    vita_tracy_init();
    tracy::SetThreadName("pmu_branch");

    /* A cheap xorshift gives a pattern with no period the predictor can
     * latch onto. */
    uint32_t seed = 0x12345678u;
    for (uint32_t i = 0; i < 256; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        g_pattern[i] = seed;
    }

    static const uint8_t events[] = {
        SCE_PERF_ARM_PMON_CYCLE_COUNT,
        SCE_PERF_ARM_PMON_BRANCH_MISPREDICT,
        SCE_PERF_ARM_PMON_PREDICT_BRANCH,
    };
    vita_tracy_pmu_begin(events, sizeof(events) / sizeof(events[0]));

    for (int i = 0; i < 120; ++i) {
        PredictablePass();
        vita_tracy_pmu_sample();
        UnpredictablePass();
        vita_tracy_pmu_sample();
        FrameMark;
    }

    vita_tracy_pmu_end();
    vita_tracy_shutdown();

    sceKernelExitProcess(0);
    return 0;
}
