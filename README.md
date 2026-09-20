# vita-tracy

A [Tracy](https://github.com/wolfpld/tracy) profiler port for the PS Vita:
a userland client, a kernel sampling backend, and offline symbolication.

Zones, frames and plots come from the Tracy client linked into the
application. CPU samples, module placements and process events come from an
optional kernel plugin through shared rings, so the hot path never makes a
syscall per event.

> **Status:** early bring-up. Zones/frames have been captured on hardware.
> The ABI-4 event-driven bridge, per-core timer-IRQ PMU capture and automatic
> link-time integration are built and host-tested, but not hardware-validated.
> An experimental PMU-overflow/Excpmgr PC sampler is now built and host-tested
> too; it remains explicit opt-in until its IRQ routing and saved-PC semantics
> are verified on a console. See `docs/implementation.md` and
> [sampling modes and limitations](docs/sampling.md).

## Layout

- `client/` — Vita platform port of the Tracy client (`libtracy_vita.a`).
- `kernel/` — `tracy_kernel.skprx`, the privileged sampling and metadata backend.
- `common/` — code compiled into both sides: the shared rings and their layout.
- `include/vita_tracy/` — the ABI between them.
- `samples/` — zones, synthetic sampling, and PMU cache/branch workloads.
- `agent/` — optional agent for profiling uninstrumented apps (not started).
- `tools/` — offline symbolication.
- `tests/` — host-native unit tests (doctest).
- `docs/` — status, firmware compatibility, reverse-engineering notes.
- `patches/tracy/` — patches applied to the pinned Tracy submodule.

## Building

Needs [VitaSDK](https://vitasdk.org) with `$VITASDK` set, and a host
compiler for the tests.

```sh
git submodule update --init --recursive

# Vita targets: client, kernel plugin, samples
cmake -B build
cmake --build build

# Host-native tests
cmake -B build-host -DVITA_TRACY_BUILD_HOST_TESTS=ON
cmake --build build-host
ctest --test-dir build-host
```

Configure applies `patches/tracy/` to the submodule; re-running is
harmless. The kernel requires taiHEN ModuleUtils and resolves its known 3.60
and 3.63+ exports at startup; no per-firmware build is needed for those lookups.
See [firmware compatibility](docs/firmware_compat.md). This is not a claim of
hardware validation. The raw IRQ entry also has optional instruction-level
tests using Unicorn; see [the kubridge review](docs/kubridge-review.md).

## Using it

For an existing executable, enable the client without editing its source:

```cmake
add_subdirectory(path/to/vita-trace)
vita_tracy_enable(my_homebrew FRAMES)
```

Add `PMU` to attach the backend and collect whole-core counters automatically:

```cmake
vita_tracy_enable(my_homebrew PMU FRAMES PMU_HZ 100 CORE_MASK 7)
```

These are alternative calls, not two calls for the same target. **The PMU
mode requires the matching kernel plugin loaded before launching the HB.**
It links strong imports and never probes an unresolved weak import. `FRAMES`
marks successful display submissions, not GPU duration. See
[automatic integration](docs/automatic.md) for deployment and lifetime
limits. PMU plots are whole-core counters, not function-level samples.

An experimental function-PC alternative is
`vita_tracy_enable(my_homebrew PC_SAMPLING FRAMES SAMPLE_HZ 100)`.
It also requires the matching plugin at launch, cannot be combined with `PMU`,
and pins the plugin resident until reboot after successful IRQ registration.
Use `-DVITA_TRACY_IRQ_CORE_MASK=1` when building the kernel for initial core-0
validation. This path has not been validated on hardware.

For manual lifetime and zones, link `tracy_vita` and use the normal Tracy API:

When included with `add_subdirectory`, sample applications default to off:

```cmake
add_subdirectory(path/to/vita-trace)
target_link_libraries(my_homebrew PRIVATE tracy_vita)
```

```cpp
#include <tracy/Tracy.hpp>
#include "vita_tracy/client.h"

int main() {
    vita_tracy_init();

    // Zones do not require a plugin. Call vita_tracy_kernel_attach only
    // after the matching plugin is known to be loaded: weak imports are
    // NOT a safe residency probe. See docs/sampling.md for diagnostics.

    while (running) {
        ZoneScopedN("frame");
        ...
        FrameMark;
    }
    vita_tracy_shutdown();
}
```

The viewer must be built from the same Tracy commit this repository pins,
since the protocol changes between versions.

To turn sampled addresses back into functions, feed the module messages
from the capture to `tools/symbol_map.py` along with the ELF.
Use `--elf ModuleName=path` for each module in a multi-module capture. The
tool reconstructs ELF virtual addresses from runtime segment placements;
it does not assume the executable was linked at zero.

ABI 4 requires reinstalling the rebuilt kernel plugin and application
together. Timer-driven suspension diagnostics require explicit
`VITA_TRACY_SAMPLING_ALLOW_SUSPEND` opt-in. Experimental non-intrusive
sampling requires `VITA_TRACY_SAMPLING_PMU_IRQ`; a normal nonzero request
still returns unsupported rather than silently selecting an unverified backend.
See [hardware validation and bisecting](docs/hardware-validation.md) before
testing the new kernel paths on a console.

## License

MIT, see `LICENSE`.
