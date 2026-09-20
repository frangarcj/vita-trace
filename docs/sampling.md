# Sampling and transport (ABI 3)

## What this revision changes

Tracy's POSIX worker entry/exit now maintains an explicit registry of the
profiler's process-local thread IDs. The kernel converts global thread IDs
with `ksceKernelGetUserThreadId` before consulting it or emitting a sample.
No thread is excluded because it is named `pthread`. A full registry disables
diagnostic sampling rather than risking suspension of an unlisted worker.

The bridge worker executes sampling control requests, so asking for sampling
from `main` no longer makes `main` the permanently excluded control thread.
Direct users of the raw kernel ABI still have the diagnostic control-caller
exclusion and must manage their own thread ownership.
The client wrappers `vita_tracy_kernel_get_stats` and
`vita_tracy_kernel_pmu_sample_start/stop` use the same worker, avoiding raw
profiler syscalls from an application thread while the diagnostic is active.

The consumer blocks in `vitaTracyWaitForData`, woken by a retained event bit
for data batches or commands. An event arriving between the last drain and
the wait is retained. Each drain batch is bounded so a continuous producer
cannot starve stop/reconfigure commands. This removes the bridge's 10 ms
idle polling; it does not remove Tracy's own networking implementation.

A `SceSysTimer` supplies the diagnostic cadence. Its fixed 48 MHz source is
prescaled to 1 MHz, with an interval of `1000000 / requested_hz` ticks. Integer
rounding means the effective cadence can differ slightly for non-divisor
rates. The interrupt signals an event and does no allocation, thread
suspension, register inspection, symbol lookup or network I/O. The worker
waits for that event. Overrun ticks coalesce rather than causing a burst of
stale scans. `timer_ticks` and `diagnostic_batches` expose the difference.

The timer is allocated through SceSystimer. The implementation does not steal
the private timer, change VBAR, detach a CPU or install guest page tables.
If allocation/configuration fails, sampling returns an error; there is no
silent fallback to a delay loop. A failed timer release retains the handles
and prevents module unload until release succeeds.

## A timer is not an interrupted PC

`SceSysTimerCallback(timer_id, user_data)` does not receive the interrupted
register frame. The IRQ-to-PC adapter is still unimplemented. Reading the
callback's own PC or calling the suspended-thread API on a running thread
would not fix that.

`vita_tracy_kernel_set_sampling(hz)` therefore requests a non-intrusive
source and currently returns `VITA_TRACY_ERROR_UNSUPPORTED` for nonzero rates.
It does not silently suspend the application. Passing zero stops sampling.

The provisional diagnostic is explicitly opt-in:

```cpp
#include "vita_tracy/abi.h"
#include "vita_tracy/client.h"

// Only after the matching ABI-3 plugin is known to be loaded.
int attached = vita_tracy_kernel_attach(0, 0);
if (attached == 0) {
    int result = vita_tracy_kernel_set_sampling_ex(
        250, VITA_TRACY_SAMPLING_ALLOW_SUSPEND);
    // Check result: a timer or thread allocation can fail.
}
```

This still suspends eligible threads and is not a production CPU-time
profiler. Thread-state checks reduce obvious sleeping-thread samples but are
not atomic with suspension. Suspension can perturb locks and scheduling or
hang an application. The uncertain debugger register-set/status semantics
also remain. Do not interpret these samples as unbiased CPU percentages.
Each diagnostic sample carries a diagnostic flag and `CPU_UNKNOWN`; ring 0
is transport, not evidence that the target ran on CPU 0. A resume failure
increments a counter and stops further scans; it may require a restart of
the target. The two modes must remain distinct in future backends.

## Whole-core PMU capture from timer interrupts

The legacy `kernel/pmu.c` experiment has been replaced by
`kernel/pmu_session.c`, `kernel/pmu_arm.c` and `common/pmu_core.c`. Each selected
core gets a system timer and a separate SPSC PMU ring. Its IRQ callback reads
the bank on that same core, stores a fixed-size vector with timestamp and
actual elapsed microseconds, and signals the consumer. It never enumerates
or suspends application threads and never enables userland PMU access.

Short-lived pinned jobs perform acquisition/restoration with local IRQs
disabled. There is no resident PMU polling thread or diagnostic busy loop.
Defaults are app cores 0..2 at 100 Hz, cycle-only unless events are configured;
the automatic CMake mode explicitly configures six events. CPU 3 is opt-in.
The IRQ checks its actual core before touching CP15 or its SPSC ring.
An incorrectly routed callback disables that reader, clears its active bit
and reports `VITA_TRACY_ERROR_CPU`. It does not touch the wrong core's bank,
silently recover on a later tick, or stop the other healthy readers. Stop
and restart explicitly once the routing problem has been diagnosed.

PMU routing/counter failures also set retained wake bits, independently of
sample/control ring capacity. `vitaTracyWaitForData` now returns a nonnegative
`VITA_TRACY_WAKE_*` mask on success (not necessarily zero). The bridge turns
failure bits into Tracy messages and deferred AppInfo on its ordinary worker;
it does not log from the IRQ or poll `GetStats`. Detailed error counters remain
available through the stats API. A wake can coalesce failures from several
cores; it is not a count of occurrences. Unconsumed notifications are discarded
when the old session is replaced, not delivered to a new target. Process death
or shutdown before the worker consumes an event can still prevent delivery.

The PMU must be unused (counter and interrupt enables clear) before acquisition.
The backend saves disabled counter values/types, selector and control state,
checks observable ownership before reads/restoration, and retains live handles
when cleanup cannot complete. This is not a lock respected by other plugins:
another writer using the exact same configuration is not detectable. No claim
of arbitrary PMU sharing or scheduler virtualization is made.

Tracy receives timestamped whole-core cycles/events per second using each
record's actual interval, not its drain time or an assumed 10 ms period.
0x68 means renamed instructions, not retired instructions. Values include
other processes, the kernel and this profiler on that core. The counter bank
continues running during sequential register reads; the vector is not a
hardware-latched simultaneous snapshot.

The first callback establishes a baseline. Backward/zero time and excessive
intervals are marked as gaps rather than invented zero work. Ring overflow
drops the new record, increments losses and leaves a sequence gap. A counter
ownership failure disables that core's reader, clears its active-mask bit
and records the error; starting an unhealthy running session does not return
success. Stop and retry explicitly after diagnosing the conflict.

On stop, callback admission closes before resources are freed. A callback
already running makes cleanup return busy and retain its timer, ring and
PMU state for retry. Timer teardown must be validated on hardware before
claiming safe unload. The host tests exercise this state machine but cannot
prove that the firmware's timer-free function has the assumed IRQ semantics.

## Compatibility and lifetime

Rebuild the client, kernel plugin and sample applications together. The
shared/control ABI is now version 3 and the exported library version is 25.
Do not run a new client against an old plugin or vice versa.

Weak imports allow an application to load without the plugin; they do not
make calling an unresolved import safe. The kernel acknowledgement validates
a completed handshake, not plugin residency. The standalone synthetic
sample requires the matching plugin already loaded. The bring-up sample
retains its explicit plugin-loading path.

The application must stop producing Tracy zones before shutting down Tracy.
`vita_tracy_shutdown` first detaches the bridge and will not destroy Tracy
if detachment fails. `vita_tracy_kernel_detach_checked` reports that failure
for callers that need to handle it. Stop and join producers before releasing
shared memory; do not free on an unconfirmed unregister operation.

`vita_tracy_shutdown_checked` reports a failed detach or optional-module
unload. Repeated initialization does not change a live profiler's timebase.
Bridge commands retain their own payload until completion; an interrupted
or failed wait does not make a caller's stack pointer available to a worker.
Failed thread joins/deletions, semaphore deletions and memblock frees retain
their handles, and callers can retry checked detach. Normal application
workers must still be stopped before shutdown. Do not unload the kernel
provider while any process still uses its imports or has a call in flight.

## Offline symbols

Preserve the unstripped ARM ELF from exactly the build being captured. The
module's segment index is matched to the ELF's PT_LOAD order, ignoring other
program-header types. Symbol lookup uses:

```
ELF_PC = runtime_PC - runtime_segment_base + ELF_segment_p_vaddr
```

The CLI removes a Thumb pointer's low bit before lookup, checks segment
bounds/executable permissions, and reports unresolved symbols as failures.

```sh
python3 tools/symbol_map.py --messages modules.txt \
    --elf MyApp=build/my_app --elf MyLibrary=build/my_library \
    --pc 0x93400125
```

A bare `--elf path` is accepted only for a capture with one module. Multiple
modules require explicit names; the tool never applies one ELF to all of
them. Matching NIDs and segment shapes is not a cryptographic build-identity
check. The caller must supply the right binaries. Module unload/reload
history at different addresses and offline stack unwinding remain future
work: split such captures before using this last-placement map. A changed
NID under the same module name is rejected rather than mixed silently.

Module placement records are also deferred Tracy AppInfo, preserving initial
metadata for an on-demand viewer that connects after attachment.

## Validation

Host tests cover registry lifecycle/overflow, real ARM ELF symbol resolution
when VitaSDK is on PATH, layout bounds and the actual timer lifecycle code
against fake APIs with failure injection. Fake timer tests cover retained
notifications, coalesced ticks, stop wakeups, startup rollback and failed
release. They do not establish interrupt behavior on a real console.

PMU tests cover occupied banks, preservation, ownership loss, wrong-core
callbacks, partial acquisition/rollback, failed setup jobs, ring overflow
and pending process cleanup. Bridge tests run the actual consumer/control
code with a host worker thread and inject wait/join/delete/free failures.
See `docs/hardware-validation.md` for the remaining console acceptance gates.

The synthetic sample now runs work in `main`, a `std::thread`, native workers
and a sleeping thread. Its loops are non-inlined and use volatile local
accumulators to prevent constant folding; the shared sink is atomic.

Before claiming hardware support for this revision, test on each supported
firmware: timer allocation and rate; callback CPU mask; GUID/PUID agreement;
main/pthread visibility; sleeping-thread behavior; connect/disconnect;
reconfiguration; process exit and plugin unload with the worker waiting;
and repeated sessions. Measure overhead versus zones-only with sampling off,
and inspect dropped records and timer overruns. Run NEON-heavy application
work while profiling to check preservation of the interrupted register file.

The kernel is compiled with general-purpose registers only and both GCC
vectorizers disabled. This is a build safeguard, not a substitute for a
hardware test of the entire interrupt path.

Primary API references:

- https://docs.vitasdk.org/systimer_8h_source.html
- https://docs.vitasdk.org/debugger_8h_source.html
- https://docs.vitasdk.org/ommon_2kernel_2threadmgr_8h.html
