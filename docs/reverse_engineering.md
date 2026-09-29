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

`scePerfGetTimebaseValue` reads a 64-bit counter at offset 0x88 (re-read
until two reads of the high word agree). **Correction (2026-09-30):** this is
not a shared page independent of ScePamgr. libperf's `module_start` stores
`sceKernelPaGetIoBaseAddress()` as the base, and in 3.36 pamgr that is the
user mapping of the performance-monitor (PFM) block, whose kernel side is
`0xE50D0000`: the block that never answers a read on retail 3.60. On 3.60 the
unresolved stub stores `0xFFFFFFFF` as the base anyway. ScePerf and ScePamgr
do read the same counter, but on retail neither can, which is why the client
times against the process timer.

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

## Measured on hardware (3.60, 2026-08-21)

Corrected 2026-08-21: earlier notes from this bring-up said 3.65. The
console's *reported* `sceKernelGetSystemSwVersion()` can be spoofed and
wasn't trustworthy either way; the real number is the `sys_version` field
inside the SCE header of the console's own `os0:kd/*.skprx` files, which is
part of what the loader cryptographically verifies and can't be faked at
that level. Decrypting `sysmem.skprx`/`threadmgr.skprx`/`modulemgr.skprx`
straight off this console gives `sys_version=0x36000000000` — 3.60, not
3.65 — confirmed by cross-checking every NID this project's kernel module
imports against those same decrypted files and finding zero mismatches
against 3.60 that weren't already flagged as version-sensitive (see
`VITA_TRACY_FIRMWARE` in `kernel/CMakeLists.txt`, which must stay at its
`360` default for this console).

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

## The four imports blocking a 3.36 `pamgr` port are all cold, all init-time

Established 2026-08-21 by disassembling `pamgr` 3.36 (`os0/kd/pamgr.elf`),
still with no hardware involved. This resolves — in `pamgr`'s favour — the
question the previous section left open about porting it forward under
taiHEN.

Of `pamgr` 3.36's 55 kernel imports, three from `SceSysrootForKernel`
(`0x4CD47EEE`, `0xA47EB096`, `0xC10A193B`) and one from
`SceSyslibtraceForKernel` (`0x7CC73CDA`) don't resolve against retail 3.60's
exports. Tracing every `blx` to their import stubs (`find_callsites.py`
against `pamgr336.elf`) puts all four calls in the same stretch of
initialization code, `0x81000200`–`0x810012c0`, well away from
`_sceKernelPaAddArmTraceByKey`, `_sceKernelPaAddCounterTraceByKey` or either
timebase function:

- `0x81000200` is a multi-core barrier: it reads the executing core's ID
  (`mrc p15, 0, r3, c0, c0, 5`), waits on a per-core cookie array through a
  `SceThreadmgrForDriver` call, and only the core that clears the barrier
  falls through.
- That core then calls a `SceSblACMgrForKernel` function (`0x49509A83`,
  resolves fine on retail) as a gate, and only if it succeeds calls the
  unresolved `SceSyslibtraceForKernel` NID with a small struct (a version
  field and a stack buffer) — a one-time "register with the host tracer"
  handshake. This matches the existing note that this NID has no counterpart
  in the prototype's exports either, so the call was inert even on firmware
  where it resolves.
- Separately, `0x81001250` writes `PMCR = 0x11` (bits E and X — enable
  counting, export events to the trace bus) once per core, walking a
  one-hot core mask through `ksceKernelGetThreadCpuAffinityMask` (NID
  `0x83DC703D`) and `ksceKernelChangeThreadCpuAffinityMask` (NID
  `0x6D0733A8`) bracketing each `mcr` — both confirmed by name against
  `db/360/SceKernelThreadMgr.yml`, which is how a single core has to reach
  another core's local CP15 state: migrate the calling thread there, write
  locally, migrate back. Immediately after, it registers three of `pamgr`'s
  own functions with `SceSysrootForKernel`, one call per NID.

Disassembling what gets registered turns the three `SceSysrootForKernel`
calls from "unresolved dependency" into "harmless outbound registration":

| NID | Registers | What it does |
|---|---|---|
| `0xA47EB096` | `0x81000134` | `movw r0, #0x14d; bx lr` — the literal 333, no hardware read at all |
| `0x4CD47EEE` | `0x81000108` | The same shared-page-offset-`0x88` torn-read as `scePerfGetTimebaseValue`, returned as `{r1:r0}` |
| `0xC10A193B` | `0x810003ac` | `(selector, index) -> pointer` into one of two 16-entry internal arrays |

So `SceSysrootForKernel` is a registry `pamgr` publishes its own timebase
accessors *into* — not a dependency its trace machinery calls *out* to. The
timebase-frequency function is confirmed to be a hardcoded constant with no
underlying register; **the unit of 333 cannot be resolved by more
disassembly**, only by runtime calibration, which the client already does.

**Practical conclusion:** a 3.36 `pamgr` running under taiHEN on 3.60/3.65
needs exactly these four call sites patched to a no-op/return-0 stub. Nothing
downstream of them — ARM trace setup, counter trace setup, or either
timebase accessor — reads any state those four calls would have produced.
The PC-sampling path is now a known-shape patch, not an open question.

**One thing worth carrying into the kernel plugin regardless of the taiHEN
question:** PMCR is core-local CP15 state, and `pamgr`'s own boot code
doesn't just write it once — it walks all four cores to do it, going through
`SceThreadmgrForDriver` rather than a plain `mcr`, because a core can only
write its own PMCR. Both functions it uses to do that are ordinary,
NID-mapped `SceThreadmgrForDriver` exports with no ScePamgr involvement, and
their stub object already exists in the installed toolchain
(`libSceThreadmgrForDriver_stub.a`, already linked by `kernel/CMakeLists.txt`)
even though `psp2kern/kernel/threadmgr.h` doesn't declare them yet — a local
prototype is enough to use them today. `kernel/sampler_pamgr.c` reading
`ScePmu*Reg` MMIO frames for a core other than the one it happens to be
running on should not assume PMCR.E is set there; either reach every core
the same way `pamgr` did, or confirm the MMIO frame reflects state
independent of the local CP15 enable.

## What SceSysrootForKernel's own implementation looks like, not just its callers

Established 2026-08-21, this time reading the callee instead of inferring
from the caller. `SceSysrootForKernel` isn't a separate module — its 3.36
implementation lives inside `os0/kd/sysmem.elf` itself, in the same base
memory-manager module that also provides `SceSysmemForKernel`. Locating the
four target NIDs in 3.36's export table and disassembling each one directly
(rather than reasoning about what a caller passes them) shows the mechanism
is smaller and more specific than "a registry `pamgr` publishes into":

| NID | Address (3.36) | Body |
|---|---|---|
| `0xA47EB096` | `0x810209f0` | `str r0, [g_sysroot + 0x430]; bx lr` |
| `0x4CD47EEE` | `0x81020a18` | `str r0, [g_sysroot + 0x434]; bx lr` |
| `0xC10A193B` | `0x81020a44` | `str r0, [g_sysroot + 0x438]; bx lr` |

Each is a bare setter — store the caller's function pointer into one fixed
field of a global struct, nothing else. Each has a paired *invoker*
immediately following it in the file (`0x81020a00`, `0x81020a28`,
`0x81020a54`): load the same field, and if it's non-NULL, `blx` to it,
otherwise return 0. None of the three invokers are themselves exported under
any NID — they're private helpers, called from whatever already-named
`SceSysrootForKernel` export is the real public entry point for "get the
timebase frequency" etc., which this pass didn't need to chase down.

The fourth NID, `0x403B509E` — the one that still resolves on retail 3.60 —
is architecturally unrelated: `ldr r3, [g_sysroot]; ldr r0, [r3, #0x7c]; bx
lr`, a direct field read with no function-pointer indirection at all. It
isn't part of the same mechanism, which is presumably why it survived.

Comparing 3.36 against retail 3.60's `sysmem.elf` (`SceSysrootForKernel`
export count 155 → 143) confirms the three setter/invoker pairs are gone
from the *base module*, not merely unreachable because nothing calls them
anymore. Sony didn't just remove `pamgr`; it excised the capability those
three slots gave any module — register a timebase-frequency callback,
register a timebase-value callback, register a private-data callback — from
`sysmem.elf` itself, in the same firmware cleanup that dropped `pamgr`,
`syslibtrace`, `dbgsdio` and `sdbgsdio`. That reframes the earlier "harmless
outbound registration" framing slightly: it's not that retail ignores the
registration, it's that retail's `sysmem.elf` no longer has anywhere to put
it.

## The scheduler never touches the PMU, and `threadmgr.elf` exports raw CP15 c9 access directly

Established 2026-08-21 by disassembling `os0/kd/threadmgr.elf` on 3.36 and
3.60 — a linear capstone sweep over every executable segment, filtered for
any `mrc`/`mcr` touching CP15 c9, cross-checked against the export table so
every hit lands inside a bounded, named function. This settles the question
`kernel/pmu.c`'s comment poses directly: it does not.

**No context-switch save/restore.** The sweep found 16 `c9`-looking hits (a
few are data misdecoded as instructions — see the caveat in
[[vita-firmware-re-workflow]] — confirmed by checking the surrounding bytes
form a repeating 16-byte data table, not code). The 13 real hits land inside
exactly six small (~150–400 byte), self-contained exported functions, none
of which resemble a scheduler's bulk register save/restore block (no
`push {r4-r11}`-style wide save, no `vmrs`/`vmsr`, no CPACR touch nearby).
The one CP15 register genuinely pervasive throughout the module is
`c13, c0, 4` (TPIDRPRW, "get current thread struct pointer"), used as a
generic idiom everywhere, not specifically around these six functions. PMU
state is therefore **not** transparently per-thread — it free-runs on
whatever happens to be executing on that core, unmanaged by the scheduler,
unless something explicitly reprograms it.

**Six exported CP15-c9 accessors, one per PMU operation**, all
`SceThreadmgrForKernel`/`SceThreadmgrForDriver`, identical in both firmware
versions (NIDs and instruction sequences match 3.36 to 3.60, only addresses
shift):

| NID | Library | c9 register | Op |
|---|---|---|---|
| `0x6ECCDCBD` | ForKernel | PMSELR (`c9,c12,5`) + PMXEVTYPER (`c9,c13,1`) | select + configure an event counter |
| `0x2EC8E376` | ForKernel | PMCNTENSET/PMCNTENCLR (`c9,c12,1`/`2`) | enable/disable a counter |
| `0xD2BE5EFB` | ForKernel | PMSELR + PMXEVCNTR (`c9,c13,2`) write, PMCCNTR (`c9,c13,0`) write | set a counter's value |
| `0xCE99E69C` | ForKernel | PMSELR + PMXEVCNTR read, PMCCNTR read | get a counter's value |
| `0x5053B005` | ForDriver | **PMUSERENR** (`c9,c14,0`) | `mcr p15,0,r6,c9,c14,0` at 3.60 `0x8101335a` |
| `0x1AAFA818` | ForDriver | **PMCR** (`c9,c12,0`) | `mcr p15,0,r6,c9,c12,0` at 3.60 `0x81013436` |

The last two matter most: **`0x5053B005` writes PMUSERENR directly**, the
same register that reads back 0 on retail via `sceKernelGetPMUSERENR` (see
[[vita-tracy-hardware-facts]]). Independently re-disassembled both past the
point the sub-agent that found them stopped, to the actual `mcr`. Both
functions share an identical wrapper shape: check `TPIDRPRW` is valid (fails
outside real thread context, error `0x80027101`), check the caller isn't in
IRQ/exception mode (error `0x80028004`), and if the first argument (`r0`) is
nonzero, run a permission check against a target UID before writing;
`r0 == 0` takes a simpler self-targeting path. The value written is the raw
second argument — full caller control, no masking seen. Both are plain
`SceThreadmgrForDriver`/`ForKernel` exports pamgr itself imports (present in
its own import table, `NOT FOUND` in vita-headers by name until now) — not
ScePamgr-specific at all.

**This is the retail PMU story completed, not just pamgr's**: PMUSERENR
reads 0 on retail not because of a hardware fuse, but because nothing on
retail ever calls the function that sets it — that function used to be
reached from `pamgr`'s own boot code (§ above), and retail never runs it.
Since context-switch doesn't touch `c9` at all (previous finding), a value
written to PMUSERENR or PMCR on a core should hold for every thread that
later runs there, not just the caller. Unverified: whether `r0`'s
target-thread semantics change what "self" means for a value that's
actually core-global hardware state — plausible that it doesn't matter
(the notify-a-listener side effect at struct offset `0x70`/`0x64` looks like
it's for a debugger UI, separate from the unconditional `mcr`), but this
wants confirming on a CEX before depending on it, not just re-derived from
the disassembly.

## Confirmed: no CoreSight trace sink exists anywhere in retail firmware

Established 2026-08-21 with a broader sweep than the one that first raised
the question. All 46 retail 3.60 `os0/kd/*.elf` kernel modules were
string-scanned (`ETB`, `ETR`, `TMC`, `trace`, `coresight`, `etm`, `ptm`,
`funnel`, `tpiu`, `sink`, the CoreSight unlock key `C5ACCE55`, and more) —
none contains any string suggesting a software-reachable path from
CoreSight trace into system RAM. `crashdump.elf`, `excpmgr.elf` and
`buserror.elf` — the modules likeliest to want one for their own postmortem
purposes — were given the closest look; `crashdump.elf` and `buserror.elf`
carry essentially no MMIO strings at all, delegating everything to
`SceCpuForDriver`/`SceDebugForDriver`.

One lead looked promising and resolved negative: `excpmgr.elf` has a
`"=== Print Waypoint =="` table with entries (`Direct branch`,
`Indirect branch`, `Debug entry/exit`, `Secure Monitor`, ...) straight out
of ARM's own PTM waypoint-instruction vocabulary. Tracing its consumer
(`0x8100165c`, called from three exception handlers) shows it walks a
32-entry ring buffer inside a per-thread/exception-context struct in
ordinary kernel RAM — every access is relative to a caller-supplied
pointer, no MMIO literal anywhere near it. Sony's kernel programmers reused
ARM's own terminology to classify which instruction type caused a nested
exception entry, in software; it has nothing to do with CoreSight's trace
stream.

A device-table sweep (grepping every module for `Sce*Reg`-style names)
turned up nothing beyond what `pamgr` already names — `bootimage.elf`,
`dmacmgr.elf`, `intrmgr.elf`, `lowio.elf`, `sysmem.elf` and `syscon.elf`
each expose their own unrelated peripherals (SPI, DMA, GPIO, timers, the
PMIC). One loose thread: `sysmem.elf` names a register frame
`SceL2CacheReg`, distinct from `pamgr`'s `ScePl310Reg` — possibly the same
PL310 block under a different name, not confirmed as the same address.

**Standing conclusion survives, now on a firmware-wide search rather than
just `pamgr`'s own device list: PTM has no way to reach system RAM on this
console, and should stay ruled out as a PC-sampling source** unless
something changes this finding (a firmware version not checked, or a block
this string/table sweep can't see because it's referenced only by a raw
MMIO literal with no accompanying string).

## Open items from the design (validate on CEX or resolve via RE)

- Whether ScePamgr's ARM/counter trace carries context-switch information at
  all — unanswerable until a taiHEN-ported 3.36 `pamgr` or a scheduler hook
  actually runs. Low priority now that PTM is confirmed to have no memory
  sink anywhere in retail firmware (see below), so this path is unlikely to
  be worth pursuing regardless of the answer.
- Whether calling `SceThreadmgrForDriver` NID `0x5053B005` with `(0, 1)`
  from the kernel plugin actually sets PMUSERENR on a real console the way
  the disassembly says, and whether it's still set for threads that run on
  that core later (expected, since context-switch never touches `c9` — see
  below — but expected isn't measured).
- Whether all six programmable counters plus the cycle counter are usable
  once PMCR and PMCNTENSET are programmed for real, and what values other
  than pamgr's `0x8000003F` are safe.
  search so far only covered the modules `pamgr` itself touches.
- Firmware drift: NIDs/structs used by kernel-side RE code must be pinned per
  firmware branch (`kernel/platform/fw_360.c`, `fw_365.c`, ...).
