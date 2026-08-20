# Reverse engineering notes

## Vita3K as a partial validation target

Checked against a local Vita3K checkout (`vita3k/modules/...`) on 2026-08-20 to
see which parts of this design could be exercised against the emulator
instead of real hardware.

| Area | Verdict | Evidence | Note |
|---|---|---|---|
| ScePerf timebase + ARM PMON | STUBBED | `modules/ScePerf/ScePerf.cpp` | Every listed function is `UNIMPLEMENTED()`, returns 0. |
| ScePamgr (trace buffers, ARM/counter/GPU trace) | STUBBED | `modules/ScePamgr/ScePamgr.cpp` | NIDs exist, bodies are `UNIMPLEMENTED()`. |
| `ksceKernelGetThreadContextInfo`, `ksceKernelDebugSuspendThread/ResumeThread` | MISSING | repo-wide grep, zero hits | Not even present as NID entries. |
| `ksceKernelGetThreadCpuRegisters/GetThreadIdList/GetThreadInfo` (kernel) | STUBBED | `modules/SceKernelThreadMgr/SceThreadmgrForDriver.cpp:159-178` | Scheduler itself is real preemptive (one host OS thread per guest thread), but nothing exposes its state through these calls. |
| `sceKernelAllocMemBlock`/`GetMemBlockBase` (user) | IMPLEMENTED | `modules/SceSysmem/SceSysmem.cpp` | Real logic. |
| `ksceKernelProcUserMap`/`ksceKernelUserMap` | STUBBED | `modules/SceSysmem/SceSysmemForDriver.cpp:497,550` | Source comment: "In the emulator there is no user/kernel UID separation" — flat address space. |
| `ksceKernelUserUnmap` | MISSING | — | Not present. |
| ModuleMgr kernel (`ksceKernelGetModuleList/GetModuleInfoForDebugger/GetModuleInfoMinByAddr/GetModulePath`) | MISSING | `nids/include/nids/nids.inc` | No NID entries at all. Userland `sceKernelGetModuleInfo`/`GetModuleList` are implemented for real. |
| ProcEvent handlers | STUBBED (fake) | `modules/SceSysmem/SceProcEventForDriver.cpp:59-69` | `create`/`start` fire once synchronously at registration; `exit`/`kill`/`stop` never fire. |
| Kernel plugin loading (.skprx-style) | PARTIAL | `modules/taiHEN/taiHEN.cpp` | Vita3K reimplements taiHEN (`taiLoadKernelModule`, hooking, injection) and can load/run/hook such code, but with no real ring0/MMU separation — not equivalent to a genuine kernel context. |
| SceNet sockets | IMPLEMENTED | `modules/SceNet/SceNet.cpp`, `net/src/posixsocket.cpp` | Backed by real host BSD sockets. |

**Conclusion:** every kernel-side building block this project depends on
(`tracy_kernel.skprx`'s shared-ring mapping, process/module/thread metadata,
the provisional suspend/read/resume sampler, and all of ScePamgr) is either
stubbed or entirely absent in this Vita3K checkout, so none of that can be
functionally validated there — build success against `psp2kern` headers is
the only signal available until real CEX hardware is on hand. SceNet is a
genuine exception: because it's backed by real sockets, `libtracy_vita.a`'s
userland pipeline (Tracy handshake, zones, frames) is a plausible later
integration-test target under Vita3K, independent of the kernel backend.

This does not change any status in the matrices below — it is a note on
*where* validation can happen, not a substitute for the CEX/RE items
themselves.

## ScePamgr was removed from retail firmware in 3.50

Established on 2026-08-21 by disassembling decrypted firmware, with no
hardware involved. This closes the design's central phase-3 hypothesis.

`scePerfGetTimebaseFrequency` is not a function in its own right. In
`vs0/sys/external/libperf`, on both 3.60 and the 3.74 module Vita3K bundles,
it is two instructions:

```
scePerfGetTimebaseFrequency:
    push {r3, lr}
    blx  <ScePamgr import stub>      ; sceKernelPaGetTimebaseFrequency, NID 0xCAEE6AF2
    pop  {r3, pc}
```

Every retail 3.60 module was scanned for a provider of that library —
`os0/kd` (46), `os0/kd/bootimage` (56) and `vs0/sys/external` (141), 299
files, every one parsed successfully — and **nothing exports `ScePamgr`
(0xAB606F3F) or `ScePamgrForDriver` (0xA41B0AAF)**.

It was not always so. `os0/kd/pamgr.skprx` shipped on retail firmware up to
and including 3.36 and disappeared in 3.50, which is also where `dbgsdio` and
`sdbgsdio` went: exactly three modules, all debug facilities, dropped in one
release. Checked across the decrypted archive from 3.01 through 3.73 — every
version up to 3.36 carries 49 kernel modules including `pamgr`, every version
from 3.50 carries 46 without it.

Two consequences:

- **The phase-3 spike cannot run as written, but the path is not closed.**
  Anexo B's `pamgr-smoke`, `pamgr-arm` and `pamgr-counter` experiments have
  nothing to call on a 3.60/3.65 console. Carrying the 3.36 module forward
  under taiHEN is a real option rather than a wild one: of the 55 kernel
  functions `pamgr` from 3.36 imports, **51 still resolve against retail
  3.60's own exports**. The four that do not are three `SceSysrootForKernel`
  NIDs (0x4CD47EEE, 0xA47EB096, 0xC10A193B) and one from
  `SceSyslibtraceForKernel` (0x7CC73CDA), whose provider is itself gone from
  retail. Import resolution is only the first hurdle — a module from an older
  branch can still depend on kernel structure layouts that moved — but the
  gap is small enough to be worth measuring before falling back to writing a
  scheduler hook.
- **`scePerfGetTimebaseFrequency` returns garbage on retail.** The import is
  unresolvable, and the compiled-in stub is `mvn r0, #0; bx lr`, so the call
  yields 0xFFFFFFFF. Dividing timestamps by that collapses the whole
  timeline to zero — a profiler that looks like it is working and reports
  nothing. The client therefore rejects implausible frequencies and measures
  the timebase against `sceKernelGetProcessTimeWide` instead.

`scePerfGetTimebaseValue` is unaffected: it reads a 64-bit counter straight
out of a shared page (offset 0x88, re-read until two reads of the high word
agree), with no ScePamgr involvement. `sceKernelPaGetTimebaseValue` reads the
same offset in the same structure, so the design's question of whether ScePerf
and ScePamgr share a clock domain is answered — they read the same counter.

The rate that counter advances at is still unknown. Both prototype 1.691.011
and retail 3.36 `pamgr` return `movw r0, #0x14d`, decimal 333 — so the value
is genuine and not a prototype artefact — with no unit stated anywhere; 333 Hz
would be far too coarse for the counter it describes, and 333 MHz matches the
Vita's nominal CPU clock, but that is a hypothesis. Runtime calibration
sidesteps it, and a bring-up run on hardware will report the measured value.

## Open items from the design (validate on CEX or resolve via RE)

- PC sampling without suspending the target thread: ScePamgr ARM trace is the
  first hypothesis (Anexo B, `pamgr-arm`); real hardware is required since
  Vita3K's ScePamgr is a stub.
- Whether ScePamgr's ARM/counter trace carries context-switch information, or
  whether a ThreadMgr hook is needed instead.
- PMU counter semantics across context switches, and how many programmable
  counters are actually usable.
- Whether ScePerf and ScePamgr share a clock domain.
- Firmware drift: NIDs/structs used by kernel-side RE code must be pinned per
  firmware branch (`kernel/platform/fw_360.c`, `fw_365.c`, ...).
