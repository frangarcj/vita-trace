#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>

#include <stdlib.h>

#include <tracy/Tracy.hpp>

#include "vita_tracy/client.h"
#include "vita_tracy/pmu.h"

/* Cache-hostile against cache-friendly access over the same buffer. The
 * dcache miss plot should separate the two clearly; if it does not, the
 * counters are not measuring what their event codes claim. */

namespace {

constexpr size_t kBufferBytes = 4 * 1024 * 1024;
volatile uint32_t g_sink = 0;

void SequentialPass(uint8_t *buffer) {
    ZoneScopedN("sequential");
    uint32_t acc = 0;
    for (size_t i = 0; i < kBufferBytes; i += 64) {
        acc += buffer[i];
    }
    g_sink += acc;
}

void StridedPass(uint8_t *buffer) {
    ZoneScopedN("strided");
    uint32_t acc = 0;
    /* A stride larger than a cache line and coprime with the set count
     * defeats both spatial locality and the prefetcher. */
    size_t index = 0;
    for (size_t i = 0; i < kBufferBytes / 64; ++i) {
        index = (index + 4157) % kBufferBytes;
        acc += buffer[index];
    }
    g_sink += acc;
}

} // namespace

int main() {
    vita_tracy_init();
    tracy::SetThreadName("pmu_cache");

    static const uint8_t events[] = {
        SCE_PERF_ARM_PMON_CYCLE_COUNT,
        SCE_PERF_ARM_PMON_DCACHE_MISS,
        SCE_PERF_ARM_PMON_DCACHE_ACCESS,
    };
    vita_tracy_pmu_begin(events, sizeof(events) / sizeof(events[0]));

    auto *buffer = (uint8_t *)malloc(kBufferBytes);
    if (buffer != nullptr) {
        for (size_t i = 0; i < kBufferBytes; ++i) {
            buffer[i] = (uint8_t)i;
        }

        for (int i = 0; i < 120; ++i) {
            SequentialPass(buffer);
            vita_tracy_pmu_sample();
            StridedPass(buffer);
            vita_tracy_pmu_sample();
            FrameMark;
        }
        free(buffer);
    }

    vita_tracy_pmu_end();
    vita_tracy_shutdown();

    sceKernelExitProcess(0);
    return 0;
}
