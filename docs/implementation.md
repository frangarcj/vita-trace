# Implementation status

Nothing here has run on a PS Vita yet. Everything below builds with the
VitaSDK toolchain and the platform-independent logic is covered by the host
test suite, but no claim about runtime behaviour has been verified on
hardware. `docs/reverse_engineering.md` explains why the emulator cannot
stand in for that.

## Phase status

| Phase | State | Notes |
|---|---|---|
| 0 — Bring-up | Built | `libtracy_vita.a` links against a pinned Tracy, and the zones sample produces a `.vpk`. Viewer handshake unverified. |
| 1 — Kernel bridge | Built | ABI v1, per-core SPSC rings, `ksceKernelProcUserMap`, process-event cleanup, module snapshots. |
| 2 — Provisional samples | Built | Suspend/read/resume sampler, injected into Tracy as callstack samples. Offline symbolication in `tools/symbol_map.py`. |
| 3 — Non-intrusive sampling | Not started | Blocked on reverse engineering; `kernel/sampler_pamgr.c` reports unsupported. |
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

- Whether the Tracy viewer completes a handshake over Wi-Fi and holds a
  session.
- The two suspend/resume status values in
  `kernel/sampler_debug_fallback.c`, which come from documentation notes
  rather than measurement.
- Whether ScePerf and the kernel timebase drift apart over a long session,
  and how large the clock-sync error actually is.
- How many PMU counters the firmware leaves programmable, and whether they
  survive context switches.
- Everything in phase 3: the design's central open question is whether
  ScePamgr can deliver PC samples without stopping the target.

## Overhead

Not measured. The design's benchmark matrix — baseline, compiled out, zones
without a viewer, connected, ring active, sampling at several rates, PMU on,
stack snapshots — needs hardware, and no number here should be quoted until
it exists.
