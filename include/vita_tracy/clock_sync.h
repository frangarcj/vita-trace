#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maps kernel timestamps into the Tracy timeline.
 *
 * The kernel stamps events with ksceKernelGetSystemTimeWide (microseconds)
 * while the client stamps zones with the ScePerf timebase converted to
 * nanoseconds. The two are different counters, so a reference pair is taken
 * once and every kernel timestamp is shifted onto the Tracy timeline
 * against it.
 *
 * The reference is captured by reading the Tracy clock immediately before
 * and after the register syscall: the kernel's own reading happened
 * somewhere between the two, so the midpoint bounds the error by the
 * duration of that one syscall. */
typedef struct VitaTracyClockSync {
    uint64_t kernel_ref_us;
    int64_t tracy_ref_ns;
} VitaTracyClockSync;

/* Builds the reference from a bracketed measurement. */
void vita_tracy_clock_sync_set(VitaTracyClockSync *sync, int64_t tracy_before_ns,
                               int64_t tracy_after_ns, uint64_t kernel_us);

/* Converts a kernel microsecond timestamp to a Tracy nanosecond timestamp.
 * Kernel timestamps taken before the reference map to earlier Tracy times,
 * so the delta is signed. */
int64_t vita_tracy_kernel_us_to_tracy_ns(const VitaTracyClockSync *sync, uint64_t kernel_us);

#ifdef __cplusplus
}
#endif
