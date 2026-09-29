# Firmware compatibility

The original client zones/frames have been captured on hardware. The revised
kernel backend is built and host-tested, not yet console-validated. Its known
export variants are 3.60 and 3.63+, including the intended 3.65 target; resolving
their names does not establish correctness of PMUIRQ behavior on those versions.

## Firmware-sensitive imports

`kernel/firmware_exports.c` resolves the following complete table on the first
`vitaTracyRegister` syscall, before any callback is published.
`kernel/exports_taihen.c` supplies the lookup via `taihenModuleUtils`; taiHEN
must be loaded. The lookup must not run from `module_start`: on the 3.60
console (2026-09-22) a `module_get_export_func` call issued from the
`module_start` of a module loaded at runtime with `taiLoadStartKernelModule`
never returned, one core stayed at 100%, the loading process could not be
killed and only a reboot recovered the console. kubridge makes the same call
from `module_start`, but as a boot-time plugin.

The module names passed to the lookup are the kernel module names from the
NID database, not the library names: `SceKernelModulemgr`,
`SceKernelThreadMgr` and `SceExcpmgr`. The first two were wrong
(`SceModulemgr`, `SceThreadmgr`) until 2026-09-22; every lookup returned
nothing and attach failed with `VITA_TRACY_ERROR_UNSUPPORTED`. This follows kubridge's runtime
resolution pattern but uses exact known library/function pairs, checks both
the status and the address, and never publishes a partial table.

| Symbol | Library NID 3.60 / 3.63+ | Function NID 3.60 / 3.63+ |
|---|---|---|
| `ksceKernelGetModuleList` | `C445FA63` / `92C9FFC2` | `97CF7B4E` / `B72C75A4` |
| `ksceKernelGetModuleInfo` | `C445FA63` / `92C9FFC2` | `D269F915` / `DAA90093` |
| `ksceKernelGetThreadContextInfo` | `A8CA0EFD` / `7F8593BA` | `D8B9AC8D` / `6C1F092F` |
| `ksceExcpmgrRegisterHandler` | `4CA0FDD5` / `1496A5B5` | `03499636` / `00063675` |

These values were checked against VitaSDK's installed stub ELF sections and
resolved successfully on the 3.60 console on 2026-09-22. The
exception function pair also matches the pinned kubridge source in
`docs/kubridge-review.md`. Unknown/absent exports reject the register call before
kernel resources or callbacks are created; module load itself succeeds. IRQ readers only call a resolved immutable
pointer; they never search export tables. There is no wildcard or offset guess.

The per-thread PMU context programming (`kernel/pmu_thread_ctx.c`) and the
scheduler hooks (`kernel/sched_hooks.c`) resolve a second table on first use.
These exports are not named in vita-headers; the names below come from
github.com/bythos14/libperf. On 3.63+ the `SceThreadmgrForKernel` functions
move to library `7F8593BA` with new NIDs. Those were taken from Bythos's
libperf where it has them, and otherwise by matching the 3.60 code against
the 3.63 and 3.65 dumps (the matcher reproduces every NID already known).
None of the 3.63 values has run on a console.

| Symbol | Library NID 3.60 / 3.63+ | Function NID 3.60 / 3.63+ | Established use |
|---|---|---|---|
| set process PMCR | `E2C40624` (ForDriver, both) | `1AAFA818` | `(pid, pmcr)`: writes +0x64 of every thread of the process |
| set thread counter | `A8CA0EFD` / `7F8593BA` | `D2BE5EFB` / `7B3368F1` | `(tid, 0x1F or 0-5, value)`: PMCCNTR / event counter in the saved context |
| get thread counter (diagnostics) | `A8CA0EFD` / `7F8593BA` | `CE99E69C` / `170F69D6` (code match, 98 %) | `(tid, counter, *out)` |
| set thread event | `A8CA0EFD` / `7F8593BA` | `6ECCDCBD` / `FFB9CD24` | `(tid, 0-5, type)`: +0x78+8i |
| set / clear enable counter | `A8CA0EFD` / `7F8593BA` | `72E5DA4E` / `7F831213`, `43D13895` / `1D2A6815` | `(tid, mask)`: +0xF0, cross-core call if the thread runs elsewhere. `2EC8E376` is a stub returning `0x80020002` |
| set on-CPU / off-CPU hook | `A8CA0EFD` / `7F8593BA` | `15AAB4F9` / `A3975A5A`, `DBE2EE32` / `F885ECCA` | single global slot; the 3.63 pair writes the same data offsets (+0x10, +0x08) |
| set process default PMCR | `746EC971` (ForDriver, both) | `61B9B6FA` | `(pid, pmcr)`: inherited by new threads, but returns `0x80029008` unless DIPSW 0xE4 is set |

The kernel also registers `SCE_EXCP_SVC` (kind 2) at priority 0 next to the
IRQ node. The devkit `pamgr` registered GIC SPI 244 for the PMU; on retail it
delivers nothing and is left opt-in.

The kernel no longer links these revision-specific `ForKernel` stub libraries.
One build contains both known lookup alternatives:

```sh
cmake -B build -DVITA_TRACY_FIRMWARE=auto
```

The default is `auto`. Existing `360` and `363` option values remain accepted
for build-script compatibility but do not change the resulting lookup policy.
Test both actual firmware targets; a matching lookup is not a hardware test.

Everything else the kernel plugin imports — ThreadMgr debugger calls,
`SceSysmemForDriver`, `SceProcEventForDriver`, `SceCpuForDriver`,
`SceSysclibForDriver` — comes from stubs VitaSDK does not version by
firmware.

## Values that are not from a header

These are compiled in from documentation notes rather than measured, and
each needs confirming before a release claims support for a firmware.

| Value | Where | Meaning |
|---|---|---|
| `0x1002` | `kernel/sampler_debug_fallback.c` | Suspend status passed to `ksceKernelDebugSuspendThread`. The valid mask is documented as `0xF7F03`. |
| `0x0002` | `kernel/sampler_debug_fallback.c` | Resume status passed to `ksceKernelDebugResumeThread`. |
| `permission=3` | `kernel/service.c` | Read/write mapping for `ksceKernelProcUserMap`. |
| `flags1=0x7FFFFFFF`, `flags2=1` | `kernel/module_snapshot.c` | Documented argument values for `ksceKernelGetModuleList`. |

## Policy

Keep firmware-specific exports in `kernel/firmware_exports.c`, with typed
wrappers and failure-injection tests, rather than teaching the sampler about
new NIDs. Keep raw exception-machine-state assumptions in the assembly boundary
and its instruction-level tests. Neither boundary licenses guessing offsets
or claiming support for an untested firmware version.
