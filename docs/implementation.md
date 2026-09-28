# Implementation status

The original zones/frames path has run on a PS Vita and under Vita3K (see
below). The ABI-4 bridge, per-core timer-IRQ PMU capture, experimental
PMU-overflow PC sampling, explicit profiler
thread registry, timer-driven diagnostic, automatic link-time integration
and ELF resolver are built and host-tested, not yet validated on hardware.
See `docs/sampling.md`, `docs/automatic.md` and `docs/hardware-validation.md`.

## Phase status

| Phase | State | Notes |
|---|---|---|
| 0 — Bring-up | **Verified on hardware** | A retail console captured over Wi-Fi: 556 frames, 554 zones in 12 s, timings self-consistent. |
| 1 — Kernel bridge | Built, ABI-4 hardware validation pending | Explicit profiler PUID registry, event-driven drain/control, separate PMU rings, retryable resource cleanup and serialized control operations. |
| 2 — Provisional samples | Explicit diagnostic only | SceSysTimer wakes the suspend/read/resume worker. No name-based exclusions; main is no longer the bridge's control caller. These are not unbiased CPU-time samples. |
| 3 — Non-intrusive sampling | **Verified on hardware (2026-09-28)** | PMCCNTR overflow in each target thread's saved PMU context drives priority-zero raw IRQ and SVC nodes. A capture of the irq_sampling workload on cores 0..2 attributes every sample to the right function and thread. Short bursts that end in a syscall are under-sampled. First successful handler registration pins the plugin resident until reboot. See `docs/pmu.md` and `docs/hardware-validation.md`. |
| 4 — PMU | Built and host-tested, no new hardware run | `kernel/pmu_session.c` reads cycles/events from per-core system-timer callbacks. Short pinned jobs acquire/restore registers; no resident polling thread or PMUSERENR writes. Timestamped whole-core deltas reach Tracy. |
| 5 — Callstacks | Phase A only | Both diagnostic and IRQ records carry PC/SP/LR, but Tracy currently receives only one PC. Stack snapshots and offline unwinding are not implemented. |
| 6 — GPU / Razor | Not started | — |
| 7 — Uninstrumented agent | Not started | `agent/` is empty. |

Link-time integration is available separately: `vita_tracy_enable(target
PMU FRAMES)` wraps startup/exit and optional display submissions without
source edits. `PC_SAMPLING FRAMES SAMPLE_HZ 100` is the explicit experimental
alternative, not an additional option to combine with PMU. Both still require
rebuilding/relinking the HB; neither completes hardware validation or the
injection agent.

The client uses statically initialized native pthread locks for lifetime,
worker registration, control and automatic-frame emission. The guard refuses
to proceed unlocked when a pthread operation fails. IRQ callback admission is
tracked per core; stop retains shared state while a callback is in flight.
Backend cleanup remains reachable even after a failed start has left the
logical sampling state STOPPED. Host tests cover these cases and an alternate
kernel build selecting only core 0.

The kubridge comparison exposed the missing raw exception-entry contract in
the old branch-only stub. Five instruction-level ARM/Thumb tests now execute
the replacement entry, in addition to the C-level tests. Four firmware-sensitive
exports are resolved via taiHEN on the first register syscall (not in `module_start`,
which hangs the 3.60 console); IRQ paths never perform lookups.
Sparse module segment ordinals are preserved for symbolication.

## Decisions that departed from the design

**TracySocket did not need porting.** The design assumed Tracy's sockets
would have to be rewritten against SceNet. VitaSDK's newlib already provides
a BSD socket layer backed by SceNet — `socket`, `bind`, `poll` via
`sceNetEpoll*`, `getaddrinfo` via `sceNetResolver*` — so Tracy's
`TracySocket.cpp` compiles and links unchanged. The same layer initializes
SceNet lazily on the first `socket()` call and tolerates an application that
already called `sceNetInit`, which also settles the network-ownership
question the design raised.

**Tracy platform patches.** Tracy already has platform hooks
for the allocator, thread ids, user info and safe copying, all used here.
It has none for the clock, and its fallback resolves to
`std::chrono::high_resolution_clock`, which on VitaSDK is the non-monotonic
wall clock behind `sceRtcGetTime_t`. `patches/tracy/` adds a
`TRACY_PLATFORM_GET_TIME` hook alongside the existing
`TRACY_PLATFORM_HEADER` mechanism, and the client feeds it the ScePerf
timebase. CMake applies the patch idempotently at configure time.
A second patch now adds POSIX worker lifecycle hooks, registering actual
profiler thread IDs rather than excluding every application `pthread`.

**PMU collection now stays in kernel.** The former polling/PMUSERENR
experiment is preserved in Git history, not the active backend. The current
timer callbacks read whole-core counters through `kernel/pmu_arm.c` and the
testable ownership/delta logic in `common/pmu_core.c`. It refuses an occupied
PMU and avoids restoring over another owner's changed configuration. This
does not virtualize counters per thread, prove hardware routing, or prevent
all interference by non-cooperating plugins.

**Failure does not imply cancellation.** The client keeps command payloads
in private storage until completion, even after an unsuccessful wait. Failed
joins/deletions/unmaps retain resource handles for retry. Process-exit
requests are atomically tied to the target PID; control calls serialize
without blocking on a reentrant process callback. These code paths are
tested with fake platform APIs and real host threads, not a console kernel.

**The kernel acknowledges the mapping.** The acknowledgement validates a
completed registration, not module residency. An unresolved weak import is
not safe to call on Vita; callers must establish that the matching kernel
plugin is loaded before attachment. A missing initial clock-sync record is
an error instead of silently choosing a zero clock reference.

## What still needs hardware

- Whether a session holds for the 30+ minutes the design asks for. Twelve
  seconds over Wi-Fi is proven; a long capture is not.
- The two suspend/resume status values in
  `kernel/sampler_debug_fallback.c`, which come from documentation notes
  rather than measurement.
- Whether ScePerf and the kernel timebase drift apart over a long session,
  and how large the clock-sync error actually is.
- Timer availability and per-core IRQ routing; whether PMU configuration
  survives helper-thread exit/context switches, and its actual overhead.
- What high-resolution clock is reachable without ScePerf. The process timer
  works but is microseconds; the kernel plugin is the likely answer.
- The IRQ sampler under a real application (a long session, many threads,
  GPU load) and its overhead. The mechanism itself was verified on
  2026-09-28.

## What the hardware run showed

`samples/bringup` on a retail 3.60 console (confirmed from the console's own
decrypted kernel modules — earlier notes said 3.65, which was wrong; see
`docs/reverse_engineering.md`), captured from a PC over Wi-Fi
with `tracy-capture` built from the pinned commit:

```
Timer resolution: 1 us
Frames: 556   Zones: 554   Time span: 12.03 s

Work  main.cpp:193  553 calls  mean 5.560 ms  min 5.421 ms  max 5.884 ms
```

The numbers agree with each other: 553 zones at 5.56 ms is 3.08 s of 12.03 s,
matching the 10.14% the exporter reports, and 5.56 ms of work plus a 16 ms
delay gives 21.6 ms a frame, matching 556 frames in 12.03 s. Every duration
is a whole number of microseconds, which is the clock showing through.

That closes phase 0 on hardware: the pinned viewer connects over Wi-Fi, the
SceNet path carries the protocol, and zones and frames arrive correctly
timed.

## What the emulator run showed

Running `samples/zones` under Vita3K and capturing with `tracy-capture` from
the pinned Tracy commit produced a real trace: 293 frames and 870 zones over
10.1 s, with source file and line intact.

The numbers are consistent with the workload rather than merely present.
`HotWork` runs ten times the loop iterations of `ColdWork`, and the capture
measured a 9.4:1 ratio between them (14.30 ms against 1.52 ms mean).
`Frame` came out at 15.88 ms against the 15.82 ms its two children sum to,
so nesting and the clock agree.

That exercises the platform header, the allocator and thread-id hooks, the
ScePerf timebase patch, the SceNet socket path and the protocol handshake.
It says nothing about the kernel backend, and the emulator supplies its own
ScePerf, so the timebase behaviour it demonstrates is not the hardware's.
Reported timer resolution was 1.37 us, which is emulated call overhead, not
the counter's real granularity.

## Overhead

Not measured. The design's benchmark matrix — baseline, compiled out, zones
without a viewer, connected, ring active, sampling at several rates, PMU on,
stack snapshots — needs hardware, and no number here should be quoted until
it exists.
