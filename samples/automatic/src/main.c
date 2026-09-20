#include <stdint.h>
#include <psp2/kernel/threadmgr.h>

/* Deliberately no profiler headers or calls. All integration is in CMake. */
static volatile uint32_t sink;
__attribute__((noinline)) static void work(uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) sink = sink * 1664525u + i + 1013904223u;
}

int main(void) {
    for (uint32_t frame = 0; frame < 1800; ++frame) {
        work(frame % 120 < 60 ? 400000 : 100000);
        sceKernelDelayThread(16000);
    }
    return 0;
}
