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


## Hardware finding 2026-09-22: PMU state is per-thread on 3.60

The firmware saves and restores the ARM PMU registers (PMCR, PMCNTENSET,
PMSELR, PMCCNTR, PMUSERENR, PMXEVTYPER/PMXEVCNTR 0-5, the set that also
appears in `SceExcpmgrExceptionContext`) on every context switch, for
kernel and user threads. Measured on the retail console: a cycle-counter
overflow armed by a kernel thread fires at the configured rate while that
thread runs (29 in 3 s at 10 Hz) and not at all while a different thread,
pinned to the same core, runs (0 in 3 s). Reads from a systimer callback
or from another job thread see a different bank and fail the ownership
check. Any design that programs the PMU from one context and expects it to
count other threads does not work on this firmware; see
`docs/hardware-validation.md` for the runs.

### How the firmware does it, and what the plugin does about it (2026-09-23)

From the decrypted 3.60 modules (`intrmgr.skprx`, `threadmgr.skprx`,
`processmgr.skprx`): intrmgr's IRQ handler, the node our priority-0 node
chains into, disables all counters on entry, saves PMCR/PMOVSR/PMSELR/
PMCNTENSET/PMCCNTR/PMXEV* into the outgoing thread's context and restores
the incoming thread's set on exit; two `SceIntrmgrForKernel` exports do the
same for synchronous switches. PMINTENSET/CLR are never touched, so the
overflow-interrupt enable is real per-core state. The per-thread context is
reachable as `[thread_object+0x34]` (PMCR at +0x64, PMUSERENR +0x70,
PMCCNTR +0x74, PMCNTENSET +0xF0); `SceThreadmgrForDriver 0x1AAFA818(pid,
pmcr)` rewrites PMCR for every thread of a process, `SceProcessmgrForDriver
0x61B9B6FA(pid, pmcr)` sets the default for new threads, and
`SceThreadmgrForKernel 0xD2BE5EFB(tid, 0x1F, value)` writes PMCCNTR. The
counter-enable export is stubbed on retail, so `kernel/pmu_thread_ctx.c`
writes PMCNTENSET.C into the context directly after verifying the mapping
with a PMCCNTR read-back. Once a thread has run with the counter enabled,
every switch-out saves the live state, so the context stays right and the
raw node's PMCCNTR reload after each overflow is preserved per thread.
Validated on the console: 10 Hz overflow cadence from a user thread while
the kernel thread sleeps.

### The IRQ backend as validated (2026-09-23)

1. Register the raw priority-0 IRQ node and a raw priority-0 SVC node
   (`kernel/irq_entry.S`, one macro, both in `.text`). The SVC node only
   services a pending overflow; it never samples, because the firmware clears
   PMOVSR (write-one-to-clear) on the restore that follows the blocking switch
   inside a syscall, and an overflow raised in the user work window before a
   yield would otherwise be lost.
2. Per selected core, one job thread prepares and arms the cycle-counter
   overflow (PMINTENSET.C is per-core state, not virtualised) and, from core 0,
   writes every target thread's saved context: PMCR.E through
   `SceThreadmgrForDriver 0x1AAFA818`, the process default through
   `SceProcessmgrForDriver 0x61B9B6FA`, PMCCNTR = 0 - period through
   `SceThreadmgrForKernel 0xD2BE5EFB`, and PMCNTENSET.C directly into the
   block at `[thread_object+0x34]` (+0x64 PMCR, +0x70 PMUSERENR, +0x74 PMCCNTR,
   +0xF0 PMCNTENSET; a restore-only second block hangs off +0xF8) after a
   read-back check of the mapping. Programming must happen where the target
   threads are switched out; the syscall thread on another core raced with
   the running thread and sometimes left the counter at the preload.
3. The raw node reloads PMCCNTR after each overflow; the firmware saves the
   live value at every switch-out, so each thread keeps its own period. An
   overflow observed while the firmware has the counters disabled (around a
   switch) is left pending instead of being reported as ownership loss.
4. Threads created after activation still need their contexts programmed
   (not implemented). The GIC line for the PMU (SPI 244 on the devkit) does
   not deliver on retail; sampling latency is bounded by the next IRQ or
   syscall on that core (about 1 ms with the kernel tick).
