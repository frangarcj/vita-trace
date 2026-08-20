#include <psp2/sysmodule.h>

#include "vita_tracy/client.h"

namespace {
bool g_loaded_perf_module = false;
}

extern "C" {

int vita_tracy_init(void) {
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_PERF) != SCE_SYSMODULE_LOADED) {
        int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_PERF);
        if (ret < 0) {
            return ret;
        }
        g_loaded_perf_module = true;
    }

    vita_tracy_timebase_frequency();
    return 0;
}

void vita_tracy_shutdown(void) {
    if (g_loaded_perf_module) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_PERF);
        g_loaded_perf_module = false;
    }
}

} // extern "C"
