#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <tracy/Tracy.hpp>

#include "vita_tracy/client.h"

namespace {

/* Two workloads with a known time split, so the viewer can be checked
 * against an expected shape rather than against "it looks busy". */
void HotWork() {
    ZoneScoped;
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 200000; ++i) {
        acc += i * 3u;
    }
}

void ColdWork() {
    ZoneScoped;
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 20000; ++i) {
        acc ^= i;
    }
}

void Frame() {
    ZoneScopedN("Frame");
    HotWork();
    ColdWork();
}

} // namespace

int main() {
    vita_tracy_init();
    tracy::SetThreadName("main");

    for (int i = 0; i < 60 * 60; ++i) {
        Frame();
        FrameMark;
        sceKernelDelayThread(16000);
    }

    vita_tracy_shutdown();
    sceKernelExitProcess(0);
    return 0;
}
