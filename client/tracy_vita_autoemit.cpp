#include <cstdio>
#include <cstring>
#include "tracy_vita_lock.hpp"
#include <tracy/Tracy.hpp>
#if VITA_TRACY_AUTO_FRAMES
#include <psp2/display.h>
#endif
#if defined(__vita__)
#include <psp2/io/stat.h>
#endif

namespace {
pthread_mutex_t emission_mutex = PTHREAD_MUTEX_INITIALIZER;
bool active = false;
}

extern "C" void vita_tracy_auto_activate(int enabled) {
    // Closing waits for any in-flight frame emission before Tracy is destroyed.
    VitaTracyLockGuard lock(&emission_mutex);
    active = enabled != 0;
}

extern "C" void vita_tracy_auto_report(const char *stage, int result) {
    char message[128];
    std::snprintf(message, sizeof(message), "vita-tracy automatic %s: %d", stage, result);
    TracyAppInfo(message, std::strlen(message));
}

extern "C" int vita_tracy_auto_kernel_opt_in(void) {
#if defined(__vita__)
    SceIoStat stat;
    return sceIoGetstat("ux0:data/vita-tracy/kernel.on", &stat) >= 0;
#else
    return 0;
#endif
}

#if VITA_TRACY_AUTO_FRAMES
extern "C" int __real_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *, SceDisplaySetBufSync);
extern "C" int __wrap_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *frame, SceDisplaySetBufSync sync) {
    const int ret = __real_sceDisplaySetFrameBuf(frame, sync);
    if (ret >= 0 && frame && frame->base) {
        VitaTracyLockGuard lock(&emission_mutex);
        if (active) FrameMarkNamed("Vita display submit");
    }
    return ret;
}
#endif
