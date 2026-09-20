# Whole-core PMU capture (ABI 4)

This backend is built and host-tested, not yet tested on a physical console.
The old PMUSERENR experiment and core-0 polling loop are preserved in commit
a68c530; they are no longer the implementation used by the plugin.

## Integration

After loading the matching kernel plugin BEFORE starting the application and
successfully calling `vita_tracy_kernel_attach`, configure while stopped:

```cpp
VitaTracyPmuConfig pmu{};
pmu.size = sizeof(pmu);
pmu.abi_version = VITA_TRACY_ABI_VERSION;
pmu.core_mask = 7;            // raw CPU bits: 0, 1, 2; system core 3 is opt-in
pmu.frequency_hz = 100;       // valid explicit range: 10..1000 Hz
pmu.counter_count = 2;
pmu.counters[0] = {0, 0x68};  // instructions passing rename, NOT retired
pmu.counters[1] = {1, 0x03};  // L1D refills
int result = vita_tracy_kernel_configure_pmu(&pmu);
if (result == 0) result = vita_tracy_kernel_pmu_sample_start();
// Later: vita_tracy_kernel_pmu_sample_stop();
```

The defaults are cycles only, cores 0..2, 100 Hz. Setting `target_tid` is
rejected: these counters include everything that runs on their CPU, including
kernel work, other processes and the profiler itself. They are neither a CPU
utilization percentage nor per-thread accounting. A higher cycle rate does not
by itself prove a CPU frequency. Unsupported event encodings may read zero;
this implementation validates slot/count/encoding size, not all A9 event
semantics.

## Execution and ownership

A short-lived helper is created already pinned to each requested core. It
checks its actual CPU, saves the selected counter configuration and acquires
the PMU with local interrupts disabled. No PMUSERENR writes occur and no PMU
access is attempted from userland. An enabled counter or PMU interrupt causes
`VITA_TRACY_ERROR_BUSY`; the backend does not silently steal that PMU.

One system timer per selected core is prepared and then armed. Recording opens
only after all cores succeed. A timer callback reads the local PMU and writes
a preallocated per-core SPSC ring, then signals the bridge. There is no polling
reader thread, thread suspension, counter reset loop or IRQ allocation. The
first callback establishes a baseline rather than emitting zero work.

Setup/cleanup helpers exist only at session boundaries. Their argument and
result storage is static; failed joins/deletes preserve the handle for retry.
Partial startup rolls back the earlier cores. Stop closes recording, quiesces
ALL timers and only then restores each core's counter values, event types,
selector, enable state and PMCR. A live callback or a failed timer release
prevents unmapping/unloading. If another component changes the PMU configuration,
the core stops producing valid data and does not overwrite the new owner's
configuration on release. Identical reprogramming/counter-only writes by another
component cannot be detected reliably; exclusive ownership is still a runtime
assumption to test.

## Transport and viewer

Each ABI-4 shared block adds four PMU rings of 128 records, separate from PC
samples and the serialized control ring. Records carry CPU, sequence, timestamp,
actual interval, cycles, event encodings and deltas. Capacity overflow drops
rather than blocks and is included in the visible dropped-record count.

Unsigned subtraction handles a single 32-bit wrap. Zero/backward time and gaps
longer than one second invalidate an interval explicitly. The next callback
starts from the new baseline. The viewer plots rates using the recorded
interval and the original capture timestamp, not the bridge's drain time.
Invalid intervals set an `interval rejected` plot and do not invent zero work.
Plot names are immutable across reconfiguration and reconnection.

## Before considering this usable on hardware

Verify timer availability, raw interrupt CPU masks versus shifted thread masks,
IRQ safety of time reads/event signaling, PMCR.N and supported events, counter
persistence across kernel scheduling and power transitions, shutdown while a
callback is active, and partial-start rollback. Check cold/warm restart, process
kill, attach/detach, and another PMU owner. Measure perturbation with the backend
off, idle, and recording at several rates. Host fakes validate algorithm and
resource ordering, not any of these physical behaviors.

The source contracts are VitaSDK `psp2kern/kernel/systimer.h` and the ARMv7-A
PMU register definitions. The latter's PMSELR/PMXEVTYPER/PMXEVCNTR selector
ordering is kept behind an ISB in `kernel/pmu_arm.c`. This timer backend does
not imply that the interrupted thread's PC is available in a timer callback.
