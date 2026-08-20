#include <string.h>

#include <psp2/system_param.h>

#include "tracy_vita_platform.hpp"

extern "C" {

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
