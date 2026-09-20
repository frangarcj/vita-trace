# Sampling and transport (ABI 2)

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

// Only after the matching ABI-2 plugin is known to be loaded.
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

The existing experimental PMU reader in `kernel/pmu.c` is separate. Its
10 ms delay loop, per-core expansion, coherent snapshots and PMU-to-Tracy
event forwarding have not been replaced by this revision.

## Compatibility and lifetime

Rebuild the client, kernel plugin and sample applications together. The
shared/control ABI is now version 2 and the exported library version is 25.
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
