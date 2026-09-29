#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <stdio.h>

#include "tracy_vita_platform.hpp"

extern "C" {

/* Context-switch records only carry this process's threads, so the thread
 * manager can name them; one that has already exited gets its id. */
void tracy_vita_external_name(uint64_t thread, char *thread_name, char *process_name, size_t size) {
    if (!size) return;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo((SceUID)thread, &info) >= 0 && info.name[0])
        snprintf(thread_name, size, "%s", info.name);
    else
        snprintf(thread_name, size, "thread 0x%08X", (unsigned)thread);
    snprintf(process_name, size, "%s", "profiled application");
}

/* The Vita has no login accounts, so the session is identified by the
 * console rather than by a user. */
const char *PlatformGetUserLogin(void) {
    return "vita";
}

const char *PlatformGetUserFullName(void) {
    return nullptr;
}

void PlatformGetHostname(char *buf, size_t size) {
    if (size == 0) {
        return;
    }
    strncpy(buf, "psvita", size - 1);
    buf[size - 1] = '\0';
}

/* Tracy uses this to read memory the viewer asked for, which may no longer
 * be mapped. On Linux it launders the read through a pipe so the kernel
 * reports EFAULT instead of faulting the process; the Vita offers userland
 * no equivalent probe, so the copy is unguarded. Reachable only through
 * callstack and memory queries, both of which this port leaves disabled. */
bool PlatformSafeMemcpy(void *dst, const void *src, size_t size) {
    memcpy(dst, src, size);
    return true;
}

} // extern "C"
