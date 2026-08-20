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
