#pragma once

/* Platform hooks Tracy pulls in through TRACY_PLATFORM_HEADER.
 *
 * Tracy has no branch for the Vita, so two things must be supplied here:
 *
 *  - The kernel thread identifier. Tracy's generic path ends in an #error
 *    because pthread_t is a library pointer and is neither unique nor
 *    reliably 32-bit; sceKernelGetThreadId() is the real kernel id.
 *  - The clock. Without this hook Tracy falls back to
 *    std::chrono::high_resolution_clock, which on VitaSDK resolves to the
 *    non-monotonic wall clock behind sceRtcGetTime_t, so a clock adjustment
 *    mid-capture would move timestamps backwards. ScePerf's timebase is
 *    monotonic and is also the domain the kernel backend correlates
 *    against. The hook itself is the TRACY_PLATFORM_GET_TIME patch in
 *    patches/tracy/.
 *
 * Tracy's bundled rpmalloc is also unusable here: it allocates through
 * mmap, and VitaSDK has no <sys/mman.h>. TRACY_HAS_CUSTOM_ALLOCATOR routes
 * those allocations to newlib instead.
 *
 * The remaining hooks stand in for POSIX calls newlib does not provide:
 * TRACY_HAS_CUSTOM_USER_INFO replaces getpwuid_r/gethostname, and
 * TRACY_HAS_CUSTOM_SAFE_COPY replaces the pipe-based safe read, which
 * relies on F_SETPIPE_SZ. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t PlatformGetThreadId(void);
int64_t tracy_vita_get_time(void);
void tracy_vita_profiler_thread_enter(void);
void tracy_vita_profiler_thread_exit(void);
void tracy_vita_profiler_threads_bind(void *shared);

void *PlatformMalloc(size_t size);
void *PlatformRealloc(void *ptr, size_t size);
void PlatformFree(void *ptr);
void PlatformAllocatorInit(void);
void PlatformAllocatorThreadInit(void);
void PlatformAllocatorThreadFinalize(void);
void PlatformAllocatorFinalize(void);

const char *PlatformGetUserLogin(void);
const char *PlatformGetUserFullName(void);
void PlatformGetHostname(char *buf, size_t size);

bool PlatformSafeMemcpy(void *dst, const void *src, size_t size);

#ifdef __cplusplus
}
#endif

#define TRACY_HAS_CUSTOM_THREAD_ID
#define TRACY_HAS_CUSTOM_ALLOCATOR
#define TRACY_HAS_CUSTOM_USER_INFO
#define TRACY_HAS_CUSTOM_SAFE_COPY
#define TRACY_PLATFORM_GET_TIME tracy_vita_get_time
#define TRACY_PLATFORM_THREAD_ENTER tracy_vita_profiler_thread_enter
#define TRACY_PLATFORM_THREAD_EXIT tracy_vita_profiler_thread_exit
