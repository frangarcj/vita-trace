# Firmware compatibility

Target is CEX 3.60 and 3.65. Nothing has been verified on either yet.

## Firmware-sensitive imports

| Symbol | Library | Selected by | Note |
|---|---|---|---|
| `ksceKernelGetModuleList` | `SceModulemgrForKernel` | `VITA_TRACY_FIRMWARE` | NIDs differ between branches; VitaSDK ships a separate `_363` stub. |
| `ksceKernelGetModuleInfo` | `SceModulemgrForKernel` | `VITA_TRACY_FIRMWARE` | Same. |
| `ksceKernelGetModuleInfoForDebugger` | `SceModulemgrForKernel` | `VITA_TRACY_FIRMWARE` | Not used yet; listed because the snapshot path may move to it. |

Build for 3.63 and later with:

```sh
cmake -B build -DVITA_TRACY_FIRMWARE=363
```

The default is `360`, which uses the unsuffixed stub.

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

Reverse-engineered or firmware-specific behaviour belongs behind a
`kernel/platform/fw_*.c` boundary rather than inline, so a second firmware
does not mean editing the sampler. That boundary does not exist yet because
nothing in the tree currently needs it: every symbol used so far is a
VitaSDK export. It should be introduced with the first symbol that is not.
