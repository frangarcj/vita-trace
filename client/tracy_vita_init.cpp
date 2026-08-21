#include <psp2/sysmodule.h>

#include <client/TracyProfiler.hpp>

#include "vita_tracy/client.h"

namespace {
bool g_loaded_perf_module = false;
bool g_profiler_started = false;
int g_perf_module_status = 0;
} // namespace

extern "C" {

int vita_tracy_init(void) {
    /* ScePerf is an optimisation, not a dependency. It supplies a finer
     * clock than the process timer and the domain the kernel backend
     * correlates against, but the profiler has to come up either way:
     * letting an optional module decide whether Tracy starts leaves every
     * later Tracy call asserting on a profiler that was never built. */
    int resident = (sceSysmoduleIsLoaded(SCE_SYSMODULE_PERF) == SCE_SYSMODULE_LOADED);
    if (!resident) {
        g_perf_module_status = sceSysmoduleLoadModule(SCE_SYSMODULE_PERF);
        if (g_perf_module_status >= 0) {
            g_loaded_perf_module = true;
            resident = 1;
        }
    }

    vita_tracy_timebase_adopt_perf(resident);

#if defined(TRACY_DELAYED_INIT) && defined(TRACY_MANUAL_LIFETIME)
    if (!g_profiler_started) {
        tracy::StartupProfiler();
        g_profiler_started = true;
    }
#endif
    return 0;
}

int vita_tracy_perf_module_status(void) {
    return g_perf_module_status;
}

void vita_tracy_shutdown(void) {
#if defined(TRACY_DELAYED_INIT) && defined(TRACY_MANUAL_LIFETIME)
    if (g_profiler_started) {
        tracy::ShutdownProfiler();
        g_profiler_started = false;
    }
#endif
    if (g_loaded_perf_module) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_PERF);
        g_loaded_perf_module = false;
    }
}

} // extern "C"
