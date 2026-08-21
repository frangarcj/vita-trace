#include <stdlib.h>

#include "tracy_vita_platform.hpp"

extern "C" {

void *PlatformMalloc(size_t size) {
    return malloc(size);
}

void *PlatformRealloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void PlatformFree(void *ptr) {
    free(ptr);
}

/* newlib's allocator needs no per-process or per-thread setup, so Tracy's
 * init plumbing has nothing to do here. */
void PlatformAllocatorInit(void) {}

void PlatformAllocatorThreadInit(void) {}

void PlatformAllocatorThreadFinalize(void) {}

void PlatformAllocatorFinalize(void) {}

} // extern "C"
