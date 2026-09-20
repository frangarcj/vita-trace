# Firmware compatibility

The original client zones/frames have been captured on hardware. The revised
kernel backend is built and host-tested, not yet console-validated. Its known
export variants are 3.60 and 3.63+, including the intended 3.65 target; resolving
their names does not establish correctness of PMUIRQ behavior on those versions.

## Firmware-sensitive imports

`kernel/firmware_exports.c` resolves the following complete table before module
startup publishes callbacks. `kernel/exports_taihen.c` supplies the lookup via
`taihenModuleUtils`; taiHEN must be loaded. This follows kubridge's runtime
resolution pattern but uses exact known library/function pairs, checks both
the status and the address, and never publishes a partial table.

| Symbol | Library NID 3.60 / 3.63+ | Function NID 3.60 / 3.63+ |
|---|---|---|
| `ksceKernelGetModuleList` | `C445FA63` / `92C9FFC2` | `97CF7B4E` / `B72C75A4` |
| `ksceKernelGetModuleInfo` | `C445FA63` / `92C9FFC2` | `D269F915` / `DAA90093` |
| `ksceKernelGetThreadContextInfo` | `A8CA0EFD` / `7F8593BA` | `D8B9AC8D` / `6C1F092F` |
| `ksceExcpmgrRegisterHandler` | `4CA0FDD5` / `1496A5B5` | `03499636` / `00063675` |

These values were checked against VitaSDK's installed stub ELF sections. The
exception function pair also matches the pinned kubridge source in
`docs/kubridge-review.md`. Unknown/absent exports reject startup before kernel
resources or callbacks are created. IRQ readers only call a resolved immutable
pointer; they never search export tables. There is no wildcard or offset guess.

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
