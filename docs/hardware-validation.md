# Console validation and bisecting the profiler series

The new PMU/bridge/automatic integration was first run on the retail 3.60
console on 2026-09-22; see the session log at the end of this document. The
only earlier hardware-tested commit is `86bcdc6` (zones/frames only).
Host tests and cross-compilation do not establish firmware IRQ routing,
counter persistence, latency, safe teardown or acceptable overhead.

## Keep each capture reproducible

Record the vita-trace commit, Tracy submodule commit, VitaSDK compiler
version, firmware, CPU/bus clocks, selected cores/events/rate, application
build, plugin hash and exact unstripped ELF. Preserve the capture and bring-up
report alongside them. Avoid combining a newer HB with a previously loaded
plugin merely because their library names still match.

After building, record the exact files with the host-only manifest tool:

```sh
python3 tools/capture_manifest.py \
  --artifact elf=build-vita/samples/automatic/automatic_sample \
  --artifact vpk=build-vita/samples/automatic/automatic_sample.vpk \
  --artifact plugin=build-vita/kernel/tracy_kernel.skprx \
  --output build-vita/session-manifest.json
```

Use `--firmware` only for the firmware actually observed on the console;
omit it before hardware testing. Repeat `--note` to record workload, clocks,
core mask/rate, measured results or failure symptoms. More named files can
be supplied with `--artifact name=path`, including captures and reports.
Keep the manifest and the actual files together; hashing does not archive them.

The tool uses SHA-256 and records source/Tracy revisions, working-tree status,
tracked-diff hashes and compiler version. It rejects duplicate artifact names,
observed file changes during hashing, and overwriting an existing manifest.
Missing source/compiler metadata is explicit, never guessed. These are
snapshots at manifest creation, not proof that the supplied files were built
from that revision or compiler. Save local patches and untracked sources
separately; their original contents cannot be reconstructed from hashes.

ABI 4 requires a matching plugin/client pair. Exported library version 25
is not an exact build identifier. Build both from the tested commit and
replace the provider only while no client is running or using its imports.
The CMake PMU integration uses strong imports and requires the provider at
process launch; it does not support loading it later in the same process.
The bring-up sample retains separate load-only and later attach runs.

Use a recoverable test setup rather than putting an unverified plugin on a
permanent boot path. Host failure-injection tests do not make kernel mistakes
harmless: a console hang or restart remains possible. Save unrelated work.

## Initial core-0 IRQ build

With VitaSDK configured, build the experimental kernel for one app core first:

```sh
cmake -S . -B build-irq-core0 -DVITA_TRACY_IRQ_CORE_MASK=1
cmake --build build-irq-core0 --target tracy_kernel.skprx-self
```

Rebuild the matching HB from the same revision and verify
`sample_irq_core_mask == 1`. This option affects only the IRQ PC backend;
the timer-PMU mode keeps its separate runtime mask. Repeat later with mask 7
to cover app cores 0..2. Do not equate the nominal cycle period with measured
wall-clock cadence, especially when CPU clocks change during a run.

The experimental Excpmgr stub places writable registration words next to
executable code. The current linker therefore warns about a LOAD segment with
RWX permissions. This is a known property of this implementation, not a clean
security audit or a warning to suppress without reviewing the registration ABI.

## Acceptance matrix

Advance only after the preceding layer produces understandable results.

| Layer | Test | What must be observed |
|---|---|---|
| Client only | Enable `FRAMES`, no kernel attach, capture with the pinned Tracy viewer | The original application runs unchanged; successful display submissions are visible; failed display calls do not manufacture frames. |
| Lifecycle | Repeat init, normal return, `exit`, wrapped `sceKernelExitProcess`, repeated launches | No second clock adoption while Tracy is live; no obvious stuck worker or growing resource use. Static constructors/destructors must not emit Tracy events in automatic mode. |
| Mapping/control | Attach without PMU or PC sampling; request stats; checked detach | Clock sync and module maps arrive, commands complete, memory remains valid until confirmed detach. Reattach creates an independent session. |
| IRQ sampler registration | Explicitly request `VITA_TRACY_SAMPLING_PMU_IRQ` at 10 Hz first | Start succeeds, `sample_irq_handler_registered=1`, ARM MHz/core mask are plausible, and the console remains responsive. This first successful registration pins the plugin resident until reboot. |
| IRQ delivery | Run a CPU-bound worker pinned in turn to cores 0, 1 and 2 | `sample_irq_calls` and `sample_irq_overflows` advance on the executing core and samples arrive. If overflow state advances but no IRQ callback is observed, stop: PMUIRQ routing is not established. |
| IRQ PC semantics | Use noinline functions with known address ranges and alternating work ratios | Sample PCs land inside the currently executing function after Thumb normalization; SP/LR are plausible. Do not infer an adjustment to the saved PC from one sample. |
| IRQ thread identity | Exercise main, native workers and `std::thread`, with thread creation/destruction during capture | GUID→PUID resolution follows the right Tracy threads, ordinary pthread workers remain visible and profiler workers remain filtered. Reused GUIDs must not produce persistent stale attribution. |
| Minimal PMU | Explicit cycle-only config, core 0 only, 10 Hz | IRQs land on core 0; `pmu_wrong_cpu` stays zero; actual interval and counters are plausible under alternating busy/idle work. |
| Event PMU | Add the six configured events; compare intentionally different cache/branch workloads | Event labels and selected slots agree, changes are reproducible; renamed instructions are not labeled retired instructions. |
| Multiple cores | Select 0..2 at 100 Hz, pin application workers to known cores | Per-core activity responds to placement. Counts remain whole-core, not falsely assigned to one application thread. CPU 3 is a separate opt-in experiment. |
| Cadence/load | Test 10/100/250 Hz before higher rates; alternate busy/idle; keep clocks recorded | Measured interval distribution and losses are understood. Requested rate is not treated as a guaranteed achieved cadence. |
| Viewer lifecycle | Connect late, disconnect, reconnect during a long session | Deferred module metadata persists, queues remain bounded, losses are visible. Disconnected PMU timers remain active and have their own overhead. |
| PMU conflicts | Start with a different PMU owner present; change configuration during a run | Acquisition refuses occupied registers. Detected ownership loss clears the core's active bit, stops its reader and does not restore over the observed new configuration. Identical third-party programming cannot be detected. |
| Stop/retry | Repeated PMU start/stop and checked detach, including busy/error returns | A failed cleanup retains resources and can be retried. No new session is admitted over retained old resources. IRQ activity stops before mappings disappear. |
| Process death | Terminate a test HB while capture is active, then relaunch | Backend cleanup is coherent, no old callback cleans up a different process, and a later session can attach. A forced kill need not deliver a final network capture. |
| Plugin unload | Before any IRQ-sampler use, unload normally; after first successful IRQ-handler registration, try again only as a validation check | Before registration normal cleanup applies. After registration unload must be refused because VitaSDK exposes no Excpmgr unregister API; reboot is the supported removal path. |
| Register preservation | Run an application-side deterministic NEON/VFP workload before/during/after PMU | Checksums/results match. Absence of FP instructions in the plugin alone is not sufficient evidence about the firmware's complete interrupt path. |
| Duration/overhead | Baseline, client without viewer, client with viewer, ring only, then PMU; repeat 30+ minutes | Report overhead as measured distributions and resource deltas, with drops/errors. No performance number is claimed before these runs. |

Useful counters: `pmu_active_mask`, `pmu_last_error`, per-core
`pmu_records`, `pmu_dropped`, `pmu_gaps`, `pmu_wrong_cpu`,
`pmu_counter_errors`, and `last_cleanup_error`. General stats are independent
atomic counters, not a simultaneous cross-core snapshot. PMU records include
the actual interval and sequence. A stopped or failed reader is not zero CPU
work, and aggregate low-32-bit legacy totals are not a wall-clock estimator.
For experimental PC sampling also record per-core `sample_irq_calls`,
`sample_irq_overflows`, `sample_irq_not_target`, `sample_irq_kernel`,
`sample_irq_context_errors`, plus `sample_irq_arm_mhz`,
`sample_irq_core_mask` and `sample_irq_last_error`.

## Keep intrusive diagnostics separate

Ordinary nonzero PC sampling still returns unsupported. The two experimental
paths are always explicit: `VITA_TRACY_SAMPLING_PMU_IRQ` requests the
non-suspending PMU-overflow path, while `VITA_TRACY_SAMPLING_ALLOW_SUSPEND`
requests the provisional suspend/read/resume diagnostic. They are mutually
exclusive. The latter can perturb scheduling and locks or hang the target.
Do not use its percentages to validate the IRQ sampler or PMU.

The synthetic workload covers main, pthread, native workers and a sleeping
thread. The explicit registry must exclude only profiler workers. Samples
from the suspension diagnostic carry unknown CPU: ring 0 is transport, not
proof of execution on core 0. A resume failure stops further scans and may
require restarting the process.

## Interrupted-PC adapter: evidence required before promoting it

The installed VitaSDK declares:

- `psp2kern/kernel/systimer.h`: a timer callback receives a timer ID and opaque
  user data, not the interrupted register frame.
- `psp2kern/kernel/threadmgr/debugger.h`: `ksceKernelGetThreadContextInfo`
  supplies PID/TID in exception context; `ksceKernelGetThreadCpuRegisters`
  requires a suspended thread. The experimental backend only uses the former.
- `psp2kern/kernel/excpmgr.h`: an exception-context structure and handler
  registration exist, but this header does not provide a corresponding
  unregister API. The experimental backend therefore refuses module unload
  after registration rather than guessing at internal list manipulation.

The implementation uses PMOVSR as the source discriminator and captures its
own private frame in `kernel/irq_entry.S`, following the raw-node pattern
observed in kubridge's abort/undefined handlers. It does not assume a firmware
C callback frame. Hardware must establish that the priority-zero IRQ entry
receives raw LR_irq/SPSR, that PMUIRQ reaches it on each selected core, and that
`ksceKernelGetThreadContextInfo` agrees with a known running thread. The
backend distinguishes PL0 user mode via SPSR and resolves global TIDs outside
IRQ context; those assumptions need console evidence before becoming default.

A callback-local PC, a guessed stack offset, or unsuspended debugger-register
reads remain unacceptable substitutes. Host tests prove filtering, PMU
ownership, rollback and transport; they do not prove the firmware's IRQ
contract. Scheduler events would complement periodic sampling, not replace it
for a long-running thread.

Optional Unicorn tests execute the compiled entry with ARM and Thumb C targets,
separate user/IRQ register banks and an unaligned incoming IRQ stack. They check
restoration and tail-chaining; they do not simulate Sony's GIC dispatcher,
its private IRQ stack capacity or effects of imported kernel functions.
The firmware export table has both known 360/363 variants in one binary; test
startup on both targets rather than treating successful resolution as support.
Do not treat nonzero sample counts as sufficient PMUIRQ evidence: a pending
PMOVSR bit can be serviced by a later unrelated IRQ. Establish the actual
interrupt source and acknowledgement path without stealing the kernel's GIC
acknowledgement. Compare cadence under different unrelated interrupt loads.

## Bisect a behavior, not the whole feature set

Use an isolated clean checkout/worktree and a distinct build directory per
revision. Keep the user's working checkout and its local Tracy patches intact.
CMake applies the versioned patches to the pinned submodule; confirm the
patches for the revision being tested, not whatever a previous build left.

The earlier known hardware baseline is `86bcdc6` for zones/frames only. It
is not a known-good PMU implementation. Useful boundaries in the existing
series are `80c83d2` (portable PMU ownership/deltas), `598b0cf` (timer callback
quiescence), `f304ce4` (ABI-3 PMU rings/plots), `b3a33cb` (per-core IRQ PMU),
`3f1ec74`/`649d63e` (control and target cleanup), `954cf1d` (client lifetime),
`d27c41d` (automatic CMake integration), `38e94ef` (PMU partial failures),
and `d6786aa` (bridge payload and resource lifetime after failed waits).

For every candidate: build the matching plugin and HB, ensure no old provider
is retained from a prior launch, use the exact ELF and viewer, and repeat the
same stimulus. Older commits may not contain `automatic_sample`, the same
ABI, or the PMU APIs. Mark an unavailable/non-buildable test as skipped rather
than calling it a runtime failure. The stable bring-up/zones targets are more
useful for some regressions than a newly introduced sample.

Host tests can automate the portable portion of the bisect. A passing build
is not a passing console run, and a failure to load a deliberately mismatched
plugin/client pair is not evidence of a sampling regression.

## Recovery checkpoint after fcf17ac

The direct-checkout continuation added these independently testable boundaries:

| Commit | Change |
|---|---|
| `3c12c6f` | Statically initialized native locks for client lifetime, registry and bridge; explicit stdio include fixes the host build. |
| `fcc8ed0` | Per-core IRQ admission and retained state when stop races an active callback. |
| `20e8ada` | Backend cleanup remains reachable after failed startup, even in the logical STOPPED state. |
| `6a8dce2` | Build-selectable app-core mask; an independently compiled core-0 backend is host-tested. |
| `dad5f17` | Automatic frame emission uses the same native-lock lifetime policy. |

At this checkpoint, 190 CTest entries passed normally and with AddressSanitizer
and UndefinedBehaviorSanitizer. Leak detection was disabled for the sanitizer
run; ThreadSanitizer was not run. These tests include fake firmware APIs and
synthetic exception frames, not real IRQ recordings. Cross-builds produced
the normal Vita targets and kernel variants for 3.63+ and core 0 only. A mask
of 8 was rejected at CMake configuration. No new console validation was done.

## kubridge review checkpoint after 194c5f2

| Commit | Change |
|---|---|
| `86f321a` | Private raw IRQ frame and explicit ARM save/restore/tail-chain; five compiled-entry tests run on a Cortex-A9 instruction emulator. |
| `7c6e90c` | An IRQ on a prepared, unarmed PMU no longer fails that core; pending process cleanup suppresses new publications. |
| `f1e283a` | Four version-sensitive exports resolve via taiHEN at startup, with known 360/363 variants and all-or-nothing publication. |
| `997b30e` | Sparse segment ordinals, bounded module counts and bounded names survive kernel-to-viewer metadata transport. |

At this checkpoint 209 CTest entries passed both normally and with ASan/UBSan.
Leak detection was disabled; TSan was not run. Unicorn 2.1.4 was installed in
an isolated test environment and the five assembly cases were executed, not
skipped. The former branch-only entry fails those same raw-entry model tests.
This comparison does not establish what priority-seven firmware dispatch did;
the new priority-zero IRQ route still requires physical-console validation.

Cross-builds produced all Vita targets and the core-0-only kernel. The legacy
363 build option was also checked; it now selects the same runtime-resolution
policy as auto/360, not a distinct fixed-NID binary. The ELF has no versioned
ForKernel import sections. RWX chain storage remains, and installed import
stubs also cause the linker's missing GNU-stack-note warning. These warnings
have not been represented as solved by the entry tests.

## Console session 2026-09-22 (retail 3.60, core-0 kernel build, HEAD 8969948 + fixes)

Setup: `tracy_kernel.skprx` from `build-vita-core0` (`VITA_TRACY_IRQ_CORE_MASK=1`)
loaded on demand by the bring-up sample from `ux0:data/`, VitaCompanion for
launch/kill/reboot, reports fetched over FTP. Three consecutive kernel loads
required three reboots; the console recovered from all of them via ensō.

| Step | Result |
|---|---|
| Client only (bring-up, no kernel load) | Passes: ABI 4 client starts, report written stage by stage, zones/frames loop runs. |
| Kernel load at HEAD | **Hang.** `taiLoadStartKernelModule` never returned, core 1 at 100%, process unkillable, no crash dump, reboot required. Cause: taiHEN `module_get_export_func` called from `module_start`. Fixed by deferring resolution to the first `vitaTracyRegister`. |
| Kernel load after the fix | Passes (`0x40010139`). `taiStopUnloadKernelModule` of the stored modid now returns `SCE_KERNEL_ERROR_INVALID_UID`; reloading the same library version then fails with `OLD_LIB`. Bump the exported library version on every rebuild (now 26). |
| Attach, first attempt | `VITA_TRACY_ERROR_UNSUPPORTED` (seen as `0xBFFFFFFA`: bit 30 of the syscall return flipped again). Cause: wrong module names in the lookup (`SceModulemgr`/`SceThreadmgr`); fixed to `SceKernelModulemgr`/`SceKernelThreadMgr`. |
| Attach, second attempt | Register, ring map, module snapshot and clock sync all reach the client, then the `VitaTracyDrain` thread aborts: Tracy's `TaggedUserlandAddress` assert in `TracyMessage`/`TracyAppInfo`, because GCC sign-extends pointer-to-`uint64_t` on ARM32 and Vita user addresses start at `0x81000000`. Fixed by `patches/tracy/0003-tagged-address-zero-extend-32bit.patch`. |
| Attach, third attempt | Passes: attach 0, `configure_pmu` 0, `PmuSampleStart` 0, stats returned, no crash dump. |
| Timer PMU readers (cores 0..2, 100 Hz, six events) | **Fail on the first tick on every core**: `pmu_counter_errors` = 1 per core, `pmu_active_mask` = 0, last error `-3` (ownership lost), `pmu_wrong_cpu` = 0. Acquisition itself succeeded (PMCNTENSET and PMINTENSET were 0 at boot). The bank programmed by the per-core job thread is not what the systimer callback sees on the same core: consistent with the 2026-08-21 PMUSERENR observation, the firmware appears to save and restore PMU state per thread (ScePerf's per-thread counters). Whole-core counting from a foreign thread context therefore does not work as built. Not yet isolated register by register. |
| Process death with an active PMU session | **Hang.** `destroy` (app kill) never completes: the eboot stays locked, SceShell stops answering VitaCompanion, reboot needed. Suspect the proc-event kill callback running `vita_tracy_detach` (timer free plus per-core release jobs with `ksceKernelWaitThreadEnd`) in the kill path. Not yet isolated. |
| IRQ sampler registration (bring-up, `PMU_IRQ|IRQ_REGISTER_ONLY`, 10 Hz, core-0 kernel v33) | **Passes.** `ksceExcpmgrRegisterHandler(SCE_EXCP_IRQ, 0, node)` returns 0 and links `next=0x008DC138`; after 2 s the raw node has counted 2079 IRQs on core 0 and tail-chained every one, console fully responsive, stop returns 0. Two things had to change first: the node and entry now live in `.text` like kubridge's (registering a node in the RWX data segment rebooted the console), and every syscall pins its caller to the core it entered on. The release job reports ownership loss (-3) because a fresh job thread on the same core does not see the PMCR the prepare job wrote: further evidence of per-thread PMU state. |
| IRQ overflow delivery (`PMU_IRQ|IRQ_COUNT_ONLY`, single-thread probe, kernel v34) | **Passes.** Preparing and arming the cycle-counter overflow in one job thread that then spins 3 s on core 0 delivered 29 overflows at a 10 Hz period (6687 IRQs seen by the node in that window), each serviced by the raw node (PMOVSR clear + reload) and chained; release 0; console stable. Arming from a *second* job thread fails with ownership loss (-3): the PMU bank programmed by one thread is not what another thread on the same core sees. Together with the timer-PMU readers' first-tick failures this establishes that the firmware keeps PMU state per thread (`SceExcpmgrExceptionContext` also carries PMCR..PMXEVCNTR5). Answered in two steps. (1) Kernel v35 two-phase probe: 29 overflows while the programming kernel thread spins on core 0, 0 while it sleeps. (2) The `intrmgr`/`threadmgr` RE (see `docs/pmu.md`) showed the PMU register set is saved/restored per thread unconditionally, with PMINTENSET left alone; kernel v45 therefore programs every target thread's saved context (PMCR.E via the ForDriver export, PMCCNTR preload via the ForKernel export, PMCNTENSET.C written into the context block at `[thread_obj+0x34]`, mapping verified by a read-back) and arms PMINTENSET.C once per core. Result: **30 and 32 overflows per 3 s at a 10 Hz period from a user thread pinned to core 0 while the kernel thread sleeps**, the cycle counter enabled at all 6073 user-mode IRQ entries. Per-thread PMU sampling of a target process through the raw IRQ node is validated end to end. Caveats found on the way: a kernel thread spinning at high priority on core 0, or a user thread saturating core 0 without yielding, gets the foreground application suspended by the firmware after about 7 s (all its threads show status 256); a target's main thread blocked inside a syscall for more than a few seconds has the same effect. Threads created after activation still need their contexts programmed (not implemented). The second context block at `[thread_obj+0x38]` is DEAD-filled and unused on these threads. |
| IRQ sampler (`irq_sampling` sample, 10 Hz, core-0 kernel), two earlier runs | **Hard hang, twice.** The second run had the raw node free of threadmgr calls, a count-only mode (no emit) and a report write right after attach: still no report, so the hang is at process start or inside attach, not in the IRQ start. The bring-up's attach on the same kernel build passed. The sample was the only client linking the kernel stub strongly (`--whole-archive`); it now uses the weak import like the bring-up, and with that it attaches without any hang: the strong import of a runtime-loaded syscall library was the launch killer (the automatic CMake integration still links it strongly). After that, `set_sampling_ex(PMU_IRQ|IRQ_REGISTER_ONLY)` never returns even though it arms nothing: kernel-log traces show the `Wakeup` syscall body completing, but neither the client's next printf nor the drain thread's command pickup appear, while the same Submit path works for the bring-up's PMU commands. One such run ended in a hard hang. Original note: No report file was ever written (the sample writes one only after `set_sampling_ex`), the console dropped off the network within seconds (no route to host, not just VitaCompanion) and needed a manual power cycle. The hang is inside attach or `vitaTracySetSampling(PMU_IRQ)`: per-core prepare job, `ksceExcpmgrRegisterHandler` (priority 0, raw node in the RWX section), arm job, or the first PMU/other IRQ entering the raw node. The sample now also writes its report right after attach so the next run can split attach from IRQ start. Review notes (`kernel/sampler_irq.c`): `handle_irq` calls `ksceKernelSetEventFlag`, `ksceKernelGetSystemTimeWide` and the resolved thread-context export from the raw priority-0 node, before Sony's dispatcher runs; kubridge's raw nodes do none of that. |

Side findings: `SCE_SYSMODULE_PERF` still fails to load (`0x805A1000`) and
`PMUSERENR` is 0, as on 2026-08-21. The RWX segment loaded without incident.

## Console session 2026-09-23 (continuation, kernel library versions 46-57)

Same console and setup. The IRQ backend now runs the design validated by the
probes: register the raw IRQ node first, then per core one job thread that
prepares and arms the overflow source, then program every target thread's
saved PMU context from that job (core 0), then (bring-up markers) count.

| Step | Result |
|---|---|
| Real start path, user spinner without yields | 10 overflows/s sustained (82 in 8 s) from a user thread pinned to core 0. |
| Same, spinner yielding every ~1 ms | Overflows stop after the first second although PMCCNTR keeps running: the firmware saves PMOVSR (write-one-to-clear) around the blocking switch inside the yield's syscall and clears it on restore before any IRQ entry sees it. |
| Raw priority-0 **SVC** node added (same entry code, services PMOVSR only) | **Sustained ~9 overflows/s with the yielding spinner** (14, 22, 31, 40, 49, 58, 66 at 1 s steps), no ownership errors. Overflows caught at syscall entry are counted as kernel-side (not attributable to a PC). |
| Overflow seen while the firmware has counters disabled | Previously reported as ownership loss (-3) and stopped the sampler; now ignored until an entry with the cycle counter enabled. |
| GIC SPI 244 (the line the devkit pamgr registered for the PMU) registered with a -1 handler | Targeted at all four cores: the counter froze (consistent with a level line delivered to cores whose node does not service it). Targeted at core 0 only: registration succeeds but no additional interrupts are delivered; retail never configures 244. Left opt-in (`VITA_TRACY_SAMPLING_IRQ_SPI244`), not used. |
| Programming contexts from the syscall thread (core 2) vs from the core-0 job | From the syscall thread the spinner's counter sometimes stayed at the preload; from the job on core 0, where the pinned target threads are switched out, it worked every time. The job does it now. |
| Foreground app suspended ~8 s into the bring-up's count loop | **Still open.** Ruled out one by one: PMU arming, context programming, the spinner thread, screen printing, re-presenting the framebuffer every 100 ms, and the kernel side of `vitaTracyGetStats` (its 8th call completes and returns 0 in the log; the user thread never issues a 9th). The register-only block and the zones loop of the same app do not trigger it. Killing the app afterwards makes the next client crash with `pc=0` (the module's syscall imports no longer bind), so every iteration ends in a reboot. |

Bring-up markers in `ux0:data/` used by `samples/bringup`: `vita_tracy_irq_register_only`,
`vita_tracy_irq_count_only`, `vita_tracy_irq_skip_program`, `vita_tracy_irq_skip_arm`,
`vita_tracy_irq_inten_only`, `vita_tracy_irq_spi244`, `vita_tracy_spin_yield`,
`vita_tracy_no_spinner`, `vita_tracy_quiet`, `vita_tracy_present`. With the IRQ
backend live, `vitaTracyGetStats` fills the never-sampled core-3 slots and two
core-2 PMU slots with IRQ-entry diagnostics (user entries, counter-enabled
entries, PMOVSR sightings, live PMINTENSET/PMCR/PMCNTENSET, cycle deltas, the
watched thread's saved context); the bring-up prints them.
