# Console validation and bisecting the profiler series

The new PMU/bridge/automatic integration has not been run on a console.
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

ABI 3 requires a matching plugin/client pair. Exported library version 25
is not an exact build identifier. Build both from the tested commit and
replace the provider only while no client is running or using its imports.
The CMake PMU integration uses strong imports and requires the provider at
process launch; it does not support loading it later in the same process.
The bring-up sample retains separate load-only and later attach runs.

Use a recoverable test setup rather than putting an unverified plugin on a
permanent boot path. Host failure-injection tests do not make kernel mistakes
harmless: a console hang or restart remains possible. Save unrelated work.

## Acceptance matrix

Advance only after the preceding layer produces understandable results.

| Layer | Test | What must be observed |
|---|---|---|
| Client only | Enable `FRAMES`, no kernel attach, capture with the pinned Tracy viewer | The original application runs unchanged; successful display submissions are visible; failed display calls do not manufacture frames. |
| Lifecycle | Repeat init, normal return, `exit`, wrapped `sceKernelExitProcess`, repeated launches | No second clock adoption while Tracy is live; no obvious stuck worker or growing resource use. Static constructors/destructors must not emit Tracy events in automatic mode. |
| Mapping/control | Attach without PMU or PC sampling; request stats; checked detach | Clock sync and module maps arrive, commands complete, memory remains valid until confirmed detach. Reattach creates an independent session. |
| Minimal PMU | Explicit cycle-only config, core 0 only, 10 Hz | IRQs land on core 0; `pmu_wrong_cpu` stays zero; actual interval and counters are plausible under alternating busy/idle work. |
| Event PMU | Add the six configured events; compare intentionally different cache/branch workloads | Event labels and selected slots agree, changes are reproducible; renamed instructions are not labeled retired instructions. |
| Multiple cores | Select 0..2 at 100 Hz, pin application workers to known cores | Per-core activity responds to placement. Counts remain whole-core, not falsely assigned to one application thread. CPU 3 is a separate opt-in experiment. |
| Cadence/load | Test 10/100/250 Hz before higher rates; alternate busy/idle; keep clocks recorded | Measured interval distribution and losses are understood. Requested rate is not treated as a guaranteed achieved cadence. |
| Viewer lifecycle | Connect late, disconnect, reconnect during a long session | Deferred module metadata persists, queues remain bounded, losses are visible. Disconnected PMU timers remain active and have their own overhead. |
| PMU conflicts | Start with a different PMU owner present; change configuration during a run | Acquisition refuses occupied registers. Detected ownership loss clears the core's active bit, stops its reader and does not restore over the observed new configuration. Identical third-party programming cannot be detected. |
| Stop/retry | Repeated PMU start/stop and checked detach, including busy/error returns | A failed cleanup retains resources and can be retried. No new session is admitted over retained old resources. IRQ activity stops before mappings disappear. |
| Process death | Terminate a test HB while capture is active, then relaunch | Backend cleanup is coherent, no old callback cleans up a different process, and a later session can attach. A forced kill need not deliver a final network capture. |
| Plugin unload | Only after all clients exit, repeat unload and inspect return values | No callbacks/import calls remain in flight; cancellation/error is handled by retry rather than assuming memory was released. |
| Register preservation | Run an application-side deterministic NEON/VFP workload before/during/after PMU | Checksums/results match. Absence of FP instructions in the plugin alone is not sufficient evidence about the firmware's complete interrupt path. |
| Duration/overhead | Baseline, client without viewer, client with viewer, ring only, then PMU; repeat 30+ minutes | Report overhead as measured distributions and resource deltas, with drops/errors. No performance number is claimed before these runs. |

Useful counters: `pmu_active_mask`, `pmu_last_error`, per-core
`pmu_records`, `pmu_dropped`, `pmu_gaps`, `pmu_wrong_cpu`,
`pmu_counter_errors`, and `last_cleanup_error`. General stats are independent
atomic counters, not a simultaneous cross-core snapshot. PMU records include
the actual interval and sequence. A stopped or failed reader is not zero CPU
work, and aggregate low-32-bit legacy totals are not a wall-clock estimator.

## Keep intrusive diagnostics separate

Ordinary nonzero PC sampling still returns unsupported. Only
`VITA_TRACY_SAMPLING_ALLOW_SUSPEND` requests the provisional suspend/read/
resume diagnostic. It can perturb scheduling and locks or hang the target.
Do not use its percentages to validate the PMU or claim unbiased CPU sampling.

The synthetic workload covers main, pthread, native workers and a sleeping
thread. The explicit registry must exclude only profiler workers. Samples
from the suspension diagnostic carry unknown CPU: ring 0 is transport, not
proof of execution on core 0. A resume failure stops further scans and may
require restarting the process.

## Interrupted-PC adapter: required evidence before implementation

The installed VitaSDK declares:

- `psp2kern/kernel/systimer.h`: a timer callback receives a timer ID and opaque
  user data, not the interrupted register frame.
- `psp2kern/kernel/threadmgr/debugger.h`: `ksceKernelGetThreadContextInfo`
  supplies PID/TID in exception context; `ksceKernelGetThreadCpuRegisters`
  requires a suspended thread. Neither is a demonstrated general timer-PC API.
- `psp2kern/kernel/excpmgr.h`: an exception-context structure and handler
  registration exist, but this header does not provide a corresponding
  unregister API or prove that ordinary system-timer IRQ delivery passes
  through that handler chain with that structure.

Before connecting a PC producer, inspect the actual firmware IRQ entry,
stack/frame construction, dispatch, nesting and return paths. Establish
which saved PC/CPSR/SP/LR belong to the interrupted application, distinguish
user/kernel execution, normalize ARM/Thumb PC semantics, and validate the
same CPU's PID/TID. Establish complete quiescence/unregistration so a plugin
can never be unloaded while its handler can run.

A callback-local PC, a guessed stack offset, or unsuspended debugger-register
reads are not acceptable substitutes. Hooking all exceptions simply because
an enum includes IRQ is not evidence of correct timer context capture.
A future decoder can be host-tested with recorded frames, but the recordings
and hook contract must first be tied to supported firmware. Scheduler events
would complement periodic sampling, not replace it for a long-running thread.

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
