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

**A trap the emulator hides.** Vita3K binds an import it does not recognise
to its own logging stub, so calling one returns cleanly and logs a line. Real
firmware does not: an unresolved weak import keeps the sixteen bytes the
toolchain emitted — `[version|flags, library NID, function NID, padding]` —
and calling it executes the NIDs as instructions. A run that passes under
Vita3K therefore proves nothing about whether optional imports are safe to
call, which is why the bridge inspects the stub header rather than trusting
a return value.

## ScePamgr was removed from retail firmware in 3.50

Established on 2026-08-21 by disassembling decrypted firmware, with no
hardware involved. This reshapes the design's central phase-3 hypothesis and
fixes a bug that would otherwise have surfaced only on a console.

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

## What ScePamgr's "ARM trace" actually programs

`_sceKernelPaAddArmTraceByKey` drives an ARM CoreSight cluster over MMIO.
The implementation walks four per-core tables of device base addresses and
unlocks each with the CoreSight software lock key:

```
    movw r1, #0xce55
    movt r1, #0xc5ac          ; r1 = 0xC5ACCE55, the CoreSight LAR key
    ldr  r3, [r5, r2]         ; per-core device base
    str.w r1, [r3, #0xfb0]    ; LAR
    ldr.w r3, [r3, #0xfb4]    ; LSR
    cmp  r3, #1               ; implemented and unlocked
```

The devices it names, all present in retail 3.36:

| Name | Device |
|---|---|
| `ScePtm0Reg`–`ScePtm3Reg` | Program Trace Macrocell, one per core |
| `SceDbg0Reg`–`SceDbg3Reg` | Per-core debug registers |
| `ScePmu0Reg`–`ScePmu3Reg` | Per-core performance monitors, memory-mapped frame |
| `SceCti0Reg`–`SceCti3Reg` | Cross Trigger Interface |
| `SceFunnelReg`, `SceTpiuReg` | Trace funnel and Trace Port Interface Unit |
| `SceItmReg` | Instrumentation Trace Macrocell |
| `ScePl310Reg` | PL310 L2 cache controller |

So "ARM trace" is genuine program-flow trace, not event counting — a
superset of PC sampling, generated by hardware at no cost to the traced
core. That is exactly what phase 3 wants.

**The catch is where the trace goes.** There is no ETB, ETR or TMC anywhere
in the device list, and no such string appears in the module. Without an
embedded trace buffer or router, a PTM stream leaves through the Funnel and
TPIU, which is an off-chip trace port needing a hardware probe. Nothing
found so far shows a path from PTM into system RAM, and
`_sceKernelPaSetupTraceBufferByKey` only records a caller-supplied
start/end pair rather than allocating or programming a sink. Until a memory
sink is demonstrated, PTM should be treated as unusable for an on-device
profiler, however attractive it looks.

For contrast, the other trace sources are ordinary:
`_sceKernelPaAddCounterTraceByKey` does a PMCR read-modify-write through
CP15 after checking the event id is under 32 — event counting with no
addresses attached — and `_sceKernelPaAddBusTraceByKey` only sets a flag bit
in a software descriptor.

Two leads worth keeping, both reachable by our own kernel plugin over MMIO
without ScePamgr existing at all: the per-core `ScePmu*Reg` frames would let
the kernel read another core's counters without executing on it, and
`ScePl310Reg` exposes L2 cache event counters that the per-core PMU does not
cover.

`syslibtrace`, which ScePamgr imports one function from, is a devkit module
that ships on no retail firmware. It exports eight functions under
`SceSyslibtraceForKernel`, imports heavily from ModuleMgr, and installs
callback tables — a module and NID-symbol tracer for host tooling, unrelated
to sampling. The single NID ScePamgr imports from it does not even match any
NID that prototype snapshot exports, so it is inert on retail either way.

## The PMU survives on retail, through CP15 rather than ScePamgr

Losing ScePamgr does not take the ARM performance counters with it, which
was the first worry once the module turned out to be gone. Disassembling
retail 3.60's `libperf` shows every `scePerfArmPmon*` entry point doing its
work in userland:

| Call | CP15 register |
|---|---|
| `scePerfArmPmonReset` | PMCR, `c9,c12,0` (read-modify-write) |
| `scePerfArmPmonStart` | PMCNTENSET, `c9,c12,1`, written `0x8000003F` |
| `scePerfArmPmonStop` | PMCNTENCLR, `c9,c12,2` |
| `scePerfArmPmonGetCounterValue` | PMSELR `c9,c12,5`, then PMXEVCNTR `c9,c13,2` or PMCCNTR `c9,c13,0` |
| `scePerfArmPmonSoftwareIncrement` | PMSWINC, `c9,c12,4` |

The catch is the thread argument. `scePerfArmPmonStart` branches on it:

```
    blx  <sceKernelGetPMUSERENR>
    cbz  r0, <error 0x80580005>      ; userland PMU access disabled
    cbz  r4, <direct CP15 path>      ; thid == 0
    mov  r0, r4
    blx  <ScePamgr sceKernelPerfArmPmonStart>   ; thid != 0
```

Naming another thread goes through ScePamgr and therefore cannot work on
3.60 or later. Passing `SCE_PERF_ARM_PMON_THREAD_ID_SELF`, which is 0, takes
the CP15 path and returns 0. The client only ever passes 0.

Both paths are gated on `sceKernelGetPMUSERENR` returning non-zero — the
ARM register controlling userland access to the monitors. The client now
checks it before programming anything, so a console with userland PMU access
disabled reports that instead of counters that quietly refuse. VitaSDK ships
the stub for it but declares it in no header.

The write of `0x8000003F` to PMCNTENSET also answers, tentatively, how many
counters exist: the cycle counter plus six programmable ones.

## Measured on hardware (3.65, 2026-08-21)

The bring-up sample ran to completion on a retail console and settled three
questions the firmware reading could only frame.

| Question | Answer |
|---|---|
| Can an application load ScePerf? | **No.** `sceSysmoduleLoadModule(SCE_SYSMODULE_PERF)` returns `0x805A1000`, `SCE_SYSMODULE_ERROR_INVALID_VALUE` — the id is rejected outright, not merely unavailable. |
| Which clock does the client end up on? | The process timer, at microsecond resolution. |
| Is the PMU reachable from userland? | **No.** `sceKernelGetPMUSERENR()` reads 0, so every CP15 monitor access is barred. |

The whole performance stack is gone from retail together: ScePamgr removed in
3.50, SceDTrace and syslibtrace absent, and ScePerf itself unloadable. It was
devkit equipment.

Three consequences for the design:

- **The clock the design specified does not exist here.** Section 6 treats
  `scePerfGetTimebaseValue` as confirmed; on a retail console it cannot be
  called at all. The client runs on `sceKernelGetProcessTimeWide` instead,
  which is a microsecond counter — 333 CPU cycles per tick. Frame and
  function-level work is measurable; anything shorter collapses.
- **Phase 4 cannot work as written.** The client-side PMU is barred by
  PMUSERENR, and ScePerf, which is how the design reaches the counters, is
  not loadable anyway.
- **The kernel backend stops being optional.** It was scoped as a bonus for
  sampling and metadata. It is now the only route to either a
  high-resolution clock or the PMU, because a kernel module can write
  PMUSERENR and can reach the per-core `ScePmu*Reg` frames directly.

## Open items from the design (validate on CEX or resolve via RE)

- PC sampling without suspending the target thread. ScePamgr is absent from
  3.60/3.65, so this is now a choice between porting the 3.36 module forward
  under taiHEN and hooking the scheduler; neither has been attempted.
- Whether ScePamgr's ARM/counter trace carries context-switch information at
  all — unanswerable until one of those two paths works.
- Whether retail firmware enables userland PMU access at all, i.e. what
  `sceKernelGetPMUSERENR()` returns on a console. A linear disassembly sweep
  of the 3.60 kernel for writes to PMUSERENR produced only false positives
  from decoding data as code, so this is not answerable that way; the client
  reads it at startup instead.
- PMU counter semantics across context switches, and whether all six
  programmable counters plus the cycle counter are really usable.
- The unit of the 333 the timebase frequency call returns.
- Firmware drift: NIDs/structs used by kernel-side RE code must be pinned per
  firmware branch (`kernel/platform/fw_360.c`, `fw_365.c`, ...).
