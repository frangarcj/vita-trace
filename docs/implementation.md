# Implementation status

The original zones/frames path has run on a PS Vita and under Vita3K (see
below). The ABI-2 bridge, explicit profiler-thread registry, timer-driven
diagnostic and ELF resolver revision are built and host-tested, not yet
validated on hardware. See `docs/sampling.md` for the precise boundaries.

## Phase status

| Phase | State | Notes |
|---|---|---|
| 0 — Bring-up | **Verified on hardware** | A retail console captured over Wi-Fi: 556 frames, 554 zones in 12 s, timings self-consistent. |
| 1 — Kernel bridge | Built, ABI-2 hardware validation pending | Explicit profiler PUID registry, event-driven drain/control, shared-ring layout checks and serialized control producers. |
| 2 — Provisional samples | Explicit diagnostic only | SceSysTimer wakes the suspend/read/resume worker. No name-based exclusions; main is no longer the bridge's control caller. These are not unbiased CPU-time samples. |
| 3 — Non-intrusive sampling | Not implemented | SceSysTimer supplies cadence but no interrupted register frame. A validated IRQ-context/PC adapter under the normal scheduler remains necessary. The PTM investigation is recorded in `docs/reverse_engineering.md`; importing pamgr alone does not establish a RAM trace sink. |
| 4 — PMU | Built, untested on hardware | Client-side ScePerf path is dead (PMUSERENR reads 0, ScePerf can't load). `kernel/pmu.c` now opens PMUSERENR and programs PMCR/PMCNTENSET/PMXEVTYPER directly via CP15, per core, using a `SceThreadmgrForDriver` NID (`0x5053B005`) reverse-engineered from `threadmgr.elf` — see `docs/reverse_engineering.md`. Loading `tracy_kernel.skprx` itself is now on-demand via taiHEN (`samples/bringup`), gated behind the same marker file, rather than a permanent `ux0:tai/config.txt` entry. |
| 5 — Callstacks | Phase A only | Samples carry a single PC. LR, stack snapshots and offline unwinding are not implemented. |
| 6 — GPU / Razor | Not started | — |
| 7 — Uninstrumented agent | Not started | `agent/` is empty. |

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

**PMU is still experimental.** The original ScePerf userland path is not
usable on retail. The kernel CP15 bring-up and cycle reader are preserved;
they are not a finished per-thread profiler. The ABI-2 timer/drain revision
does not replace that separate reader or implement its Tracy event forwarding.

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
- How many PMU counters are programmable once a kernel module opens
  PMUSERENR, and whether they survive context switches.
- What high-resolution clock is reachable without ScePerf. The process timer
  works but is microseconds; the kernel plugin is the likely answer.
- A non-intrusive sampling source. ScePamgr is absent from 3.60/3.65, so the
  choice is between porting the 3.36 module forward and writing a scheduler
  hook; see `docs/reverse_engineering.md`.

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
