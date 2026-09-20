#include <cstdio>
#include <cstring>
#include <mutex>
#include <tracy/Tracy.hpp>
#if VITA_TRACY_AUTO_FRAMES
#include <psp2/display.h>
#endif

namespace {
std::mutex emission_mutex;
bool active = false;
}

extern "C" void vita_tracy_auto_activate(int enabled) {
    // Closing waits for any in-flight frame emission before Tracy is destroyed.
    std::lock_guard<std::mutex> lock(emission_mutex);
    active = enabled != 0;
}

extern "C" void vita_tracy_auto_report(const char *stage, int result) {
    char message[128];
    std::snprintf(message, sizeof(message), "vita-tracy automatic %s: %d", stage, result);
    TracyAppInfo(message, std::strlen(message));
}

#if VITA_TRACY_AUTO_FRAMES
extern "C" int __real_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *, SceDisplaySetBufSync);
extern "C" int __wrap_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *frame, SceDisplaySetBufSync sync) {
    const int ret = __real_sceDisplaySetFrameBuf(frame, sync);
    if (ret >= 0 && frame && frame->base) {
        std::lock_guard<std::mutex> lock(emission_mutex);
        if (active) FrameMarkNamed("Vita display submit");
    }
    return ret;
}
#endif
