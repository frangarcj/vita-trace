# vita-tracy

A [Tracy](https://github.com/wolfpld/tracy) profiler port for the PS Vita:
a userland client, a kernel sampling backend, and offline symbolication.

Zones, frames and plots come from the Tracy client linked into the
application. CPU samples, module placements and process events come from an
optional kernel plugin through shared rings, so the hot path never makes a
syscall per event.

> **Status:** early bring-up. Zones/frames have been captured on hardware.
> The ABI-2 event-driven bridge and timer-driven suspension diagnostic are
> built and host-tested, but not yet hardware-validated. Non-intrusive PC
> sampling remains unimplemented. See `docs/implementation.md` and
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
harmless. For firmware 3.63 and later, add `-DVITA_TRACY_FIRMWARE=363`.

## Using it

Link `tracy_vita` and instrument with the normal Tracy API:

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

ABI 2 requires reinstalling the rebuilt kernel plugin and application
together. Timer-driven suspension diagnostics require explicit
`VITA_TRACY_SAMPLING_ALLOW_SUSPEND` opt-in; a normal nonzero sampling request
returns unsupported until a real interrupted-PC source is implemented.

## License

MIT, see `LICENSE`.
