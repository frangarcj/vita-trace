# vita-tracy

A [Tracy](https://github.com/wolfpld/tracy) profiler port for the PS Vita:
a userland client, a kernel sampling backend, and offline symbolication.

Zones, frames and plots come from the Tracy client linked into the
application. CPU samples, module placements and process events come from an
optional kernel plugin through shared rings, so the hot path never makes a
syscall per event.

> **Status:** early bring-up. It builds and the host test suite passes, but
> nothing has run on a Vita yet. See `docs/implementation.md`.

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

```cpp
#include <tracy/Tracy.hpp>
#include "vita_tracy/client.h"

int main() {
    vita_tracy_init();

    // Optional: without tracy_kernel.skprx this fails and the app keeps
    // producing zones, losing only samples and module metadata.
    if (vita_tracy_kernel_attach(0, 0) == 0) {
        vita_tracy_kernel_set_sampling(500);
    }

    while (running) {
        ZoneScopedN("frame");
        ...
        FrameMark;
    }
}
```

The viewer must be built from the same Tracy commit this repository pins,
since the protocol changes between versions.

To turn sampled addresses back into functions, feed the module messages
from the capture to `tools/symbol_map.py` along with the ELF.

## License

MIT, see `LICENSE`.
