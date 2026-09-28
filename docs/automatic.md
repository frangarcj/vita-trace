# Link-time homebrew integration

For an executable built with VitaSDK/CMake, profiling can be enabled without
adding profiler headers or calls to its source files:

```cmake
add_subdirectory(path/to/vita-trace)
add_executable(my_homebrew main.c other_sources.c)
vita_tracy_enable(my_homebrew PMU FRAMES)
# Keep the normal vita_create_self/vita_create_vpk calls for this executable.
```

Call the helper exactly once, after creating the executable. Samples in the
vita-trace subdirectory default to off. The helper adds debug information;
keep the resulting unstripped ELF from the exact captured build on the PC.

## Modes

`vita_tracy_enable(my_homebrew FRAMES)` starts the Tracy client and marks
successful non-blackout `sceDisplaySetFrameBuf` calls. It never calls the
optional kernel ABI and does not require tracy_kernel.skprx. Existing Tracy
zones can coexist with it but are not required for frame markers.

`vita_tracy_enable(my_homebrew PMU FRAMES)` additionally attaches the kernel
bridge and starts the whole-core PMU backend. The default is cores 0..2 at
100 Hz, cycles plus six events: renamed instructions (0x68), instruction-
and data-cache stall cycles (0x60/0x61), L1D/L1I refills (0x03/0x01), and
branch mispredictions (0x10). These are not per-thread counters or a function
profile. A reported interval includes everything running on the core.

Optional numeric parameters:

```cmake
vita_tracy_enable(my_homebrew PMU FRAMES PMU_HZ 250 CORE_MASK 3)
```

`CORE_MASK` is decimal raw bits 0..3, not the shifted CreateThread mask; 3
selects cores 0 and 1. Rates are 10..1000 Hz. The fourth, system core is
excluded by default and requires an explicit mask that includes bit 3.
Timer availability, routing, overhead and PMU persistence require console
validation. A startup failure is included in Tracy AppInfo; it does not
change the original main function's arguments or return value.

For the experimental function-PC path, use this alternative instead of `PMU`:

```cmake
vita_tracy_enable(my_homebrew PC_SAMPLING FRAMES SAMPLE_HZ 100)
```

`PMU` and `PC_SAMPLING` are mutually exclusive because both own the cycle
counter. `SAMPLE_HZ` selects the nominal overflow cadence at the ARM clock
observed at startup, not a guaranteed wall-clock sampling rate. The kernel's
`VITA_TRACY_IRQ_CORE_MASK` build option selects app cores (default 7; use 1
for core-0 bring-up). The helper's `CORE_MASK` option applies only to `PMU`.
After the first successful Excpmgr registration, the experimental plugin
cannot be unloaded until reboot. Neither IRQ routing nor sampled-PC semantics
has yet been established on a console.

## Plugin loading is an explicit prerequisite

**A PMU- or PC_SAMPLING-enabled executable attaches only when the matching
kernel plugin was already loaded before the process launched, and only when
`ux0:data/vita-tracy/kernel.on` exists.** The control ABI is imported weakly.
An earlier version imported it strongly, and on the 3.60 console that hung
the process launch (2026-09-22). A weak import the loader could not bind
branches to address 0 and cannot be probed, so the marker is the user's
statement that the plugin is resident. Without the marker, Tracy AppInfo
records a skipped kernel attach and the executable runs with zones and frames
only.

The helper does not load or unload plugins, edit taiHEN configuration, or
attempt a late same-process load. Load the plugin first, create the marker,
then launch the HB. Rebuild and deploy the client and the plugin from the same
commit/ABI: the runtime ABI and size checks reject a mismatch. Use the
bring-up application's separate load and attach runs when managing the plugin
manually.

Without `PMU` or `PC_SAMPLING`, no kernel control call is made by the helper.
Adding either is a deliberate deployment choice, not a presence probe.

## What the wrappers do

`--wrap=main` initializes Tracy after crt0 and static initialization, then
forwards argc/argv and the original return code. An atexit callback performs
checked shutdown; callbacks the HB registers later run first. A wrapper for
`sceKernelExitProcess` also performs cleanup before forwarding that call.
Failed cleanup leaves the profiler alive for retry, rather than destroying
Tracy while a producer might still reference it.

The optional frame wrapper forwards the real display function first and
emits only on success with a non-null framebuffer/base. The timeline is
named **Vita display submit**: it measures API submissions, not physical
presentation, GPU execution or an exact count of distinct displayed images.
Several successful updates in one refresh period can create several marks.
Its emission mutex quiesces in-flight automatic frame marks before shutdown.

Wrappers act at link time on external references. Calls internal to a single
translation unit, dynamically loaded modules, indirect function pointers or
aggressive whole-program transformations may bypass a wrapper. Check the
linked executable, especially when using LTO or nonstandard startup code.
The helper does not inject into an already-built binary.

## Lifetime limits

Use this helper to own the profiler lifetime rather than explicitly shutting
Tracy down from the HB while automatic wrappers remain enabled. Repeated
initialization is harmless, but ownership should still be unambiguous.
The HB must stop and join its own annotated workers before ordinary exit.
The frame wrapper protects its own emissions, not arbitrary user zones.

Do not place Tracy calls in static constructors/destructors with this mode:
startup happens at main, and static destruction can happen after shutdown.
Abort, forced termination, `_Exit`, or calls that bypass the linker wrappers
do not guarantee a flushed final capture. Kernel process cleanup protects
the backend but cannot manufacture a lost network capture.

On-demand mode prevents disconnected viewers from accumulating normal Tracy
capture data. It does not stop the configured PMU timers while disconnected;
measure disconnected overhead as a separate configuration.

## Example and validation

`samples/automatic/src/main.c` is an unannotated workload: no profiler headers,
zones, initialization or teardown calls. Its CMake target enables the PMU.
Host tests execute the actual bootstrap and frame-wrapper code against fake
APIs, including unchanged arguments/results, startup errors, frame rejection
and retryable shutdown. Vita builds validate the real SDK signatures, stubs,
link order and SELF/VPK generation; they do not establish hardware behavior.

The `PC_SAMPLING` option explicitly selects the experimental interrupted-PC
source. The helper never selects it or the suspend diagnostic as a fallback.
