# Implementation status

Nothing here has run on a PS Vita yet. Everything below builds with the
VitaSDK toolchain and the platform-independent logic is covered by the host
test suite. The userland client has additionally been run end to end under
Vita3K (see below); the kernel backend cannot be, for the reasons in
`docs/reverse_engineering.md`.

## Phase status

| Phase | State | Notes |
|---|---|---|
| 0 — Bring-up | Verified under emulation | Handshake, zones, nesting and frames confirmed against a real Tracy capture from Vita3K. |
| 1 — Kernel bridge | Built | ABI v1, per-core SPSC rings, `ksceKernelProcUserMap`, process-event cleanup, module snapshots. |
| 2 — Provisional samples | Built | Suspend/read/resume sampler, injected into Tracy as callstack samples. Offline symbolication in `tools/symbol_map.py`. |
| 3 — Non-intrusive sampling | Blocked | ScePamgr does not exist on retail firmware, so the design's primary hypothesis is out; a kernel hook is the remaining option. |
| 4 — PMU | Partial | ScePerf counters and plots are implemented client-side. The privileged kernel path is not. |
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

**Tracy needed one patch, for the clock.** Tracy already has platform hooks
for the allocator, thread ids, user info and safe copying, all used here.
It has none for the clock, and its fallback resolves to
`std::chrono::high_resolution_clock`, which on VitaSDK is the non-monotonic
wall clock behind `sceRtcGetTime_t`. `patches/tracy/` adds a
`TRACY_PLATFORM_GET_TIME` hook alongside the existing
`TRACY_PLATFORM_HEADER` mechanism, and the client feeds it the ScePerf
timebase. CMake applies the patch idempotently at configure time.

**PMU lives in the client, not the kernel.** ScePerf's ARM PMON functions
are a userland library, so a kernel module cannot import them. Since the
client can drive them per thread and plot the values directly, the counters
never need to cross the ring. `vitaTracySetPmu` stays in the ABI for a
privileged path that would drive the CP15 registers, and reports unsupported
until the questions in the risk table are answered.

**The kernel acknowledges the mapping.** The design requires that an
instrumented application keep working without the plugin, so the control ABI
is imported weakly. What an unresolved weak import returns is not documented,
and a zero return would be indistinguishable from success, leaving the client
draining an empty ring and reporting a profile that is silently blank. The
kernel therefore writes an acknowledgement into the shared header during
`vitaTracyRegister`, and the client believes the call only when it sees it.

## What still needs hardware

- Whether the handshake works over Wi-Fi rather than a host loopback, and
  holds a session for the 30+ minutes the design asks for.
- The two suspend/resume status values in
  `kernel/sampler_debug_fallback.c`, which come from documentation notes
  rather than measurement.
- The rate the ScePerf timebase counter advances at. The client measures it
  at startup, and a bring-up run should report what it measured.
- Whether ScePerf and the kernel timebase drift apart over a long session,
  and how large the clock-sync error actually is.
- How many PMU counters the firmware leaves programmable, and whether they
  survive context switches.
- A non-intrusive sampling source. ScePamgr is ruled out (see
  `docs/reverse_engineering.md`), which leaves a kernel hook.

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
